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
    File:       main.cpp

    Contains:   main function to drive streaming server.


*/

#include <stdio.h>
#include <stdlib.h>
#include "SafeStdLib.h"
#include "defaultPaths.h"
#ifndef __MacOSX__ 
#include "getopt.h"
#endif

#include <sys/time.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/wait.h>

#ifndef __Win32__ 
#include <unistd.h>
#endif

#if defined (__solaris__) || defined (__osf__) || defined (__hpux__)
#include "daemon.h"
#endif

#if __MacOSX__ || __FreeBSD__
#include <sys/sysctl.h>
#include <sys/stat.h>
#endif

#include "FilePrefsSource.h"
#include "RunServer.h"
#include "QTSServer.h"
#include "QTSSExpirationDate.h"
#include "GenerateXMLPrefs.h"

static int sSigIntCount = 0;
static int sSigTermCount = 0;
static pid_t sChildPID = 0;

void usage();
void usage()
{
    const char *usage_name = PLATFORM_SERVER_BIN_NAME;

   qtss_printf("%s/%s ( Build/%s; Platform/%s; %s) Built on: %s\n",QTSServerInterface::GetServerName().Ptr,
                                        QTSServerInterface::GetServerVersion().Ptr,
                                        QTSServerInterface::GetServerBuild().Ptr,
                                        QTSServerInterface::GetServerPlatform().Ptr,
                                        QTSServerInterface::GetServerComment().Ptr,
                                        QTSServerInterface::GetServerBuildDate().Ptr);
    qtss_printf("usage: %s [ -d | -p port | -v | -c /myconfigpath.xml | -o /myconfigpath.conf | -x | -S numseconds | -I | -h ]\n", usage_name);
    qtss_printf("-d: Run in the foreground\n");
    qtss_printf("-D: Display performance data\n");
    qtss_printf("-p XXX: Specify the default RTSP listening port of the server\n");
    qtss_printf("-c /myconfigpath.xml: Specify a config file\n");
    qtss_printf("-o /myconfigpath.conf: Specify a DSS 1.x / 2.x config file to build XML file from\n");
    qtss_printf("-x: Force create new .xml config file and exit.\n");
    qtss_printf("-S n: Display server stats in the console every \"n\" seconds\n");
    qtss_printf("-I: Start the server in the idle state\n");
    qtss_printf("-h: Prints usage\n");
}

Bool16 sendtochild(int sig, pid_t myPID);
Bool16 sendtochild(int sig, pid_t myPID)
{
    if (sChildPID != 0 && sChildPID != myPID) // this is the parent
    {   // Send signal to child
        ::kill(sChildPID, sig);						
        return true;
    }

    return false;
}

void sigcatcher(int sig, int /*sinfo*/, struct sigcontext* /*sctxt*/);
/* 信号处理函数 */
void sigcatcher(int sig, int /*sinfo*/, struct sigcontext* /*sctxt*/)
{
#if DEBUG
    qtss_printf("Signal %d caught\n", sig);
#endif
    pid_t myPID = getpid();
    //
    // SIGHUP means we should reread our preferences
    /* 收到SIGHUP 信号,重读偏好配置信息,并发送信号杀死子进程 */
    if (sig == SIGHUP)
    {
        if (sendtochild(sig,myPID))
        {
            return;
        }
        else
        {
            // This is the child process.
            // Re-read our preferences.
            RereadPrefsTask* task = new RereadPrefsTask;
            task->Signal(Task::kStartEvent);

        }
    }
        
    //Try to shut down gracefully the first time, shutdown forcefully the next time
    if (sig == SIGINT) // kill the child only
    {
        if (sendtochild(sig,myPID))
        {
            return;// ok we're done 
        }
        else
        {
			//
			// Tell the server that there has been a SigInt, the main thread will start
			// the shutdown process because of this. The parent and child processes will quit.
			if (sSigIntCount == 0)
				QTSServerInterface::GetServer()->SetSigInt();
			sSigIntCount++;
		}
    }
	
	if (sig == SIGTERM || sig == SIGQUIT) // kill child then quit
    {
        if (sendtochild(sig,myPID))
        {
             return;// ok we're done 
        }
        else
        {
			// Tell the server that there has been a SigTerm, the main thread will start
			// the shutdown process because of this only the child will quit
    
    
			if (sSigTermCount == 0)
				QTSServerInterface::GetServer()->SetSigTerm();
			sSigTermCount++;
		}
    }
}

