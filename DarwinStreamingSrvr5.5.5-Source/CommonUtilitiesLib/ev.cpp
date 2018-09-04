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
    File:       ev.cpp

    Contains:   POSIX select implementation of MacOS X event queue functions.


    

*/

#define EV_DEBUGGING 0 //Enables a lot of printfs

    #include <sys/time.h>
    #include <sys/types.h>

#ifndef __MACOS__
#ifndef __hpux__
    #include <sys/select.h>
#endif
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/errno.h>

#include "ev.h"
#include "OS.h"
#include "OSHeaders.h"
#include "MyAssert.h"
#include "OSThread.h"
#include "OSMutex.h"

static fd_set   sReadSet;
static fd_set   sWriteSet;
static fd_set   sReturnedReadSet;
static fd_set   sReturnedWriteSet;
static void**   sCookieArray = NULL;
static int*     sFDsToCloseArray = NULL;
static int sPipes[2];

static int sCurrentFDPos = 0;
/**************************************
*
* sMaxFDPos==集合中的最大描述符 + 1
*
***************************************/

static int sMaxFDPos = 0;

/**************************************
*
* 是否是读事件集合
*
***************************************/
static bool sInReadSet = true;

/**************************************
*
* 已经准备好的描述符数
*
***************************************/
static int sNumFDsBackFromSelect = 0;

/**************************************
*
* 已经处理过的描述符数
*
***************************************/
static UInt32 sNumFDsProcessed = 0;

/**************************************
*
* 描述符锁
*
***************************************/
static OSMutex sMaxFDPosMutex;


static bool selecthasdata();
static int constructeventreq(struct eventreq* req, int fd, int event);


void select_startevents()
{	
	/******************************************
	*
	* 清空文件描述符集合
	*
	******************************************/
    FD_ZERO(&sReadSet);
    FD_ZERO(&sWriteSet);
    FD_ZERO(&sReturnedReadSet);
    FD_ZERO(&sReturnedWriteSet);

	/******************************************************
	*
	* 申请文件描述符集合内存	
	* sizeof(fd_set)==128
	*
	*******************************************************/
    //We need to associate cookies (void*)'s with our file descriptors.
    //We do so by storing cookies in this cookie array. Because an fd_set is
    //a big array of bits, we should have as many entries in the array as
    //there are bits in the fd set  
    sCookieArray = new void*[sizeof(fd_set) * 8];
    ::memset(sCookieArray, 0, sizeof(void *) * sizeof(fd_set) * 8);
    
    //We need to close all fds from the select thread. Once an fd is passed into
    //removeevent, its added to this array so it may be deleted from the select thread
    sFDsToCloseArray = new int[sizeof(fd_set) * 8];
    for (int i = 0; i < (int) (sizeof(fd_set) * 8); i++)
        sFDsToCloseArray[i] = -1;
    
    //We need to wakeup select when the masks have changed. In order to do this,
    //we create a pipe that gets written to from modwatch, and read when select returns
    int theErr = ::pipe((int*)&sPipes);
    Assert(theErr == 0);

	/***************************************************
	*
	* 将管道读端添加到可读文件描述符集合
	*
	****************************************************/
    //Add the read end of the pipe to the read mask
    FD_SET(sPipes[0], &sReadSet);
    sMaxFDPos = sPipes[0];
}

int select_removeevent(int which)
{

    {
        //Manipulating sMaxFDPos is not pre-emptive safe, so we have to wrap it in a mutex
        //I believe this is the only variable that is not preemptive safe....
        OSMutexLocker locker(&sMaxFDPosMutex);
        
    //Clear this fd out of both sets
        FD_CLR(which, &sWriteSet);
        FD_CLR(which, &sReadSet);
        
        FD_CLR(which, &sReturnedReadSet);
        FD_CLR(which, &sReturnedWriteSet);
    
        sCookieArray[which] = NULL; // Clear out the cookie
        
        if (which == sMaxFDPos)
        {
            //We've just deleted the highest numbered fd in our set,
            //so we need to recompute what the highest one is.
            while (!FD_ISSET(sMaxFDPos, &sReadSet) && !FD_ISSET(sMaxFDPos, &sWriteSet) &&
                (sMaxFDPos > 0))
                {
                    sMaxFDPos--;
                }
        }

        //We also need to keep the mutex locked during any manipulation of the
        //sFDsToCloseArray, because it's definitely not preemptive safe.
            
        //put this fd into the fd's to close array, so that when select wakes up, it will
        //close the fd
        UInt32 theIndex = 0;
        while ((sFDsToCloseArray[theIndex] != -1) && (theIndex < sizeof(fd_set) * 8))
            theIndex++;
        Assert(sFDsToCloseArray[theIndex] == -1);
        sFDsToCloseArray[theIndex] = which;
    }
    
    //write to the pipe so that select wakes up and registers the new mask
    int theErr = ::write(sPipes[1], "p", 1);
    Assert(theErr == 1);

    return 0;
}

