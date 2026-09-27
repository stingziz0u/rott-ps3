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
 * Entry point for the PS3. Boots in numbered steps, each one leaves a line
 * in USRDIR/rott_log.txt, so a black screen can be located from the log
 * alone (handoff notes 4.1):
 *
 *   [0] main() reached              -> if missing: -lrt -llv2 in the link
 *   [1] game thread                 -> the EBOOT's own stack is tiny
 *   [2] sysutil callback            -> without it, "Quit Game" reboots
 *   [3] file system (USRDIR, /dev_hdd0/data/rott writable)
 *   [4] CPU and data (endianness, WAD headers read byte by byte)
 *   [5] DualShock 3
 *   [6] RSX
 *   [7] launcher
 *   [8] the engine takes over
 *   [9] shutdown (PS3_Exit: every way out goes through it)
 *
 * The engine is linked in twice (registered and shareware, see
 * ps3/Makefile): ROTT_Main_Registered and ROTT_Main_Shareware are
 * Taradino's main() under two names.
 *
 * =======================================================================
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __PPU__
#include <ppu-types.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>
#else
#include <pthread.h>
#endif

#include "SDL.h"
#include "SDL_mixer.h"
#include "ps3_platform.h"
#include "ps3_launcher.h"
#include "ps3_pad.h"
#include "ps3_video.h"

#ifdef __PPU__
/* The loader sizes the primary thread's stack from this ELF section. */
SYS_PROCESS_PARAM(1001, 0x100000);
#endif

/* ROTT's end screen keeps two 640x400 images on the stack, and the
   renderer recurses: plenty. */
#define GAME_STACK_SIZE (4 * 1024 * 1024)

int ROTT_Main_Registered(int argc, char **argv);
int ROTT_Main_Shareware(int argc, char **argv);

int  PS3_Pad_Init(void);
void PS3_Pad_Shutdown(void);
void PS3_Pad_Poll(ps3_padstate_t *out);

static volatile int exit_requested;
static int sysutil_registered;

/* ================================================================ */
/* sysutil                                                            */
/* ================================================================ */

#ifdef __PPU__
static void
SysutilCallback(u64 status, u64 param, void *userdata)
{
	(void)userdata;

	switch (status)
	{
		case SYSUTIL_EXIT_GAME:
			PS3_Log("[sysutil] SYSUTIL_EXIT_GAME -> quit");
			exit_requested = 1;
			break;

		case SYSUTIL_DRAW_BEGIN:
			PS3_Log("[sysutil] XMB overlay: draw begin");
			break;

		case SYSUTIL_DRAW_END:
			PS3_Log("[sysutil] XMB overlay: draw end");
			break;

		case SYSUTIL_MENU_OPEN:
			PS3_Log("[sysutil] XMB menu open");
			break;

		case SYSUTIL_MENU_CLOSE:
			PS3_Log("[sysutil] XMB menu close");
			break;

		default:
			PS3_Log("[sysutil] event 0x%llx (param 0x%llx)",
					(unsigned long long)status, (unsigned long long)param);
			break;
	}
}
#endif

static unsigned pump_count;

unsigned
PS3_PumpCount(void)
{
	return pump_count;
}

void
PS3_Pump(void)
{
	pump_count++;

#ifdef __PPU__
	if (sysutil_registered)
	{
		s32 r = sysUtilCheckCallback();
		static int reported;

		if (r != 0 && reported < 5)
		{
			reported++;
			PS3_Log("[sysutil] sysUtilCheckCallback returned 0x%x", (unsigned)r);
		}
	}
#endif
}

int
PS3_ExitRequested(void)
{
	return exit_requested;
}

/* ================================================================ */
/* Leaving                                                            */
/* ================================================================ */

/* Every way out ends here: the engine's exit() is renamed to this
   (ps3/Makefile), and so are the XMB's "Quit Game" and fatal errors.
   Whatever the system calls on its own (vblank and flip handlers, the
   audio thread) is stopped before the process goes, or the whole
   console hangs (handoff notes 3.64). */
void
PS3_Exit(int code)
{
	static int exiting;

	if (exiting++)
	{
		exit(code);
	}

	PS3_Log("[9] exit(%d): shutting down", code);

	Mix_CloseAudio();
	PS3_Video_Shutdown();
	PS3_Pad_Shutdown();

#ifdef __PPU__
	if (sysutil_registered)
	{
		sysUtilUnregisterCallback(0);
		sysutil_registered = 0;
	}
#endif

	PS3_Log("[9] bye");
	PS3_LogShutdown();

	exit(code);
}

/* the engine's exit(), renamed (ps3/Makefile) */
void
PS3_exit(int code)
{
	PS3_Exit(code);
}

void
PS3_abort(void)
{
	PS3_Log("abort() called");
	PS3_Exit(3);
}

