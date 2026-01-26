//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "rc_base.h"

#if PLATFORM_WINDOWS

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

int vasprintf(char ** ret, const char * format, va_list ap) {
    int len;
    /* Get Length */
    len = _vsnprintf(NULL,0,format,ap);
    if (len < 0) return -1;
    /* +1 for \0 terminator. */
    *ret = (char*)malloc(len + 1);
    /* Check malloc fail*/
    if (!*ret) return -1;
    /* Write String */
    _vsnprintf(*ret,len+1,format,ap);
    /* Terminate explicitly */
    (*ret)[len] = '\0';
    return len;
}

int gettimeofday(struct timeval *tv, void *unused) {
  uint64_t tmp;
  FILETIME ft;
  if (tv) { 
    GetSystemTimeAsFileTime(&ft);
    tmp = ((uint64_t)ft.dwHighDateTime << 32) | ((uint64_t)ft.dwLowDateTime);
    tmp = tmp / 10  - 11644473600000000Ui64;
    tv->tv_sec  = (long)(tmp / 1000000UL);
    tv->tv_usec = (long)(tmp % 1000000UL);
  }
  return 0;
}

void RC__usleep(int interval) {
	Sleep(interval / 1000);		// XXX: ms resolution (probably not a real problem)
}

#else 

#include <sys/time.h>
#include <unistd.h>

void RC__usleep(int interval) {
	usleep(interval);
}

#endif

#if defined(PLATFORM_MACOSX) 

#include <mach/clock.h>
#include <mach/mach.h>
    
static clock_serv_t macosx_cclock;
static bool macosx_didinit = false;

int64_t RC__now_ns(void) {
    if (!macosx_didinit) {
        host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &macosx_cclock);
        macosx_didinit = true;
    }
    mach_timespec_t mts;
    clock_get_time(macosx_cclock, &mts);
    return  (int64_t)mts.tv_sec * 1000000000 + (int64_t)mts.tv_nsec;
}

#elif defined(PLATFORM_IOS) 

#include <mach/mach_time.h>
    
int64_t RC__now_ns(void) {
    static mach_timebase_info_data_t s_timebase_info;
    static bool ios_didinit = false;
    if (!ios_didinit) {
        mach_timebase_info(&s_timebase_info);
        ios_didinit = true;
    }
    uint64_t theHostTime      = mach_absolute_time();
    double   thePartialAnswer = theHostTime / s_timebase_info.denom;
    double   theFloatAnswer   = thePartialAnswer * s_timebase_info.numer;
    int64_t theAnswer         = (int64_t)theFloatAnswer;
    return theAnswer;
}

#elif defined(PLATFORM_LINUX)

int64_t RC__now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

#else

int64_t RC__now_ns(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)((tv.tv_sec * 1000000 + (int64_t)tv.tv_usec) * 1000);
}

#endif

int64_t RC__now_us(void) {
    return RC__now_ns() / 1000;
}


#ifdef PLATFORM_MACOSX
static void _signal_handler(int signum) {
    _exit(128 + signum);
}
#endif

RC_API void RC__suppress_crash_dialogs(void) {
#if defined(PLATFORM_WINDOWS)
    {
        /* disable crash dialogs */
        DWORD dwMode = SetErrorMode(SEM_NOGPFAULTERRORBOX);
        SetErrorMode(dwMode | SEM_NOGPFAULTERRORBOX);
    }
#elif defined(PLATFORM_MACOSX)
    {
        /* suppress apple crash reporter by swallowing signals */
        signal(SIGILL, _signal_handler);
        signal(SIGTRAP, _signal_handler);
        signal(SIGABRT, _signal_handler);
        signal(SIGFPE, _signal_handler);
        signal(SIGBUS, _signal_handler);
        signal(SIGSEGV, _signal_handler);
    }
#endif
}