int select_watchevent(struct eventreq *req, int which)
{
    return select_modwatch(req, which);
}

int select_modwatch(struct eventreq *req, int which)
{
    {
        //Manipulating sMaxFDPos is not pre-emptive safe, so we have to wrap it in a mutex
        //I believe this is the only variable that is not preemptive safe....
        OSMutexLocker locker(&sMaxFDPosMutex);

		/*******************************************************************
		*
		* 判断事件类型，并加入到相应的文件描述符集合
		*
		********************************************************************/
        //Add or remove this fd from the specified sets
        if (which & EV_RE)
        {
            FD_SET(req->er_handle, &sReadSet);
        }else{
            FD_CLR(req->er_handle, &sReadSet);
        }
		
        if (which & EV_WR){
            FD_SET(req->er_handle, &sWriteSet);
        }else{
            FD_CLR(req->er_handle, &sWriteSet);
        }

		/*********************************************************************
		*
		* 如果当前socket fd 大于 当前最大的 socket fd
		* 将当前socket fd 赋值给 sMaxFDPos
		* 如此设置是select 的需要
		* 
		**********************************************************************/
        if (req->er_handle > sMaxFDPos){
            sMaxFDPos = req->er_handle;
		}

        /******************************************************************************************
        *
        * 1.@see: EventContext::RequestEvent():242行
        *		//fill out the eventreq data structure
        *		::memset(&fEventReq, '\0', sizeof(fEventReq));
        *		fEventReq.er_type = EV_FD;
        *		fEventReq.er_handle = fFileDesc;
        *		fEventReq.er_eventbits = theMask;
        *		fEventReq.er_data = (void*)fUniqueID;
        * 2.将文件描述符 fd 与 socket 唯一ID号关联
        *
        * 3.事件产生后，通过fd找到ID号，然后再通过ID号找到socket对象，并调用ProcessEvent()函数将
        *   fTask添加到TaskThread中
        *
        *******************************************************************************************/
        // Also, modifying the cookie is not preemptive safe. This must be
        // done atomically wrt setting the fd in the set. Otherwise, it is
        // possible to have a NULL cookie on a fd.
        Assert(req->er_handle < (int)(sizeof(fd_set) * 8));
        Assert(req->er_data != NULL);
        sCookieArray[req->er_handle] = req->er_data;
    }
	
    /***************************************************************
    *
    * 向管道写入数据，激活 select 函数 注册新的掩码(新的socket fd )
    *
    ***************************************************************/
    //write to the pipe so that select wakes up and registers the new mask
    int theErr = ::write(sPipes[1], "p", 1);
    Assert(theErr == 1);

    return 0;
}

int constructeventreq(struct eventreq* req, int fd, int event)
{
	/********************************************
	*
	* 记录事件及描述符、处理句柄
	*
	*********************************************/
    req->er_handle = fd;
    req->er_eventbits = event;
    Assert(fd < (int)(sizeof(fd_set) * 8));
	req->er_data = sCookieArray[fd];
	
	/*********************************************
	*
	* 增加描述符索引位置及已处理过的描述符数量
	*
	**********************************************/
    sCurrentFDPos++;
    sNumFDsProcessed++;

	/*********************************************
	*
	* 将描述符从集合中清除
	*
	*********************************************/
    //don't want events on this fd until modwatch is called.
    FD_CLR(fd, &sWriteSet);
    FD_CLR(fd, &sReadSet);
    
    return 0;
}

