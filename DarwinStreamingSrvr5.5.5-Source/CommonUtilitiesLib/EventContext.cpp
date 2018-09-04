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
    File:       EventContext.cpp

    Contains:   Impelments object in .h file
                    
    
    
*/

#include "EventContext.h"
#include "OSThread.h"
#include "atomic.h"

#include <fcntl.h>
#include <errno.h>

#ifndef __Win32__
#include <unistd.h>
#endif

#if MACOSXEVENTQUEUE
#include "tempcalls.h" //includes MacOS X prototypes of event queue functions
#endif

#define EVENT_CONTEXT_DEBUG 0

#if EVENT_CONTEXT_DEBUG
#include "OS.h"
#endif

#ifdef __Win32__
unsigned int EventContext::sUniqueID = WM_USER; // See commentary in RequestEvent
#else
unsigned int EventContext::sUniqueID = 1;
#endif

EventContext::EventContext(int inFileDesc, EventThread* inThread)
:   fFileDesc(inFileDesc),
    fUniqueID(0),
    fUniqueIDStr((char*)&fUniqueID, sizeof(fUniqueID)),
    fEventThread(inThread),
    fWatchEventCalled(false),
    fAutoCleanup(true)
{}


void EventContext::InitNonBlocking(int inFileDesc)
{
    fFileDesc = inFileDesc;
    /****************************************************
    *
    * 设置socket为非阻塞
    *
    *
    *****************************************************/
#ifdef __Win32__
    u_long one = 1;
    int err = ::ioctlsocket(fFileDesc, FIONBIO, &one);
#else
    int flag = ::fcntl(fFileDesc, F_GETFL, 0);
    int err = ::fcntl(fFileDesc, F_SETFL, flag | O_NONBLOCK);
#endif
    AssertV(err == 0, OSThread::GetErrno());
}

void EventContext::Cleanup()
{
    int err = 0;

	/****************************************************
	*
	* 清理事件环境
	*
	*****************************************************/
    if (fFileDesc != kInvalidFileDesc)
    {
        //if this object is registered in the table, unregister it now
        if (fUniqueID > 0)
        {
            fEventThread->fRefTable.UnRegister(&fRef);

#if !MACOSXEVENTQUEUE
            select_removeevent(fFileDesc);//The eventqueue / select shim requires this
#ifdef __Win32__
            err = ::closesocket(fFileDesc);
#endif

#else
            //On Linux (possibly other UNIX implementations) you MUST NOT close the fd before
            //removing the fd from the select mask, and having the select function wake up
            //to register this fact. If you close the fd first, bad things may happen, like
            //the socket not getting unbound from the port & IP addr.
            //
            //So, what we do is have the select thread itself call close. This is triggered
            //by calling removeevent.
            err = ::close(fFileDesc);
#endif      
        }
        else
#ifdef __Win32__
            err = ::closesocket(fFileDesc);
#else
            err = ::close(fFileDesc);
#endif
    }

    fFileDesc = kInvalidFileDesc;
    fUniqueID = 0;
    
    AssertV(err == 0, OSThread::GetErrno());//we don't really care if there was an error, but it's nice to know
}


void EventContext::SnarfEventContext( EventContext &fromContext )
{  
    //+ show that we called watchevent
    // copy the unique id
    // set our fUniqueIDStr to the unique id
    // copy the eventreq
    // find the old event object
    // show us as the object in the fRefTable
    //      we take the OSRef from the old context, point it at our context
    //
    //TODO - this whole operation causes a race condition for Event posting
    //  way up the chain we need to disable event posting
    // or copy the posted events afer this op completes
    
    fromContext.fFileDesc = kInvalidFileDesc;
    
    fWatchEventCalled = fromContext.fWatchEventCalled; 
    fUniqueID = fromContext.fUniqueID;
    fUniqueIDStr.Set((char*)&fUniqueID, sizeof(fUniqueID)),
    
    ::memcpy( &fEventReq, &fromContext.fEventReq, sizeof( struct eventreq  ) );

    fRef.Set( fUniqueIDStr, this );
    fEventThread->fRefTable.Swap(&fRef);
    fEventThread->fRefTable.UnRegister(&fromContext.fRef);
}