extern "C" {
typedef int (*EntryFunction)(int input);
}

Bool16 RunInForeground();
Bool16 RunInForeground()
{
	/* 定向到标准输出 */
    #if __linux__ || __MacOSX__
         (void) setvbuf(stdout, NULL, _IOLBF, 0);
         OSThread::WrapSleep(true);
    #endif
    
    return true;
}


Bool16 RestartServer(char* theXMLFilePath)
{
	/* 
		重新解析xml文件 (streamingserver.xml) 
	*/
	Bool16 autoRestart = true;
	XMLPrefsParser theXMLParser(theXMLFilePath);
	theXMLParser.Parse();

	/* 获取服务器配置信息 */
	ContainerRef server = theXMLParser.GetRefForServer();
	/* 查找自动重启配置信息 */
	ContainerRef pref = theXMLParser.GetPrefRefByName( server, "auto_restart" );
	char* autoStartSetting = NULL;

	/* 获取 auto_restart 信息(只有true 或 false 两种取值) */
	if (pref != NULL) 
		autoStartSetting = theXMLParser.GetPrefValueByRef( pref, 0, NULL, NULL );
		
	if ( (autoStartSetting != NULL) && (::strcmp(autoStartSetting, "false") == 0) )
		autoRestart = false;
	/* 返回查询结果 */	
	return autoRestart;
}
/*
	Darwin streaming server 主函数
*/
int main(int argc, char * argv[]) 
{
    extern char* optarg;

    /*
		屏蔽部分信号
    */
    // on write, don't send signal for SIGPIPE, just set errno to EPIPE
    // and return -1
    //signal is a deprecated and potentially dangerous function
    //(void) ::signal(SIGPIPE, SIG_IGN);
    struct sigaction act;
    
#if defined(__linux__) || defined(sun) || defined(i386) || defined (__MacOSX__) || defined(__powerpc__) || defined (__osf__) || defined (__sgi_cc__) || defined (__hpux__)
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = (void(*)(int))&sigcatcher;
#elif defined(__sgi__) 
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = (void(*)(...))&sigcatcher;
#else
    act.sa_mask = 0;
    act.sa_flags = 0;
    act.sa_handler = (void(*)(...))&sigcatcher;
#endif
    (void)::sigaction(SIGPIPE, &act, NULL);
    (void)::sigaction(SIGHUP, &act, NULL);
    (void)::sigaction(SIGINT, &act, NULL);
    (void)::sigaction(SIGTERM, &act, NULL);
    (void)::sigaction(SIGQUIT, &act, NULL);
    (void)::sigaction(SIGALRM, &act, NULL);

	/*
		获取最大可用资源

	*/
#if __MacOSX__ || __solaris__ || __linux__ || __hpux__
    //grow our pool of file descriptors to the max!
    struct rlimit rl;
    
    // set it to the absolute maximum that the operating system allows - have to be superuser to do this
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
 
    setrlimit (RLIMIT_NOFILE, &rl);
#endif

#if __MacOSX__
   getrlimit(RLIMIT_NOFILE,  &rl); //get the resulting max value from setting RLIM_INFINITY
   rl.rlim_cur = (rlim_t) ( (float) rl.rlim_cur * 0.9);   //use 90%
   rl. rlim_max = (rlim_t) ( (float) rl. rlim_max * 0.9);   //use 90%
   setrlimit (RLIMIT_NOFILE, &rl);
#endif

#if 0 // testing
    getrlimit(RLIMIT_NOFILE,  &rl);
    printf("current open file limit =%lu\n", (long unsigned) rl.rlim_cur);
    printf("current open file max =%lu\n", (long unsigned) rl.rlim_max);
#endif


#if __MacOSX__ || __FreeBSD__
        //
        // These 2 OSes have problems with large socket buffer sizes. Make sure they allow even
        // ridiculously large ones, because we may need them to receive a large volume of ACK packets
        // from the client
        
        //
        // We raise the limit imposed by the kernel by calling the sysctl system call.
        int mib[CTL_MAXNAME];
        mib[0] = CTL_KERN;
        mib[1] = KERN_IPC;
        mib[2] = KIPC_MAXSOCKBUF;
        mib[3] = 0;

        int maxSocketBufferSizeVal = 2048 * 1024; // Allow up to 2 MB. That is WAY more than we should need
        (void) ::sysctl(mib, 3, 0, 0, &maxSocketBufferSizeVal, sizeof(maxSocketBufferSizeVal));
 #endif

    /*
		解析命令行
    */
    //First thing to do is to read command-line arguments.
    int ch;
    int thePort = 0; //port can be set on the command line
    int statsUpdateInterval = 0;
    QTSS_ServerState theInitialState = qtssRunningState;
    
    Bool16 dontFork = false;
    Bool16 theXMLPrefsExist = true;
    UInt32 debugLevel = 0;
    UInt32 debugOptions = kRunServerDebug_Off;
	static char* sDefaultConfigFilePath = DEFAULTPATHS_ETC_DIR_OLD "streamingserver.conf";
	static char* sDefaultXMLFilePath = DEFAULTPATHS_ETC_DIR "streamingserver.xml";

    char* theConfigFilePath = sDefaultConfigFilePath;
    char* theXMLFilePath = sDefaultXMLFilePath;
    while ((ch = getopt(argc,argv, "vdfxp:DZ:c:o:S:Ih")) != EOF) // opt: means requires option arg
    {
        switch(ch)
        {
            case 'v':   /*打印用法*/
                usage();
                ::exit(0);  
            case 'd': 	/*前台运行,单进程*/
                dontFork = RunInForeground(); 
                
                break;                
            case 'D': 	/* 前台运行,单进程，显示性能信息 */
               dontFork = RunInForeground(); 

               debugOptions |= kRunServerDebugDisplay_On;
                
               if (debugLevel == 0)
                    debugLevel = 1;
                    
               if (statsUpdateInterval == 0)
                    statsUpdateInterval = 3;
                    
               break;            
            case 'Z':	/* 设置调试等级 */
                Assert(optarg != NULL);// this means we didn't declare getopt options correctly or there is a bug in getopt.
                debugLevel = (UInt32) ::atoi(optarg);
                                
                break;
            case 'f':   /*使用默认streamingserver.xml文件*/
				theXMLFilePath  = DEFAULTPATHS_ETC_DIR "streamingserver.xml";
                break;
            case 'p':   /* 监听指定端口 */
                Assert(optarg != NULL);// this means we didn't declare getopt options correctly or there is a bug in getopt.
                thePort = ::atoi(optarg);
                break;
            case 'S':	/* 指定状态刷新间隔 */
                dontFork = RunInForeground();
                Assert(optarg != NULL);// this means we didn't declare getopt options correctly or there is a bug in getopt.
                statsUpdateInterval = ::atoi(optarg);
                break;
            case 'c':	/* 使用指定文件作为 偏好配置文件(功能同streamingserver.xml) */
                Assert(optarg != NULL);// this means we didn't declare getopt options correctly or there is a bug in getopt.
                theXMLFilePath = optarg;
                break;
            case 'o':	/* 使用指定config 文件 */
                Assert(optarg != NULL);// this means we didn't declare getopt options correctly or there is a bug in getopt.
                theConfigFilePath = optarg;
                break;
            case 'x':  /* 强制创建新的 .xml config 文件并退出 */
                theXMLPrefsExist = false; // Force us to generate a new XML prefs file
                theInitialState = qtssShuttingDownState;
                dontFork = true;
                break;
            case 'I':   /* 以调度状态创建服务器 */
                theInitialState = qtssIdleState;
                break;
            case 'h':	/* 显示帮助信息并退出 */
                usage();
                ::exit(0);
            default:	/* 采用默认值创建服务器 */
                break;
        }
    }
    
  	/*
		检查端口
  	*/
    // Check port
    if (thePort < 0 || thePort > 65535)
    { 
        qtss_printf("Invalid port value = %d max value = 65535\n",thePort);
        exit (-1);
    }

	/*
		检查到期日期
	*/
    // Check expiration date
    QTSSExpirationDate::PrintExpirationDate();
    if (QTSSExpirationDate::IsSoftwareExpired())
    {
        qtss_printf("Streaming Server has expired\n");
        ::exit(0);
    }

	/*
		分析配置文件
	*/
    XMLPrefsParser theXMLParser(theXMLFilePath);

    /*
		检查配置文件是否存在
    */
    //
    // Check to see if the XML file exists as a directory. If it does,
    // just bail because we do not want to overwrite a directory
    if (theXMLParser.DoesFileExistAsDirectory())
    {
        qtss_printf("Directory located at location where streaming server prefs file should be.\n");
        exit(-1);
    }

    /*
		检查是否有写入权限
    */
    //
    // Check to see if we can write to the file
    if (!theXMLParser.CanWriteFile())
    {
        qtss_printf("Cannot write to the streaming server prefs file.\n");
        ::exit(-1);
    }
	/*
		是否需要强制创建配置文件
	*/
    // If we aren't forced to create a new XML prefs file, whether
    // we do or not depends solely on whether the XML prefs file exists currently.
    if (theXMLPrefsExist)
        theXMLPrefsExist = theXMLParser.DoesFileExist();

    /* 偏好设置文件不存在,我们做些处理 */
    if (!theXMLPrefsExist)
    {
        
        //
        // The XML prefs file doesn't exist, so let's create an old-style
        // prefs source in order to generate a fresh XML prefs file.
        
        if (theConfigFilePath != NULL)
        {   
            FilePrefsSource* filePrefsSource = new FilePrefsSource(true); // Allow dups
            
            if ( filePrefsSource->InitFromConfigFile(theConfigFilePath) )
            { 
               qtss_printf("Generating a new prefs file at %s\n", theXMLFilePath);
            }

            if (GenerateAllXMLPrefs(filePrefsSource, &theXMLParser))
            {
                qtss_printf("Fatal Error: Could not create new prefs file at: %s. (%d)\n", theXMLFilePath, OSThread::GetErrno());
                ::exit(-1);
            }
        }
    }

 	/* 分析配置文件 */
    //
    // Parse the configs from the XML file
    int xmlParseErr = theXMLParser.Parse();
    if (xmlParseErr)
    {
        qtss_printf("Fatal Error: Could not load configuration file at %s. (%d)\n", theXMLFilePath, OSThread::GetErrno());
        ::exit(-1);
    }

    /*
		是否后台运行
    */
    //Unless the command line option is set, fork & daemonize the process at this point
    if (!dontFork)
    {
#ifdef __sgi__
		// for some reason, this method doesn't work right on IRIX 6.4 unless the first arg
		// is _DF_NOFORK.  if the first arg is 0 (as it should be) the result is a server
		// that is essentially paralized and doesn't respond to much at all.  So for now,
		// leave the first arg as _DF_NOFORK
//		if (_daemonize(_DF_NOFORK, STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO) != 0)
        if (_daemonize(0, STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO) != 0)
#else
		/* 启动守护进程 */
        if (daemon(0,0) != 0)
#endif
        {
#if DEBUG
            qtss_printf("Failed to daemonize process. Error = %d\n", OSThread::GetErrno());
#endif
            exit(-1);
        }
    }
    
    //Construct a Prefs Source object to get server text messages
    FilePrefsSource theMessagesSource;
    theMessagesSource.InitFromConfigFile("qtssmessages.txt");
    
	
    int status = 0;
    int pid = 0;
    pid_t processID = 0;

	/*
		为守护进程创建新的子进程

		守护进程监听子进程的退出状态
	*/
    if ( !dontFork) // if (fork) 
    {
        //loop until the server exits normally. If the server doesn't exit
        //normally, then restart it.
        // normal exit means the following
        // the child quit 
        do // fork at least once but stop on the status conditions returned by wait or if autoStart pref is false
        {	/* 创建新进程 */
            processID = fork();
            Assert(processID >= 0);
            /* 
            	父进程 
				守护进程
            */
            if (processID > 0) // this is the parent and we have a child
            {
                sChildPID = processID;
                status = 0;
                while (status == 0) //loop on wait until status is != 0;
                {	
                	/* 等待子进程中断或结束 */
                 	pid =::wait(&status);

                 	/* 
                 		返回子进程的退出码，用来判断子进程的退出值 
						如子进程以 -1 退出，则 exitStatus = -1
						子进程以 2 退出，则 exitStatus = 2
                 	*/
                 	SInt8 exitStatus = (SInt8) WEXITSTATUS(status);

                	/* 指出子进程是否为正常退出的，如果是，返回一个非零值 */
					if (WIFEXITED(status) && pid > 0 && status != 0) // child exited with status -2 restart or -1 don't restart 
					{
						/*
							子进程以-1退出，父进程也退出
						*/
						if ( exitStatus == -1) // child couldn't run don't try again
						{
							qtss_printf("child exited with -1 fatal error so parent is exiting too.\n");
							exit (EXIT_FAILURE); 
						}
						break; // restart the child
							
					}

					/* 子进程是因为信号而结束则此宏值为真
					   重启子进程
					*/
					if (WIFSIGNALED(status)) // child exited on an unhandled signal (maybe a bus error or seg fault)
					{	
						//qtss_printf("child was signalled\n");
						break; // restart the child
					}

                 	/* 父进程被信号唤醒,接着等待 */
                	if (pid == -1 && status == 0) // parent woken up by a handled signal
                   	{
						//qtss_printf("handled signal continue waiting\n");
                   		continue;
                   	}
                   	/*子进程完全退出，父进程退出*/
                 	if (pid > 0 && status == 0)
                 	{
                 		//qtss_printf("child exited cleanly so parent is exiting\n");
                 		exit(EXIT_SUCCESS);                		
                	}
                	/* 子进程因未知的原因退出，父进程退出 */
                	//qtss_printf("child died for unknown reasons parent is exiting\n");
                	exit (EXIT_FAILURE);
                }
            }/* 子进程跳出循环接着向下执行 */
            else if (processID == 0) // must be the child
				break;
            else
            	exit(EXIT_FAILURE);
            	
            	
            //eek. If you auto-restart too fast, you might start the new one before the OS has
            //cleaned up from the old one, resulting in startup errors when you create the new
            //one. Waiting for a second seems to work
            sleep(1);
            /* 返回true重启子进程,返回false 关闭服务器 */
        } while (RestartServer(theXMLFilePath)); // fork again based on pref if server dies
        if (processID != 0) //the parent is quitting
        	exit(EXIT_SUCCESS);   

        
    }
    sChildPID = 0;
    /* 屏蔽子进程部分信号 */
    //we have to do this again for the child process, because sigaction states
    //do not span multiple processes.
    (void)::sigaction(SIGPIPE, &act, NULL);
    (void)::sigaction(SIGHUP, &act, NULL);
    (void)::sigaction(SIGINT, &act, NULL);
    (void)::sigaction(SIGTERM, &act, NULL);
    (void)::sigaction(SIGQUIT, &act, NULL);

	/* hpux 平台代码  */
#ifdef __hpux__  
	// Set Priority Type to Real Time, timeslice = 100 milliseconds. Change the timeslice upwards as needed. This keeps the server priority above the playlist broadcaster which is a time-share scheduling type.
	char commandStr[64];
	qtss_sprintf(commandStr, "/usr/bin/rtprio -t -%d", (int) getpid()); 
#if DEBUG
	qtss_printf("setting priority to Real Time: %s\n", commandStr);
#endif
	(void) ::system(commandStr);    
#endif

	/* solaris 平台代码 */
#ifdef __solaris__  
    // Set Priority Type to Real Time, timeslice = 100 milliseconds. Change the timeslice upwards as needed. This keeps the server priority above the playlist broadcaster which is a time-share scheduling type.
    char commandStr[64];
    qtss_sprintf(commandStr, "priocntl -s -c RT -t 10 -i pid %d", (int) getpid()); 
    (void) ::system(commandStr);    
#endif

	/* mac 平台代码 */
#ifdef __MacOSX__
    (void) ::umask(S_IWGRP|S_IWOTH); // make sure files are opened with default of owner -rw-r-r-
#endif
	/* 启动服务器 */
    //This function starts, runs, and shuts down the server
    if (::StartServer(&theXMLParser, &theMessagesSource, thePort, statsUpdateInterval, theInitialState, dontFork, debugLevel, debugOptions) != qtssFatalErrorState)
    {    
		 /* 收集服务器状态信息 */
		 ::RunServer();
		 /* 清除进程号 */
         CleanPid(false);
		 /* 进程退出 */
         exit (EXIT_SUCCESS);
    }
	/* 出错退出 */
    else{
    	exit(-1); //Cant start server don't try again
   }
}
