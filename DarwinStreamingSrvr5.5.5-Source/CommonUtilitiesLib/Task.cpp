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
    File:       Task.cpp

    Contains:   implements Task class
                    
    
*/

#include "Task.h"
#include "OS.h"
#include "OSMemory.h"
#include "atomic.h"
#include "OSMutexRW.h"


unsigned int    Task::sThreadPicker = 0;
OSMutexRW       TaskThreadPool::sMutexRW;
static char* sTaskStateStr="live_"; //Alive

Task::Task()
:   fEvents(0), fUseThisThread(NULL), fWriteLock(false), fTimerHeapElem(), fTaskQueueElem()
{
    this->SetTaskName("unknown");

	fTaskQueueElem.SetEnclosingObject(this);
	fTimerHeapElem.SetEnclosingObject(this);

}

void Task::SetTaskName(char* name) 
{
    if (name == NULL) 
        return;
   
   ::strncpy(fTaskName,sTaskStateStr,sizeof(fTaskName));
   ::strncat(fTaskName,name,sizeof(fTaskName));
   fTaskName[sizeof(fTaskName) -1] = 0; //terminate in case it is longer than ftaskname.
   
}

Bool16 Task::Valid()
{
    if  (   (this->fTaskName == NULL)
         || (0 != ::strncmp(sTaskStateStr,this->fTaskName, 5))
         )
     {
        return false;
     }
    
    return true;
}

Task::EventFlags Task::GetEvents()
{
	/*****************************************************
	*
	* 屏蔽当前掩码中除激活标志位之外的所有标志位
	* 激活标志位为最高位
	* 处理后 fEvents == kAlive
	*
	******************************************************/
    //Mask off every event currently in the mask except for the alive bit, of course,
    //which should remain unaffected and unreported by this call.
    EventFlags events = fEvents & kAliveOff;
    (void)atomic_sub(&fEvents, events);
    return events;
}

void Task::Signal(EventFlags events)
{
    if (!this->Valid()){
        return;
	}
        
    //Fancy no mutex implementation. We atomically mask the new events into
    //the event mask. Because atomic_or returns the old state of the mask,
    //we only schedule this task once.
    /***********************************************
    *
    * 设置任务激活标志位
    *
    ************************************************/
    events |= kAlive;
	
    /***********************************************
    *
    * 记录新的任务标志位集合并返回老的任务标志位集合
    *
    ************************************************/
    EventFlags oldEvents = atomic_or(&fEvents, events);

	/**********************************************************************************
	*
	* 	enum
    *    {
    *        kKillEvent     =   0x1 << 0x0, //these are all of type "EventFlags"
    *        kIdleEvent 	=   0x1 << 0x1,
    *        kStartEvent 	=   0x1 << 0x2,
    *        kTimeoutEvent 	= 	0x1 << 0x3,
    *        kReadEvent 	=   0x1 << 0x4, //All of type "EventFlags"
    *        kWriteEvent 	=   0x1 << 0x5,
    *        kUpdateEvent 	=   0x1 << 0x6
    *    };
    * 
    * atomic_or函数计算后结果对应关系:
    *
    * events== kKillEvent 		--> fEvents== 0x80000000 | 0x1 << 0x0
	* events== kIdleEvent       --> fEvents== 0x80000000 | 0x1 << 0x1
	* events== kStartEvent      --> fEvents== 0x80000000 | 0x1 << 0x2
	* events== kTimeoutEvent    --> fEvents== 0x80000000 | 0x1 << 0x3
	* events== kReadEvent       --> fEvents== 0x80000000 | 0x1 << 0x4
	* events== kWriteEvent      --> fEvents== 0x80000000 | 0x1 << 0x5
	* events== kUpdateEvent     --> fEvents== 0x80000000 | 0x1 << 0x6
	*
	**********************************************************************************/
	
    /***********************************************
    *
    * 1.旧标志位必须为非激活状态
    * 2.线程池中的任务线程数必须 > 0
    * 3.将任务放入到任务线程的阻塞队列中。
    *
    ************************************************/
    if ((!(oldEvents & kAlive)) && (TaskThreadPool::sNumTaskThreads > 0))
    {	
		/***********************************************
		*
		* 任务指定了运行线程
		*
		************************************************/
        if (fUseThisThread != NULL){ // Task needs to be placed on a particular thread.
            fUseThisThread->fTaskQueue.EnQueue(&fTaskQueueElem);
        }
			
		/***********************************************
		*
		* 放入随机选择的线程中
		*
		************************************************/
        else{
            //find a thread to put this task on
            unsigned int theThread = atomic_add(&sThreadPicker, 1);
            theThread %= TaskThreadPool::sNumTaskThreads;
            TaskThreadPool::sTaskThreadArray[theThread]->fTaskQueue.EnQueue(&fTaskQueueElem);
        }
    }
}