void EventContext::RequestEvent(int theMask)
{
	/*************************************************
	*
	* 此处一般设置为EV_RE(读事件)
	*
	*
	**************************************************/
    //
    // The first time this function gets called, we're supposed to
    // call watchevent. Each subsequent time, call modwatch. That's
    // the way the MacOS X event queue works.
    
    if (fWatchEventCalled)
    {
        fEventReq.er_eventbits = theMask;
#if MACOSXEVENTQUEUE
        if (modwatch(&fEventReq, theMask) != 0)
#else
        if (select_modwatch(&fEventReq, theMask) != 0)
#endif  
            AssertV(false, OSThread::GetErrno());
    }
    else
    {
    	/********************************************************************
    	*
    	* 为该socket申请一个唯一ID号
    	*
   	 	*********************************************************************/
        //allocate a Unique ID for this socket, and add it to the ref table
        
#ifdef __Win32__
        //
        // Kind of a hack. On Win32, the way that we pass around the unique ID is
        // by making it the message ID of our Win32 message (see win32ev.cpp).
        // Messages must be >= WM_USER. Hence this code to restrict the numberspace
        // of our UniqueIDs. 
        if (!compare_and_store(8192, WM_USER, &sUniqueID))  // Fix 2466667: message IDs above a
            fUniqueID = (PointerSizedInt)atomic_add(&sUniqueID, 1);         // level are ignored, so wrap at 8192
        else
            fUniqueID = (PointerSizedInt)WM_USER;
#else
		/********************************************************
		*
		*
		*
		*********************************************************/
        if (!compare_and_store(10000000, 1, &sUniqueID))
            fUniqueID = (PointerSizedInt)atomic_add(&sUniqueID, 1);
        else
            fUniqueID = 1;
#endif
		/********************************************************************
		* 
		* 将该socket对象加入到 OSRef 对象中,key=fUniqueID,value=this
		*
		*********************************************************************/
        fRef.Set(fUniqueIDStr, this);

		/********************************************************************
		* 
		* 将 OSRef 对象添加到对象引用表中
		*
		*********************************************************************/
        fEventThread->fRefTable.Register(&fRef);
		
		/********************************************************************
		* 
		* 填充 eventreq 结构体
		* 1.记录事件类型
		* 2.记录socket 文件描述符
		* 3.记录事件标志位
		* 4.记录socket对象唯一ID
		*
		*********************************************************************/
        //fill out the eventreq data structure
        ::memset(&fEventReq, '\0', sizeof(fEventReq));
        fEventReq.er_type = EV_FD;
        fEventReq.er_handle = fFileDesc;
        fEventReq.er_eventbits = theMask;
        fEventReq.er_data = (void*)fUniqueID;

		/********************************************************************
		*
		* 设置监听事件正在调用标志位 
		*
		*********************************************************************/
        fWatchEventCalled = true;

		/********************************************************************
		*
		* 将 eventreq 结构体 添加到监听队列中
		*
		*********************************************************************/
#if MACOSXEVENTQUEUE
        if (watchevent(&fEventReq, theMask) != 0)
#else
        if (select_watchevent(&fEventReq, theMask) != 0)
#endif  
            //this should never fail, but if it does, cleanup.
            AssertV(false, OSThread::GetErrno());
            
    }
}

void EventThread::Entry()
{
    struct eventreq theCurrentEvent;
    ::memset( &theCurrentEvent, '\0', sizeof(theCurrentEvent) );

	/***************************************
	*	
	* 事件线程进入永久循环
	*
	****************************************/
    while (true)
    {
        int theErrno = EINTR;
        while (theErrno == EINTR){
        	/****************************************
			*	
			* 等待事件
			*
			*****************************************/
#if MACOSXEVENTQUEUE
            int theReturnValue = waitevent(&theCurrentEvent, NULL);
#else
            int theReturnValue = select_waitevent(&theCurrentEvent, NULL);
#endif  
            //Sort of a hack. In the POSIX version of the server, waitevent can return
            //an actual POSIX errorcode.
            if (theReturnValue >= 0){
                theErrno = theReturnValue;
            }else{
                theErrno = OSThread::GetErrno();
			}
        }
        /********************************************
		*	
		* 异常检查
		*
		*********************************************/
        AssertV(theErrno == 0, theErrno);

		/*******************************************
		*	
		* 检查到有事件发生
		*
		********************************************/
        //ok, there's data waiting on this socket. Send a wakeup.
        if (theCurrentEvent.er_data != NULL)
        {
            //The cookie in this event is an ObjectID. Resolve that objectID into
            //a pointer.
            StrPtrLen idStr((char*)&theCurrentEvent.er_data, 
            					/*sizeof(theCurrentEvent.er_data)*//* 此处在64位系统有bug */
								4/*将长度写死，在32或64位系统上均适用*/);
			/****************************************
			*
			* 取出对象引用
			*
			*****************************************/
			OSRef* ref = fRefTable.Resolve(&idStr);
            if (ref != NULL)
            {
            	/*********************************************
            	*
            	* 获取事件上下文
            	*
            	**********************************************/
                EventContext* theContext = (EventContext*)ref->GetObject();

				/**************************************** 
				*	
				* 将事件任务加入到任务线程的任务队列中
				*
				*****************************************/
                theContext->ProcessEvent(theCurrentEvent.er_eventbits);

				/****************************************
				*
				* 引用表释放引用
				*
				*****************************************/
				fRefTable.Release(ref);
            }
        }
		
		/*****************************************
		*
		* 通知系统内核,主动让出执行权
		*
		******************************************/
        this->ThreadYield();
    }
}
