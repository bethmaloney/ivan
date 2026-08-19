/*
 *
 *  Iter Vehemens ad Necem (IVAN)
 *  Copyright (C) Timo Kiviluoto
 *  Released under the GNU General
 *  Public License
 *
 *  See LICENSING which should be included
 *  along with this file for more details
 *
 */

#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#ifdef BACKTRACE
#include <execinfo.h>
#endif

#ifdef WIN32
#include "SDL.h"
#include <windows.h>
#else
#include <iostream>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#endif

#ifdef VC
#include <new.h>
#define set_new_handler _set_new_handler
#else
#include <new>
#define set_new_handler std::set_new_handler
#endif

#include "dbgmsgproj.h"

#include "error.h"

/* Shouldn't be initialized here! */

cchar* globalerrorhandler::BugMsg
= "\n\nPlease submit a bug report on our forum at http://attnam.com\n"
"including a brief description of what you did, what version\n"
"you are running and which kind of system you are using.";

#ifdef VC
int (*globalerrorhandler::OldNewHandler)(size_t) = 0;
#else
void (*globalerrorhandler::OldNewHandler)() = 0;
#endif

#ifdef BACKTRACE
void globalerrorhandler::DumpStackTraceToStdErr(int Signal){
  // Prints stack trace to stderr.
  void* CallStack[128];
  size_t Frames = backtrace(CallStack, 128);
  if(Signal>-1)std::cerr << strsignal(Signal) << std::endl;
  backtrace_symbols_fd(CallStack, Frames, STDERR_FILENO);
}
#endif


genericException::genericException(cchar* pc)
{
  pcMsg=pc;
  DBG1(pc);
  DBGBREAKPOINT;
}

void globalerrorhandler::Install()
{
  static truth AlreadyInstalled = false;

  if(!AlreadyInstalled)
  {
    AlreadyInstalled = true;
    OldNewHandler = set_new_handler(NewHandler);

    atexit(globalerrorhandler::DeInstall);
  }
}

void globalerrorhandler::DeInstall()
{
  set_new_handler(OldNewHandler);
}

void globalerrorhandler::Abort(cchar* Format, ...)
{
#ifdef BACKTRACE
  DumpStackTraceToStdErr();
#endif

  char Buffer[512];

  va_list AP;
  va_start(AP, Format);
  vsnprintf(Buffer, sizeof(Buffer), Format, AP);
  va_end(AP);

  strcat(Buffer, BugMsg);

#ifdef WIN32
  ShowWindow(GetActiveWindow(), SW_HIDE);
  MessageBox(NULL, Buffer, "Program aborted!",
             MB_OK|MB_ICONEXCLAMATION|MB_TASKMODAL);
#endif
#ifdef UNIX
  std::cout << Buffer << std::endl;
#endif

  DBGSTK;DBG2("ABORT:",Buffer);DBGBREAKPOINT;
  exit(4);
}

#ifdef VC
int globalerrorhandler::NewHandler(size_t)
#else
  void globalerrorhandler::NewHandler()
#endif
{
  cchar* Msg = "Fatal Error: Memory depleted.\n"
                    "Get more RAM and hard disk space.";
#ifdef WIN32
  ShowWindow(GetActiveWindow(), SW_HIDE);
  MessageBox(NULL, Msg, "Program aborted!", MB_OK|MB_ICONEXCLAMATION);
#endif
#ifdef UNIX
  std::cout << Msg << std::endl;
#endif

  exit(1);

#ifdef VC
  return 0;
#endif
}

truth genericException::bGeneratingNewDungeonLevel=false;
