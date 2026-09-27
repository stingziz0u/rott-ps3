/*
 * Copyright (C) 2026 the ROTT-PS3 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * =======================================================================
 *
 * Log file. On the PS3 there is no console: a failure looks like a black
 * screen and a trip back to the XMB. The log is the only witness, so:
 *
 *  - it is rotated on every boot (rott_log.txt -> rott_log.old.txt),
 *    so it never grows forever and the previous run survives a relaunch;
 *  - every write is flushed: after a crash, the last line is the one that
 *    says where it died;
 *  - writes are serialized: the audio thread logs too.
 *
 * =======================================================================
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "ps3_platform.h"

#ifdef __PPU__
#include <sys/mutex.h>

static sys_mutex_t log_mutex;
static int log_mutex_ok;

static void
LogLockInit(void)
{
	sys_mutex_attr_t attr;

	memset(&attr, 0, sizeof(attr));
	attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
	attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
	attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
	attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	strcpy(attr.name, "rottlog");
	log_mutex_ok = (sysMutexCreate(&log_mutex, &attr) == 0);
}

#define LOG_LOCK()   do { if (log_mutex_ok) sysMutexLock(log_mutex, 0); } while (0)
#define LOG_UNLOCK() do { if (log_mutex_ok) sysMutexUnlock(log_mutex); } while (0)

#else
#include <pthread.h>

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static void LogLockInit(void) { }
#define LOG_LOCK()   pthread_mutex_lock(&log_mutex)
#define LOG_UNLOCK() pthread_mutex_unlock(&log_mutex)
#endif

static FILE *logfp;

/* One session's log never grows past this; the previous session's is
   kept as rott_log.old.txt. So at most 2 x 512 KB on the HDD. */
#define LOG_MAX_BYTES (512 * 1024)
static long logbytes;
static int logfull;

/* Called with the lock held. Returns 0 once the limit is reached. */
static int
LogRoom(size_t len)
{
	if (logfull)
	{
		return 0;
	}

	if (logbytes + (long)len > LOG_MAX_BYTES)
	{
		fputs("\n=== log size limit reached: the rest of this session is not logged ===\n",
				logfp);
		fflush(logfp);
		logfull = 1;
		return 0;
	}

	logbytes += (long)len;

	return 1;
}
/* The engine's printf hands us fragments; remember whether we are mid-line so the
 * next PS3_Log() starts on a fresh one. */
static int midline;

void
PS3_LogInit(void)
{
	if (logfp)
	{
		return;
	}

	LogLockInit();

	remove(PS3_LOGFILE_OLD);
	rename(PS3_LOGFILE, PS3_LOGFILE_OLD);

	logfp = fopen(PS3_LOGFILE, "w");

	if (logfp)
	{
		fputs("=== Rise of the Triad (PS3) log ===\n", logfp);
		fflush(logfp);
	}
}

void
PS3_LogRaw(const char *text)
{
	size_t len;

	if (!logfp || !text || !text[0])
	{
		return;
	}

	len = strlen(text);

	LOG_LOCK();

	if (LogRoom(len))
	{
		fwrite(text, 1, len, logfp);
		midline = (text[len - 1] != '\n');
		fflush(logfp);
	}

	LOG_UNLOCK();
}

void
PS3_Log(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	if (!logfp)
	{
		return;
	}

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	LOG_LOCK();

	if (LogRoom(strlen(buf) + 2))
	{
		if (midline)
		{
			fputc('\n', logfp);
		}

		fputs(buf, logfp);
		fputc('\n', logfp);
		midline = 0;
		fflush(logfp);
	}

	LOG_UNLOCK();
}

void
PS3_LogShutdown(void)
{
	if (!logfp)
	{
		return;
	}

	LOG_LOCK();

	/* always, even past the limit: tells a clean exit from a crash */
	fputs(midline ? "\n=== end of log ===\n" : "=== end of log ===\n", logfp);

	fclose(logfp);
	logfp = NULL;
	LOG_UNLOCK();
}
