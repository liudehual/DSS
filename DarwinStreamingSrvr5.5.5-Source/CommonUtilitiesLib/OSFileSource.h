/*
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 *
 */
/*
    File:       osfilesource.h

    Contains:   simple file abstraction. This file abstraction is ONLY to be
                used for files intended for serving 
                    
    
*/

#ifndef __OSFILE_H_
#define __OSFILE_H_

#include <stdio.h>
#include <time.h>

#include "OSHeaders.h"
#include "StrPtrLen.h"
#include "OSQueue.h"

#define READ_LOG 0

class FileBlockBuffer 
{

 public:
    FileBlockBuffer(): fArrayIndex(-1),fBufferSize(0),fBufferFillSize(0),fDataBuffer(NULL),fDummy(0){}
    ~FileBlockBuffer(void);
    void AllocateBuffer(UInt32 buffSize);
    void TestBuffer(void);
    void CleanBuffer() { ::memset(fDataBuffer,0, fBufferSize); }
    void SetFillSize(UInt32 fillSize) {fBufferFillSize = fillSize;}
    UInt32 GetFillSize(void) { return fBufferFillSize;}
    OSQueueElem *GetQElem() { return &fQElem; }
    SInt64              fArrayIndex;
    UInt32              fBufferSize;
    UInt32              fBufferFillSize;
    char                *fDataBuffer;
    OSQueueElem         fQElem;
    UInt32              fDummy;
};



class FileBlockPool
{
    enum {
            kDataBufferUnitSizeExp      = 15,// base 2 exponent
            kBufferUnitSize             = (1 << kDataBufferUnitSizeExp ) // 32Kbytes
    };

    public:
        FileBlockPool(void) :  fMaxBuffers(1),  fNumCurrentBuffers(0), fBufferUnitSizeBytes(kBufferUnitSize){}
        ~FileBlockPool(void);
        
        void SetMaxBuffers(UInt32 maxBuffers) { if (maxBuffers > 0) fMaxBuffers = maxBuffers; }

        void SetBuffIncValue(UInt32 bufferInc) { if (bufferInc > 0) fBufferInc = bufferInc;}
        void IncMaxBuffers(void) { fMaxBuffers += fBufferInc; }
        void DecMaxBuffers(void) { if (fMaxBuffers > fBufferInc) fMaxBuffers-= fBufferInc; }
        void DecCurBuffers(void) { if (fNumCurrentBuffers > 0) fNumCurrentBuffers--; }
        
        void SetBufferUnitSize  (UInt32 inUnitSizeInK)      { fBufferUnitSizeBytes = inUnitSizeInK * 1024; }
        UInt32 GetBufferUnitSizeBytes()     { return fBufferUnitSizeBytes; }
        UInt32 GetMaxBuffers(void)  { return fMaxBuffers; }
        UInt32 GetIncBuffers()      { return fBufferInc; }
        UInt32 GetNumCurrentBuffers(void)   { return fNumCurrentBuffers; }
        void DeleteBlockPool();
        FileBlockBuffer* GetBufferElement(UInt32 bufferSizeBytes);
        void MarkUsed(FileBlockBuffer* inBuffPtr);

    private:
        OSQueue fQueue;
        UInt32  fMaxBuffers;
        UInt32  fNumCurrentBuffers; 
        UInt32  fBufferInc; 
		/* 缓冲单位大小字节 */
        UInt32  fBufferUnitSizeBytes;
        UInt32  fBufferDataSizeBytes;

};

class FileMap 
{

    public:
        FileMap(void):fFileMapArray(NULL),fDataBufferSize(0),fMapArraySize(0),fNumBuffSizeUnits(0) {}
        ~FileMap(void) {fFileMapArray = NULL;}
        void    AllocateBufferMap(UInt32 inUnitSizeInK, UInt32 inNumBuffSizeUnits, UInt32 inBufferIncCount, UInt32 inMaxBitRateBuffSizeInBlocks, UInt64 fileLen, UInt32 inBitRate);
        char*   GetBuffer(SInt64 bufIndex, Bool16 *outIsEmptyBuff);
        void    TestBuffer(SInt32 bufIndex) {Assert (bufIndex >= 0); fFileMapArray[bufIndex]->TestBuffer();};
        void    SetIndexBuffFillSize(SInt32 bufIndex, UInt32 fillSize) { Assert (bufIndex >= 0); fFileMapArray[bufIndex]->SetFillSize(fillSize);}
        UInt32  GetMaxBufSize(void) {
						//qtss_printf("----- fDataBufferSize %ld \n",fDataBufferSize);
						return fDataBufferSize;
		}
        UInt32  GetBuffSize(SInt64 bufIndex)    { Assert (bufIndex >= 0); return fFileMapArray[bufIndex]->GetFillSize(); }
        UInt32  GetIncBuffers(void) { return fBlockPool.GetIncBuffers(); }
        void    IncMaxBuffers()     {fBlockPool.IncMaxBuffers(); }
        void    DecMaxBuffers()     {fBlockPool.DecMaxBuffers(); }
        Bool16  Initialized()       { return fFileMapArray == NULL ? false : true; }
        void    Clean(void);
        void    DeleteMap(void);
        void    DeleteOldBuffs(void);
        SInt64  GetBuffIndex(UInt64 inPosition) {   return inPosition / this->GetMaxBufSize();  }
        SInt64  GetMaxBuffIndex() { Assert(fMapArraySize > 0); return fMapArraySize -1; }
        UInt64  GetBuffOffset(SInt64 bufIndex) { return (UInt64) (bufIndex * this->GetMaxBufSize() ); }

