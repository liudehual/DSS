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
    File:       OSMutex.cpp

    Contains:   

    

*/

#include "OSMutexRW.h"
#include "OSMutex.h"
#include "OSCond.h"

#include <stdlib.h>
#include "SafeStdLib.h"
#include <string.h>

void OSMutexRW::LockRead()
{
    OSMutexLocker locker(&fInternalLock);
    
    AddReadWaiter();
    while   (   ActiveWriter() // active writer so wait
            ||  WaitingWriters() // reader must wait for write waiters
            )
    {   
        fReadersCond.Wait(&fInternalLock,OSMutexRW::eMaxWait);
    }
        
    RemoveReadWaiter();
    AddActiveReader(); // add 1 to active readers
    fActiveReaders = fState;
    
}

void OSMutexRW::LockWrite()
{
    OSMutexLocker locker(&fInternalLock);
    AddWriteWaiter();       //  1 writer queued            

    while (ActiveReaders()){  // active readers
        fWritersCond.Wait(&fInternalLock,OSMutexRW::eMaxWait);
    }

    RemoveWriteWaiter(); // remove from waiting writers
    SetState(OSMutexRW::eActiveWriterState);    // this is the active writer    
    fActiveReaders = fState; 

}


void OSMutexRW::Unlock()
{           
    OSMutexLocker locker(&fInternalLock);

    if (ActiveWriter()) 
    {           
        SetState(OSMutexRW::eNoWriterState); // this was the active writer 
        if (WaitingWriters()) // there are waiting writers
        {   fWritersCond.Signal();
        }
        else
        {   fReadersCond.Broadcast();
        }
    }
    else
    {
        RemoveActiveReader(); // this was a reader
        if (!ActiveReaders()) // no active readers
        {   SetState(OSMutexRW::eNoWriterState); // this was the active writer now no actives threads
            fWritersCond.Signal();
        } 
    }
    fActiveReaders = fState;

}



// Returns true on successful grab of the lock, false on failure
int OSMutexRW::TryLockWrite()
{
    int    status  = EBUSY;
    OSMutexLocker locker(&fInternalLock);

    if ( !Active() && !WaitingWriters()) // no writers, no readers, no waiting writers
    {
        this->LockWrite();
        status = 0;
    }

    return status;
}

int OSMutexRW::TryLockRead()
{
    int    status  = EBUSY;
    OSMutexLocker locker(&fInternalLock);

    if ( !ActiveWriter() && !WaitingWriters() ) // no current writers but other readers ok
    {
        this->LockRead(); 
        status = 0;
    }
    
    return status;
}



