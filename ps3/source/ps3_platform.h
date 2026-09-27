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
 * PS3 (PSL1GHT) platform layer -- shared declarations.
 *
 * Everything that talks to PSL1GHT lives behind __PPU__. Without it the
 * same files build against plain POSIX, which is what the host harness
 * (ps3/hosttest: x86, or powerpc64 big-endian under qemu) uses.
 *
 * The engine is linked twice into one binary (registered and shareware,
 * see ps3/Makefile), so this layer never calls into the engine: the
 * engine calls it, through the SDL shim and the few functions below.
 *
 * =======================================================================
 */

#ifndef PS3_PLATFORM_H
#define PS3_PLATFORM_H

#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef PS3_APPID
#define PS3_APPID "RISETRIAD"
#endif

/* Game data is uploaded here by FTP. Wiped when the PKG is uninstalled. */
#ifndef PS3_USRDIR
#define PS3_USRDIR "/dev_hdd0/game/" PS3_APPID "/USRDIR"
#endif

/* Config and savegames: survives reinstalling the PKG. */
#ifndef PS3_DATADIR
#define PS3_DATADIR "/dev_hdd0/data/rott"
#endif

#define PS3_MODSDIR     PS3_USRDIR "/mods"
/* The launcher's MODS tab: off in 1.0 (the classic games only), for 1.1 */
#define PS3_LAUNCHER_MODS 0
#define PS3_LOGFILE     PS3_USRDIR "/rott_log.txt"
#define PS3_LOGFILE_OLD PS3_USRDIR "/rott_log.old.txt"

/* ps3_log.c */
void PS3_LogInit(void);
void PS3_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void PS3_LogRaw(const char *text);   /* as is, no newline added */
void PS3_LogShutdown(void);

/* ps3_main.c */
void PS3_Pump(void);                 /* sysutil queue; cheap, call often */
int  PS3_ExitRequested(void);        /* "Quit Game" from the XMB */
void PS3_Exit(int code) __attribute__((noreturn));
void PS3_FatalScreen(const char *title, const char *message);

/* The engine's stdout/stderr go to the log: the engine objects have
   printf, fprintf, puts... renamed to these (ps3/Makefile). */
int PS3_printf(const char *fmt, ...);
int PS3_fprintf(FILE *f, const char *fmt, ...);
int PS3_vfprintf(FILE *f, const char *fmt, va_list ap);
int PS3_vprintf(const char *fmt, va_list ap);
int PS3_puts(const char *s);
int PS3_putchar(int c);
int PS3_fputs(const char *s, FILE *f);
int PS3_fputc(int c, FILE *f);
int PS3_putc(int c, FILE *f);
size_t PS3_fwrite(const void *p, size_t size, size_t n, FILE *f);
int PS3_fflush(FILE *f);
void PS3_perror(const char *s);
void PS3_abort(void) __attribute__((noreturn));
void PS3_exit(int code) __attribute__((noreturn));

#ifndef __PPU__
void PS3_Host_RequestExit(void);     /* host harness: simulated XMB quit */
#endif

/* ps3_video.c: the picture keeps this aspect on the TV (ROTT's 320x200
   and 640x480 are both 4:3 on a CRT). */
void PS3_Video_SetDisplayAspect(int num, int den);

/* ps3_input.c: the DualShock 3.
 *
 * Two modes. While the engine is playing (it calls PS3_Input_Game every
 * tic from PollControls) the pad drives the game directly: buttons and
 * sticks, no key events. Otherwise (menus, automap, screens that wait
 * for a key) the pad types: d-pad/left stick = arrows, X = Enter,
 * O and START = Escape, with auto-repeat. Buttons held across a switch
 * are ignored until released (so the X that closes a menu doesn't fire
 * in the game). */
typedef struct
{
	unsigned buttons;       /* PS3_PAD_* bits, masked as above */
	unsigned pressed;       /* went down since the previous call */
	int lx, ly, rx, ry;     /* -32767..32767 after the dead zone */
	int connected;
} ps3_gamepad_t;

void PS3_Input_Game(ps3_gamepad_t *out);
void PS3_Input_Pump(void);           /* called by SDL_PollEvent */
void PS3_Input_SetDeadzone(int percent);
void PS3_Input_SetMapButton(unsigned bit);
void PS3_Input_Capture(int on);
unsigned PS3_Input_CapturePressed(void);

/* sdl_ps3.c: frame rate in the top right corner; a microsecond clock */
void PS3_Video_SetShowFPS(int on);
unsigned long long PS3_Micros(void);

/* ps3_video.c: time the engine spent waiting for its next tic (35 per
   second), for the fps line in the log */
void PS3_Video_ProfileIdle(long long usec);

/* ps3_video.c: color of the TV's bars around the picture (0xAARRGGBB).
   In DOS the damage flash colored the VGA's overscan border: here it
   colors the bars. */
void PS3_Video_SetBorderColor(unsigned int argb);

/* sdl_ps3.c: the same, as one of the game's palette colors (0 = black) */
void PS3_SetBorderIndex(int index);

/* sdl_ps3.c: large HUD. While on, the 640x480 screen's status bars (the
   top one, 320x16 at the top left; the bottom one, 16 rows split to both
   ends) are shown at 2x, like in 320x200. Only in the image sent to the
   TV: the game's own buffer is untouched. */
void PS3_SetHudScale(int top, int bottom);

/* ps3_main.c: sysutil diagnostics for the log */
unsigned PS3_PumpCount(void);

/* Options > Music Synth (ps3_sf2.c): 1 = General MIDI through the first
   .sf2 in USRDIR (loaded now), 0 = the AdLib driver. Returns what's in
   use afterwards. */
int PS3_Music_UseSoundFont(int on);
const char *PS3_Music_SoundFontName(void);   /* NULL: none loaded */

#endif