/* ================================================================ */
/* Error screen                                                       */
/* ================================================================ */

#define TEXT_COLUMNS 60
#define TEXT_LINES   18

/* Waits for X (a fresh press), the XMB, or 'seconds'. */
static void
WaitForCross(int seconds)
{
	ps3_padstate_t st;
	unsigned last = 0xffffu;
	int ms = 0;

	PS3_Pad_Init();

	while (ms < seconds * 1000 && !exit_requested)
	{
		unsigned now;

		PS3_Pad_Poll(&st);
		now = st.connected ? st.buttons : 0;

		if ((now & ~last) & PS3_PAD_CROSS)
		{
			break;
		}

		last = now;
		usleep(20000);
		ms += 20;
		PS3_Pump();
	}
}

void
PS3_FatalScreen(const char *title, const char *message)
{
	static char buf[TEXT_LINES][TEXT_COLUMNS + 1];
	const char *lines[TEXT_LINES + 4];
	const char *p = message ? message : "";
	int n = 0;

	PS3_Log("[error] %s: %s", title ? title : "", p);

	/* quiet: the game may have died in the middle of a sound */
	if (Mix_QuerySpec(NULL, NULL, NULL))
	{
		Mix_HaltChannel(-1);
		Mix_HaltMusic();
	}

	/* wrap the message at spaces */
	while (*p && n < TEXT_LINES)
	{
		int len = 0, cut = -1;

		while (p[len] && p[len] != '\n' && len < TEXT_COLUMNS)
		{
			if (p[len] == ' ')
			{
				cut = len;
			}

			len++;
		}

		if (p[len] && p[len] != '\n' && cut > 0)
		{
			len = cut;
		}

		memcpy(buf[n], p, (size_t)len);
		buf[n][len] = '\0';
		lines[n] = buf[n];
		n++;

		p += len;

		if (*p == '\n' || *p == ' ')
		{
			p++;
		}
	}

	lines[n++] = "";
	lines[n++] = "Details: USRDIR/rott_log.txt";
	lines[n++] = "";
	lines[n++] = "Press X to go back to the XMB.";

	if (PS3_Video_Init() == 0)
	{
		PS3_Video_TextScreen("RISE OF THE TRIAD", lines, n);
		WaitForCross(60);
	}
}

static void
NoDataScreen(void)
{
	static char found[80];
	const char *lines[16];
	int n = 0;

	snprintf(found, sizeof(found), "Found %s.", PS3_Launcher_Missing());

	lines[n++] = "No complete game data found.";
	lines[n++] = "";

	if (PS3_Launcher_Missing()[0])
	{
		lines[n++] = found;
		lines[n++] = "";
	}

	lines[n++] = "Copy the files of YOUR copy of Rise of the Triad";
	lines[n++] = "(GOG, Steam or CD) by FTP to:";
	lines[n++] = "  " PS3_USRDIR "/";
	lines[n++] = "";
	lines[n++] = "Dark War:  DARKWAR.WAD, DARKWAR.RTL, DARKWAR.RTC,";
	lines[n++] = "           REMOTE1.RTS (and EXTREME.RTL if you have it)";
	lines[n++] = "Shareware: HUNTBGIN.WAD, HUNTBGIN.RTL, HUNTBGIN.RTC,";
	lines[n++] = "           REMOTE1.RTS";
	lines[n++] = "";
	lines[n++] = "Press X to go back to the XMB.";

	PS3_Log("[7] no game data: showing the instructions");

	if (PS3_Video_Init() == 0)
	{
		PS3_Video_TextScreen("RISE OF THE TRIAD", lines, n);
		WaitForCross(120);
	}
}

/* ================================================================ */
/* [3] file system                                                    */
/* ================================================================ */

static int
ProbeDir(const char *path, int list)
{
	DIR *d;
	struct dirent *e;
	int count = 0;

	d = opendir(path);

	if (!d)
	{
		PS3_Log("[3] %s -> can't open", path);
		return -1;
	}

	while ((e = readdir(d)) != NULL)
	{
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
		{
			continue;
		}

		if (list && count < 40)
		{
			PS3_Log("[3]     %s", e->d_name);
		}

		count++;
	}

	closedir(d);
	PS3_Log("[3] %s -> OK, %d entries", path, count);

	return count;
}

static void
ProbeFilesystem(void)
{
	FILE *f;

	PS3_Log("[3] file system");
	ProbeDir(PS3_USRDIR, 1);
#if PS3_LAUNCHER_MODS
	ProbeDir(PS3_MODSDIR, 1);
#endif

	if (mkdir(PS3_DATADIR, 0777) == 0)
	{
		PS3_Log("[3] %s -> created", PS3_DATADIR);
	}

	f = fopen(PS3_DATADIR "/.writetest", "w");

	if (f)
	{
		fputs("ok", f);
		fclose(f);
		remove(PS3_DATADIR "/.writetest");
		PS3_Log("[3] %s -> WRITABLE", PS3_DATADIR);
		ProbeDir(PS3_DATADIR, 1);
	}
	else
	{
		PS3_Log("[3] %s -> NOT writable (config and saves will fail)", PS3_DATADIR);
	}
}