int select_waitevent(struct eventreq *req, void* /*onlyForMacOSX*/)
{
    //Check to see if we still have some select descriptors to process
    int theFDsProcessed = (int)sNumFDsProcessed;
    bool isSet = false;

	/*************************************************************
	*
	* 处理过的描述符数量小于select 返回值，没有处理完成，接着处理
	*
	**************************************************************/
    if (theFDsProcessed < sNumFDsBackFromSelect){

		/********************************************
		*
		* 读事件描述符集合
		*
		*********************************************/
        if (sInReadSet){
			
        	/******************************************************************
        	*
        	* 加锁，防止增删改查，改变数据
        	*
        	*******************************************************************/
            OSMutexLocker locker(&sMaxFDPosMutex);
			
			/***************************************************************************************************
			*
			* 获取所有有效的文件描述符
			* FD_ISSET 返回 0 表示 sCurrentFDPos 不在 sReturnedReadSet 中
			* FD_ISSET 返回 1 表示 sCurrentFDPos 在 sReturnedReadSet 中
			* 以下表达式的逻辑是  FD_ISSET 返回1 且 sCurrentFDPos < sMaxFDPos，找到了期望的文件描述符并跳出循环
			*
			*****************************************************************************************************/
            while((!(isSet = FD_ISSET(sCurrentFDPos, &sReturnedReadSet))) && (sCurrentFDPos < sMaxFDPos)){ 
                sCurrentFDPos++;        
			}
			/*******************************************************
			* 
			* sCurrentFDPos 在可读文件描述集合中
			*
			********************************************************/
            if (isSet){   
				/***************************************************
				*
				* 先将 sCurrentFDPos 从  sReturnedReadSet 集合中清除
				*
				****************************************************/
                FD_CLR(sCurrentFDPos, &sReturnedReadSet);
				/***************************************************
				* 
				* 创建事件请求对象并返回
				*
				***************************************************/
                return constructeventreq(req, sCurrentFDPos, EV_RE);
            }else{
				/***************************************************
				* 
				* 不在可读文件描述集合中，sInReadSet=false
				* 并重置 sCurrentFDPos=0
				*
				***************************************************/

                sInReadSet = false;
                sCurrentFDPos = 0;
            }
        }
		/********************************************
		*
		* 写事件描述符集合
		*
		*********************************************/

		if (!sInReadSet){
            OSMutexLocker locker(&sMaxFDPosMutex);

			/***************************************************************************************************
			*
			* 获取所有有效的文件描述符
			* FD_ISSET 返回 0 表示 sCurrentFDPos 不在 sReturnedReadSet 中
			* FD_ISSET 返回 1 表示 sCurrentFDPos 在 sReturnedReadSet 中
			* 以下表达式的逻辑是  FD_ISSET 返回1 且 sCurrentFDPos < sMaxFDPos，找到了期望的文件描述符并跳出循环
			*
			*****************************************************************************************************/
            while((!(isSet = FD_ISSET(sCurrentFDPos, &sReturnedWriteSet))) && (sCurrentFDPos < sMaxFDPos)){
                sCurrentFDPos++;
			}
			
			/*******************************************************
			* 
			* sCurrentFDPos 在可写文件描述集合中
			*
			********************************************************/
            if (isSet){
				
				/***************************************************
				*
				* 先将 sCurrentFDPos 从  sReturnedReadSet 集合中清除
				*
				****************************************************/
                FD_CLR(sCurrentFDPos, &sReturnedWriteSet);
				
				/***************************************************
				* 
				* 创建事件请求对象并返回
				*
				***************************************************/
                return constructeventreq(req, sCurrentFDPos, EV_WR);
            }else{
                // This can happen if another thread calls select_removeevent at just the right
                // time, setting sMaxFDPos lower than it was when select() was last called.
                // Becase sMaxFDPos is used as the place to stop iterating over the read & write
                // masks, setting it lower can cause file descriptors in the mask to get skipped.
                // If they are skipped, that's ok, because those file descriptors were removed
                // by select_removeevent anyway. We need to make sure to finish iterating over
                // the masks and call select again, which is why we set sNumFDsProcessed
                // artificially here.
                sNumFDsProcessed = sNumFDsBackFromSelect;
                Assert(sNumFDsBackFromSelect > 0);
            }
        }
    }
	
	/*******************************************************
	* 
	* 已经处理过的描述符>0 ,重置部分变量
	*
	********************************************************/
    if (sNumFDsProcessed > 0){
        OSMutexLocker locker(&sMaxFDPosMutex);
        //We've just cycled through one select result. Re-init all the counting states
        sNumFDsProcessed = 0;
        sNumFDsBackFromSelect = 0;
        sCurrentFDPos = 0;
        sInReadSet = true;
    }
    
    while(!selecthasdata()){
		
        {
            OSMutexLocker locker(&sMaxFDPosMutex);
            //Prepare to call select. Preserve the read and write sets by copying their contents
            //into the corresponding "returned" versions, and then pass those into select
            ::memcpy(&sReturnedReadSet, &sReadSet, sizeof(fd_set));
            ::memcpy(&sReturnedWriteSet, &sWriteSet, sizeof(fd_set));
        }

        SInt64  yieldDur = 0;
        SInt64  yieldStart;
        
        //Periodically time out the select call just in case we
        //are deaf for some reason
        // on platforw's where our threading is non-preemptive, just poll select

        struct timeval  tv;
        tv.tv_usec = 0;

    #if THREADING_IS_COOPERATIVE
        tv.tv_sec = 0;
        
        if ( yieldDur > 4 )
            tv.tv_usec = 0;
        else
            tv.tv_usec = 5000;
    #else
        tv.tv_sec = 15;
    #endif

        yieldStart = OS::Milliseconds();
        OSThread::ThreadYield();
        
        yieldDur = OS::Milliseconds() - yieldStart;
		/******************************************************************
		*
		* 等待网络事件和超时事件
		*
		*******************************************************************/
        sNumFDsBackFromSelect = ::select(sMaxFDPos+1, &sReturnedReadSet, &sReturnedWriteSet, NULL, &tv);

    }
    

    if (sNumFDsBackFromSelect >= 0)
        return EINTR;   //either we've timed out or gotten some events. Either way, force caller
                        //to call waitevent again.
    return sNumFDsBackFromSelect;
}