void    Task::GlobalUnlock()    
{   
    /***********************************************
    *
    *
    *
    ************************************************/

    if (this->fWriteLock){   
		this->fWriteLock = false;   
        TaskThreadPool::sMutexRW.Unlock();
    }                                               
}



void TaskThread::Entry()
{
    Task* theTask = NULL;
    
    while (true){
    	/******************************************************
    	*
    	* 等待任务
    	*
    	*******************************************************/
        theTask = this->WaitForTask();

    	/******************************************************
    	*
    	* 检查任务对象是否为空，不允许有空任务指针
		* 否则会导致任务线程退出
    	*
    	*******************************************************/
        // WaitForTask returns NULL when it is time to quit
        if (theTask == NULL || false == theTask->Valid()){
            return;
		}
                    
        Bool16 doneProcessingEvent = false;
		
        /********************************************************************
    	*
    	* doneProcessingEvent 标志在任务执行完成后会被设置成true，使循环结束
    	* 
    	*********************************************************************/
        while (!doneProcessingEvent){
            //If a task holds locks when it returns from its Run function,
            //that would be catastrophic and certainly lead to a deadlock
            theTask->fUseThisThread = NULL; // Each invocation of Run must independently
                                            // request a specific thread.
            SInt64 theTimeout = 0;

			/******************************************************
			*
			* 使用写锁
			*
			*******************************************************/
            if (theTask->fWriteLock){  
				/*******************************************************
				*
				* 如果任务中有写锁，需要使用写互斥量，否则可能造成死锁
				*
				********************************************************/
                OSMutexWriteLocker mutexLocker(&TaskThreadPool::sMutexRW);
				
				/******************************************************
				*
				* 执行任务，并取得任务返回的期望下次调用的超时
				*
				*******************************************************/
                theTimeout = theTask->Run();
                theTask->fWriteLock = false;
            }else{
                OSMutexReadLocker mutexLocker(&TaskThreadPool::sMutexRW);
				/******************************************************
				*
				* 执行任务，并取得任务返回的期望下次调用的超时
				*
				*******************************************************/

                theTimeout = theTask->Run();
            }
        
			/******************************************************
    		*
    		* 超时<0,删除任务
    		*
    		*******************************************************/
            if (theTimeout < 0){
                theTask->fTaskName[0] = 'D'; //mark as dead
				/************************************
				*
				* 删除任务
				*
				*************************************/
				delete theTask;
                theTask = NULL;
                doneProcessingEvent = true;

            }
			
			/********************************************************************************
    		*
    		* 超时==0,表明任务希望在下次传信时被再次立即执行
    		* theTask->fEvents 会在theTask->Run()调用GetEvents()函数时被设置成 Task::kAlive
    		*
    		**********************************************************************************/
			else if (theTimeout == 0){
                //We want to make sure that 100% definitely the task's Run function WILL
                //be invoked when another thread calls Signal. We also want to make sure
                //that if an event sneaks in right as the task is returning from Run()
                //(via Signal) that the Run function will be invoked again.
                doneProcessingEvent = compare_and_store(Task::kAlive, 0, &theTask->fEvents);
                if (doneProcessingEvent)
                    theTask = NULL; 
            }
			
			/******************************************************
    		*
    		* 超时>0,将任务插入到最小堆中
    		*
    		*******************************************************/
            else{
                //note that if we get here, we don't reset theTask, so it will get passed into
                //WaitForTask
                theTask->fTimerHeapElem.SetValue(OS::Milliseconds() + theTimeout);
                fHeap.Insert(&theTask->fTimerHeapElem);
                (void)atomic_or(&theTask->fEvents, Task::kIdleEvent);
                doneProcessingEvent = true;
            }
			
			/************************************************
			*
			* 让出线程执行权，通知系统内核切换到其他线程执行
			*
			*************************************************/
        	this->ThreadYield();
        }
    }
}