/* ================================================================ */
/* [4] CPU                                                            */
/* ================================================================ */

static void
ProbeCPU(void)
{
	unsigned int one = 1;

	PS3_Log("[4] CPU is %s-endian, sizeof(long)=%d sizeof(void*)=%d, char is %s",
			(*(unsigned char *)&one == 1) ? "little" : "big",
			(int)sizeof(long), (int)sizeof(void *),
			((char)0xff < 0) ? "signed" : "unsigned");
}

/* ================================================================ */
/* The game thread                                                    */
/* ================================================================ */

static void
GameMain(void)
{
	ps3_launch_t launch;
	static char *argv[20];
	int argc = 0, i, r;

	PS3_Log("[1] game thread running");

#ifdef __PPU__
	if (sysUtilRegisterCallback(0, SysutilCallback, NULL) == 0)
	{
		sysutil_registered = 1;
		PS3_Log("[2] sysutil callback registered");
	}
	else
	{
		PS3_Log("[2] WARNING: sysUtilRegisterCallback failed");
	}
#else
	PS3_Log("[2] (host: no sysutil)");
#endif

	ProbeFilesystem();
	ProbeCPU();

	PS3_Log("[5] pad: %s", PS3_Pad_Init() == 0 ? "ok" : "FAILED");

	if (PS3_Video_Init() != 0)
	{
		PS3_Log("[6] FATAL: the RSX didn't start");
		PS3_Exit(1);
	}

	PS3_Video_SetDisplayAspect(4, 3);
	PS3_Video_SetFit(90);
	PS3_Video_SetPerfLog(1);   /* first builds: fps in the log */
	PS3_Log("[6] RSX up: %dx%d", PS3_Video_Width(), PS3_Video_Height());

	r = PS3_Launcher_Run(&launch);

	if (r == -1)
	{
		NoDataScreen();
		PS3_Exit(0);
	}

	if (r != 0)
	{
		PS3_Exit(0);
	}

	PS3_Log("[7] launcher: '%s'", launch.label);

	argv[argc++] = (char *)"rott";

	for (i = 0; i < launch.argc && argc < 19; i++)
	{
		argv[argc++] = (char *)launch.argv[i];
		PS3_Log("[8]   arg %s", launch.argv[i]);
	}

	argv[argc] = NULL;

	PS3_Log("[8] %s engine starts", launch.engine == PS3_ENGINE_SHAREWARE ?
			"shareware" : "registered");

	if (launch.engine == PS3_ENGINE_SHAREWARE)
	{
		r = ROTT_Main_Shareware(argc, argv);
	}
	else
	{
		r = ROTT_Main_Registered(argc, argv);
	}

	PS3_Log("[8] the engine returned %d", r);
	PS3_Exit(r);
}

#ifdef __PPU__

static void
GameThread(void *arg)
{
	(void)arg;
	GameMain();
	sysThreadExit(0);
}

int
main(int argc, char **argv)
{
	sys_ppu_thread_t tid;
	u64 code = 0;
	s32 r;

	(void)argc;
	(void)argv;

	PS3_LogInit();
	PS3_Log("[0] main() reached (%s)", BUILD_DATE);

	r = sysThreadCreate(&tid, GameThread, NULL, 1000, GAME_STACK_SIZE,
			THREAD_JOINABLE, "rottgame");

	if (r != 0)
	{
		PS3_Log("[0] FATAL: sysThreadCreate failed (%d)", (int)r);
		PS3_LogShutdown();
		return 1;
	}

	sysThreadJoin(tid, &code);

	PS3_Exit((int)code);
}

#else /* host harness */

void
PS3_Host_RequestExit(void)
{
	if (!exit_requested)
	{
		PS3_Log("[host] XMB \"Quit Game\" simulated");
	}

	exit_requested = 1;
}

static void *
HostThread(void *arg)
{
	(void)arg;
	GameMain();
	return NULL;
}

int
main(int argc, char **argv)
{
	pthread_attr_t attr;
	pthread_t tid;

	(void)argc;
	(void)argv;

	PS3_LogInit();
	PS3_Log("[0] main() reached (host harness, %s)", BUILD_DATE);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, GAME_STACK_SIZE);
	pthread_create(&tid, &attr, HostThread, NULL);
	pthread_join(tid, NULL);

	PS3_Exit(0);
}

#endif