bool selecthasdata()
{
	/**********************************************************
	*
	* select 返回值<0,判断出错状态
	*
	***********************************************************/
    if (sNumFDsBackFromSelect < 0)
    {
        int err=OSThread::GetErrno();
        
        if ( 
#if __solaris__
            err == ENOENT || // this happens on Solaris when an HTTP fd is closed
#endif      
            err == EBADF || //this might happen if a fd is closed right before calling select
            err == EINTR 
           ) // this might happen if select gets interrupted
             return false;
        return true;//if there is an error from select, we want to make sure and return to the caller
    }
	
    /*********************************************************
    *
    * select 返回值==0，产生超时事件，返回
    *
    **********************************************************/    
    if (sNumFDsBackFromSelect == 0)
        return false;//if select returns 0, we've simply timed out, so recall select

	/*******************************************************
	*
	* 管道被激活
	*
	********************************************************/
    if (FD_ISSET(sPipes[0], &sReturnedReadSet))
    {
        //we've gotten data on the pipe file descriptor. Clear the data.
        // increasing the select buffer fixes a hanging problem when the Darwin server is under heavy load
        // CISCO contribution
        char theBuffer[4096]; 
        (void)::read(sPipes[0], &theBuffer[0], 4096);

        FD_CLR(sPipes[0], &sReturnedReadSet);
        sNumFDsBackFromSelect--;

		/********************************************************
		*
		* 遍历FD 关闭数组
		*
		*
		*********************************************************/
        {
            //Check the fds to close array, and if there are any in it, close those descriptors
            OSMutexLocker locker(&sMaxFDPosMutex);
            for (UInt32 theIndex = 0; ((sFDsToCloseArray[theIndex] != -1) && (theIndex < sizeof(fd_set) * 8)); theIndex++)
            {
                (void)::close(sFDsToCloseArray[theIndex]);
                sFDsToCloseArray[theIndex] = -1;
            }
        }
    }
    Assert(!FD_ISSET(sPipes[0], &sReturnedWriteSet));

	/*********************************************
	* 
	* 只有 管道文件描述符 返回，重新调用select
	*
	**********************************************/
    if (sNumFDsBackFromSelect == 0){
        return false;//if the pipe file descriptor is the ONLY data we've gotten, recall select
    }else{
    	/************************************
    	*
    	* 获取一个真实的事件，返回处理
    	*
    	*************************************/
        return true;//we've gotten a real event, return that to the caller
    }
}