		/* 文件块内存池 */
		FileBlockPool fBlockPool;
		/* 正在使用的 FileBlockBuffer */
        FileBlockBuffer**   fFileMapArray; /*  */
    
    private:

    UInt32              fDataBufferSize;
    SInt64              fMapArraySize;
    UInt32              fNumBuffSizeUnits;
    
};

class OSFileSource
{
    public:
    
        OSFileSource() :    fFile(-1), fLength(0), fPosition(0), fReadPos(0), fShouldClose(true), fIsDir(false), fCacheEnabled(false)                       
        {
        
        #if READ_LOG 
            fFileLog = NULL;
            fTrackID = 0;
            fFilePath[0]=0;
        #endif
        
        }
                
        OSFileSource(const char *inPath) :  fFile(-1), fLength(0), fPosition(0), fReadPos(0), fShouldClose(true), fIsDir(false),fCacheEnabled(false)
        {
         Set(inPath); 
         
        #if READ_LOG 
            fFileLog = NULL;
            fTrackID = 0;
            fFilePath[0]=0;
        #endif      
         
         }
        
        ~OSFileSource() { Close();  fFileMap.DeleteMap();}

		/* 设置 OSFileSource 基本信息 */
        //Sets this object to reference this file
        void            Set(const char *inPath);
        
        // Call this if you don't want Close or the destructor to close the fd
        void            DontCloseFD() { fShouldClose = false; }
        
        //Advise: this advises the OS that we are going to be reading soon from the
        //following position in the file
        void            Advise(UInt64 advisePos, UInt32 adviseAmt);

		/* 硬盘读取封装函数 */
        OS_Error    Read(void* inBuffer, UInt32 inLength, UInt32* outRcvLen = NULL)
                    {   return ReadFromDisk(inBuffer, inLength, outRcvLen);
                    }
        /* 从指定位置读取数据 */           
        OS_Error    Read(UInt64 inPosition, void* inBuffer, UInt32 inLength, UInt32* outRcvLen = NULL);

		/* 从硬盘读取数据 */
		OS_Error    ReadFromDisk(void* inBuffer, UInt32 inLength, UInt32* outRcvLen = NULL);

		/* 从内存读取数据 */
		OS_Error    ReadFromCache(UInt64 inPosition, void* inBuffer, UInt32 inLength, UInt32* outRcvLen = NULL);

		/* 从指定位置读取数据 */
		OS_Error    ReadFromPos(UInt64 inPosition, void* inBuffer, UInt32 inLength, UInt32* outRcvLen = NULL);
		/* 启动从缓冲区获取数据功能 */
		void        EnableFileCache(Bool16 enabled) {OSMutexLocker locker(&fMutex); fCacheEnabled = enabled; }
		/* 获取缓冲区使能 */
		Bool16      GetCacheEnabled() { return fCacheEnabled; }
		/* 申请缓冲区 */
		void        AllocateFileCache(UInt32 inUnitSizeInK = 32, UInt32 bufferSizeUnits = 0, UInt32 incBuffers = 1, UInt32 inMaxBitRateBuffSizeInBlocks = 8, UInt32 inBitRate = 32768) 
                    {   fFileMap.AllocateBufferMap(inUnitSizeInK, bufferSizeUnits,incBuffers, inMaxBitRateBuffSizeInBlocks, fLength, inBitRate);
                    } 
		/* 增加最大内存 */
        void        IncMaxBuffers()     {OSMutexLocker locker(&fMutex); fFileMap.IncMaxBuffers(); }
		/* 减小最大内存 */
		void        DecMaxBuffers()     {OSMutexLocker locker(&fMutex); fFileMap.DecMaxBuffers(); }

		/* 填充缓冲区 */
        OS_Error    FillBuffer(char* ioBuffer, char *buffStart, SInt32 bufIndex);
                
        void            Close();
		/* 获取文件最后修改时间 */
        time_t          GetModDate()                { return fModDate; }
		/* 获取文件长度 */
        UInt64          GetLength()                 { return fLength; }
		/* 获取游标位置 */
        UInt64          GetCurOffset()              { return fPosition; }
        void            Seek(SInt64 newPosition)    { fPosition = newPosition;  }

		/* 检查文件描述符是否可用 */
        Bool16 IsValid()                            { return fFile != -1;       }
		/* 检查是否是目录 */
        Bool16 IsDir()                              { return fIsDir; }

		/* 获取文件好 */
        // For async I/O purposes
        int             GetFD()                     { return fFile; }
		/* 设置轨道号 */
        void            SetTrackID(UInt32 trackID);
        // So that close won't do anything
        void ResetFD()  { fFile=-1; }

		/* 设置log */
        void SetLog(const char *inPath);
    
    private:

        int     fFile;
        UInt64  fLength;
        UInt64  fPosition;
        UInt64  fReadPos;
        Bool16  fShouldClose;
        Bool16  fIsDir;
        time_t  fModDate;
        
        
        OSMutex fMutex;
        FileMap fFileMap;
        Bool16  fCacheEnabled;
#if READ_LOG
        FILE*               fFileLog;
        char                fFilePath[1024];
        UInt32              fTrackID;
#endif

};

#endif //__OSFILE_H_