Task* TaskThread::WaitForTask()
{
    while (true){
    	/******************************************************
    	*
    	* 获取系统时间
    	*
    	*******************************************************/
        SInt64 theCurrentTime = OS::Milliseconds();

    	/******************************************************
    	*
    	* 先探测下，不实际调用任务对象
    	*
    	*******************************************************/
        if ((fHeap.PeekMin() != NULL) && (fHeap.PeekMin()->GetValue() <= theCurrentTime)){    

			/******************************************************
    		*
    		* 有超时任务，抽取实际任务对象并返回
    		*
    		*******************************************************/
			return (Task*)fHeap.ExtractMin()->GetEnclosingObject();
        }
		
     	/******************************************************
    	*
    	* 没有超时的任务，重新计算超时时间
    	*
    	*******************************************************/
        //if there is an element waiting for a timeout, figure out how long we should wait.
        SInt64 theTimeout = 0;
        if (fHeap.PeekMin() != NULL){
            theTimeout = fHeap.PeekMin()->GetValue() - theCurrentTime;
		}
        Assert(theTimeout >= 0);
        
        //
        // Make sure we can't go to sleep for some ridiculously short
        // period of time
        // Do not allow a timeout below 10 ms without first verifying reliable udp 1-2mbit live streams. 
        // Test with streamingserver.xml pref reliablUDP printfs enabled and look for packet loss and check client for  buffer ahead recovery.
		/******************************************************
		*
		* 超时时间小于10ms没有实际意义，设置为10ms
		*
		*******************************************************/
		if (theTimeout < 10){ 
           	theTimeout = 10;
		}
	
		/*****************************************************************************
		*
		* 进入阻塞队列，直到有任务加入到队列，取出并返回
		* 注：任务在调用Signal函数时，会将自身插入到阻塞队列中
		*     当任务被第一次执行后，并且Run函数返回值大于0，会将任务插入到最小堆对象中
		*
		******************************************************************************/
        //wait...
        OSQueueElem* theElem = fTaskQueue.DeQueueBlocking(this, (SInt32) theTimeout);
        if (theElem != NULL){    
			
			/******************************************************
    		*
    		* 有超时任务，抽取实际任务对象并返回
    		*
    		*******************************************************/
			return (Task*)theElem->GetEnclosingObject();
        }

		/******************************************************
		*
		* 检测下任务线程是否被终止 
		*
		*******************************************************/
        // If we are supposed to stop, return NULL, which signals the caller to stop
        if (OSThread::GetCurrent()->IsStopRequested()){
            return NULL;
		}
    }   
}

TaskThread** TaskThreadPool::sTaskThreadArray = NULL;
UInt32       TaskThreadPool::sNumTaskThreads = 0;

Bool16 TaskThreadPool::AddThreads(UInt32 numToAdd)
{
    Assert(sTaskThreadArray == NULL);
    sTaskThreadArray = new TaskThread*[numToAdd];
    /***********************************************
    *
    * 1.添加任务线程到线程池
    * 2.记录任务线程数量
    *
    ************************************************/
    for (UInt32 x = 0; x < numToAdd; x++){
        sTaskThreadArray[x] = NEW TaskThread();
        sTaskThreadArray[x]->Start();
    }
	
    sNumTaskThreads = numToAdd;
	
    return true;
}


void TaskThreadPool::RemoveThreads()
{
	/*********************************************
	*
	* 通知所有线程停止
	*
	**********************************************/
    //Tell all the threads to stop
    for (UInt32 x = 0; x < sNumTaskThreads; x++){
        sTaskThreadArray[x]->SendStopRequest();
    }
	
	/*********************************************
	*
	* 防止线程阻塞在队列中，发信号通知线程跳出阻塞队列
	*
	**********************************************/
    //Because any (or all) threads may be blocked on the queue, cycle through
    //all the threads, signalling each one
    for (UInt32 y = 0; y < sNumTaskThreads; y++){
        sTaskThreadArray[y]->fTaskQueue.GetCond()->Signal();
	}
	
	/*********************************************
	*
	* 删除所有线程对象
	*
	**********************************************/
    //Ok, now wait for the selected threads to terminate, deleting them and removing
    //them from the queue.
    for (UInt32 z = 0; z < sNumTaskThreads; z++){
        delete sTaskThreadArray[z];
    }
    
    sNumTaskThreads = 0;
}
