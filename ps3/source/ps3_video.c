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
 * PS3 display. Put together from the two versions proven on hardware:
 *
 *  - RSX/video init, framebuffers, flip handler and the text screen come
 *    from Doom64-PS3 (ps3_video.c).
 *  - The scaled blit (rsxSetTransferScaleSurface, fixed function 2D unit,
 *    no shaders) and its flip sequence come from TyrQuakeCell (vid_ps3.c).
 *
 * The engine draws 8 bit pixels; sdl_ps3.c turns them into 32 bit ones
 * (through the palette) straight into a source image in RSX memory
 * (SDL_LockTexture + SDL_BlitSurface). There is one source image per
 * framebuffer, so the CPU never writes into an image the RSX may still
 * be reading: image N is written again only after the flip that used it
 * three frames ago has completed.
 *
 * Unlike the Quake II port there is no OpenGL mode and no shader pass:
 * brightness is the game's own gamma (palette), and the picture always
 * goes to the TV through the transfer scaler.
 *
 * Rules learned the hard way (handoff notes):
 *  - rsxInit twice leaks the previous context: init is idempotent.
 *  - Shutdown: gcmSetFlipHandler(NULL) FIRST, then free.
 *  - This file is built at -O1 -fno-inline (see ps3/Makefile): PSL1GHT's
 *    syscall stubs don't survive aggressive inlining.
 *  - Never read RSX memory from the CPU (very slow); only write it.
 *
 * =======================================================================
 */

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <lv2/systime.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sysutil/video.h>

#include "ps3_platform.h"
#include "ps3_video.h"
#include "ps3_font.h"

#define FB_COUNT      3
#define FB_ALIGN      64
#define RSX_CB_SIZE   (1 * 1024 * 1024)     /* command buffer */
#define RSX_IO_SIZE   (32 * 1024 * 1024)    /* IO buffer (Doom64-PS3 value) */

static gcmContextData *ctx;
static void *io_buffer;
static int ready;

static u32 disp_w = 1280, disp_h = 720, disp_pitch;

static u32 *fb[FB_COUNT];
static u32 fb_offset[FB_COUNT];
static int cur;

static volatile u32 flip_queued, flip_completed;

static u32 *src[FB_COUNT];
static u32 src_offset[FB_COUNT];
static int src_w, src_h;
static u32 src_pitch;

static int clear_frames;     /* framebuffers that still need black borders */
static u32 border_argb = 0xff000000u;   /* color of the bars (damage flash) */
static u32 fb_border[FB_COUNT];         /* what each framebuffer's bars have */
static int aspect_num = 4, aspect_den = 3;   /* of the picture on the TV */
static int perf_log;         /* fps and timings in the log every 10 s */
static int fit_percent = 90;
static int filter_linear = 1;

static volatile u32 vblank_count;
static u32 last_flip_vblank;
static int lock30;

/* profiling, reset every 10 seconds */
static u64 prof_copy_us, prof_wait_us, prof_lock_us, prof_idle_us;

/* ================================================================ */

static void
FlipHandler(const u32 head)
{
	(void)head;
	flip_completed++;
}

static void
VBlankHandler(const u32 head)
{
	(void)head;
	vblank_count++;
}

/* Blocks only while both framebuffers that are not being built are still
   in flight. Afterwards the one at 'cur' is free: nothing displays it and
   the RSX has finished the blit into it (and read its source image). */
static void
WaitFlips(void)
{
	int waited = 0;

	while ((int)(flip_queued - flip_completed) > FB_COUNT - 2)
	{
		usleep(100);

		if (++waited > 20000)
		{
			PS3_Log("[video] WARNING: flip fence timed out, force-sync");
			flip_completed = flip_queued;
			break;
		}
	}
}

/* Waits until the RSX has done everything queued so far. rsxFinish(ctx, v)
   waits for the RSX's reference register to become v: with the same v
   every time it returns at once from the second call on (the register
   already holds it), and memory still in use gets freed (TyrQuakeCell
   crashed the console on the second resolution change). A new value each
   time. */
void
PS3_Video_Finish(void)
{
	static u32 ref = 0x1000;

	if (!ctx)
	{
		return;
	}

	ref++;

	if (ref == 0 || ref == 0xffffffffu)
	{
		ref = 0x1000;
	}

	rsxFinish(ctx, ref);
}

int
PS3_Video_Ready(void)
{
	return ready;
}

int
PS3_Video_Width(void)
{
	return (int)disp_w;
}

int
PS3_Video_Height(void)
{
	return (int)disp_h;
}

int
PS3_Video_Init(void)
{
	s32 ret;
	s32 vid_res = VIDEO_RESOLUTION_720;
	u8 vid_aspect = VIDEO_ASPECT_16_9;
	videoResolution res;
	videoConfiguration vconfig;
	videoState state;
	int i, waited;

	if (ready)
	{
		return 0;
	}

	PS3_Log("[video] init");

	io_buffer = memalign(1024 * 1024, RSX_IO_SIZE);

	if (!io_buffer)
	{
		PS3_Log("[video] FATAL: memalign failed for the %d MB IO buffer", RSX_IO_SIZE >> 20);
		return -1;
	}

	ret = rsxInit(&ctx, RSX_CB_SIZE, RSX_IO_SIZE, io_buffer);

	if (ret != 0 || !ctx)
	{
		PS3_Log("[video] FATAL: rsxInit failed (%d)", (int)ret);
		return -1;
	}

	/* 720p if the TV takes it; otherwise whatever the TV is set to. */
	if (!videoGetResolutionAvailability(VIDEO_PRIMARY, VIDEO_RESOLUTION_720,
				VIDEO_ASPECT_16_9, 0))
	{
		videoGetState(0, 0, &state);
		vid_res = state.displayMode.resolution;
		vid_aspect = state.displayMode.aspect;
		PS3_Log("[video] 720p not available, using the TV's mode (%d)", (int)vid_res);
	}

	if (videoGetResolution(vid_res, &res) != 0)
	{
		PS3_Log("[video] FATAL: videoGetResolution failed");
		return -1;
	}

	disp_w = res.width;
	disp_h = res.height;
	disp_pitch = disp_w * 4;

	memset(&vconfig, 0, sizeof(vconfig));
	vconfig.resolution = vid_res;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = disp_pitch;
	vconfig.aspect = vid_aspect;

	if (videoConfigure(0, &vconfig, NULL, 0) != 0)
	{
		PS3_Log("[video] FATAL: videoConfigure failed");
		return -1;
	}

	/* wait until the video out is no longer busy switching modes */
	waited = 0;

	do
	{
		usleep(10000);

		if (videoGetState(0, 0, &state) != 0)
		{
			break;
		}
	}
	while (state.state == 3 && ++waited < 300);

	gcmSetFlipMode(GCM_FLIP_VSYNC);

	for (i = 0; i < FB_COUNT; i++)
	{
		fb[i] = (u32 *)rsxMemalign(FB_ALIGN, disp_pitch * disp_h);

		if (!fb[i])
		{
			PS3_Log("[video] FATAL: rsxMemalign failed for framebuffer %d", i);
			return -1;
		}

		memset(fb[i], 0, disp_pitch * disp_h);
		rsxAddressToOffset(fb[i], &fb_offset[i]);
		gcmSetDisplayBuffer(i, fb_offset[i], disp_pitch, disp_w, disp_h);
	}

	cur = 0;
	flip_queued = 0;
	flip_completed = 0;
	clear_frames = FB_COUNT;

	gcmSetFlipHandler(FlipHandler);
	gcmSetVBlankHandler(VBlankHandler);

	ready = 1;
	PS3_Log("[video] ready: %ux%u, %d framebuffers", (unsigned)disp_w,
			(unsigned)disp_h, FB_COUNT);

	return 0;
}

void
PS3_Video_FreeSource(void)
{
	int i;

	if (ready)
	{
		PS3_Video_Finish();
	}

	for (i = 0; i < FB_COUNT; i++)
	{
		if (src[i])
		{
			rsxFree(src[i]);
			src[i] = NULL;
		}
	}

	src_w = src_h = 0;
}

void
PS3_Video_Shutdown(void)
{
	int i;

	if (!ready)
	{
		return;
	}

	PS3_Log("[video] shutdown");

	/* ORDER MATTERS: unhook the handlers first. */
	gcmSetFlipHandler(NULL);
	gcmSetVBlankHandler(NULL);
	PS3_Video_Finish();

	PS3_Video_FreeSource();

	for (i = 0; i < FB_COUNT; i++)
	{
		if (fb[i])
		{
			rsxFree(fb[i]);
			fb[i] = NULL;
		}
	}

	ready = 0;
}

/* Allocates the source images: w x h, 'pitch' bytes per row. */
static int
AllocSource(int w, int h, u32 pitch)
{
	int i;

	PS3_Video_FreeSource();

	for (i = 0; i < FB_COUNT; i++)
	{
		src[i] = (u32 *)rsxMemalign(FB_ALIGN, pitch * h);

		if (!src[i])
		{
			PS3_Log("[video] FATAL: rsxMemalign failed for a %dx%d source image", w, h);
			PS3_Video_FreeSource();
			return -1;
		}

		memset(src[i], 0, pitch * h);
		rsxAddressToOffset(src[i], &src_offset[i]);
	}

	src_w = w;
	src_h = h;
	src_pitch = pitch;
	clear_frames = FB_COUNT;   /* the picture's size changed: redo borders */

	return 0;
}

int
PS3_Video_SetSource(int w, int h)
{
	if (!ready || w <= 0 || h <= 0)
	{
		return -1;
	}

	if (src[0] && w == src_w && h == src_h)
	{
		return 0;
	}

	if (AllocSource(w, h, (u32)w * 4) != 0)
	{
		return -1;
	}

	PS3_Log("[video] source image %dx%d (x%d)", w, h, FB_COUNT);

	return 0;
}

uint32_t *
PS3_Video_Source(void)
{
	if (!ready || !src[cur])
	{
		return NULL;
	}

	WaitFlips();

	return src[cur];
}

void
PS3_Video_SetFit(int percent)
{
	if (percent < 50)
	{
		percent = 50;
	}

	if (percent > 100)
	{
		percent = 100;
	}

	if (percent != fit_percent)
	{
		fit_percent = percent;
		clear_frames = FB_COUNT;
	}
}

void
PS3_Video_SetFilter(int linear)
{
	filter_linear = linear ? 1 : 0;
}

void
PS3_Video_SetPerfLog(int on)
{
	perf_log = on ? 1 : 0;
}

void
PS3_Video_SetLock30(int lock)
{
	lock30 = lock ? 1 : 0;
}

void
PS3_Video_SetBorderColor(unsigned int argb)
{
	border_argb = 0xff000000u | argb;
}

/* The bars around the picture (not the picture: the blit writes it). */
static void
FillBars(u32 *dst, int off_x, int off_y, int out_w, int out_h, u32 c)
{
	int x, y, w = (int)disp_w, h = (int)disp_h;
	u32 stride = disp_pitch / 4;

	for (y = 0; y < h; y++)
	{
		u32 *row = dst + (u32)y * stride;

		if (y < off_y || y >= off_y + out_h)
		{
			for (x = 0; x < w; x++)
			{
				row[x] = c;
			}
		}
		else
		{
			for (x = 0; x < off_x; x++)
			{
				row[x] = c;
			}

			for (x = off_x + out_w; x < w; x++)
			{
				row[x] = c;
			}
		}
	}
}

void
PS3_Video_ProfileIdle(long long usec)
{
	if (usec > 0)
	{
		prof_idle_us += (u64)usec;
	}
}

void
PS3_Video_ProfileCopy(long long usec)
{
	if (usec > 0)
	{
		prof_copy_us += (u64)usec;
	}
}

void
PS3_Video_SetDisplayAspect(int num, int den)
{
	if (num <= 0 || den <= 0)
	{
		num = 4;
		den = 3;
	}

	if (num != aspect_num || den != aspect_den)
	{
		aspect_num = num;
		aspect_den = den;
		clear_frames = FB_COUNT;
	}
}

void
PS3_Video_Present(void)
{
	static int frames;
	gcmTransferScale scale;
	gcmTransferSurface surface;
	int avail_w, avail_h, out_w, out_h, off_x, off_y;

	u64 t0;

	if (!ready || !src[cur])
	{
		return;
	}

	t0 = sysGetSystemTime();
	WaitFlips();
	prof_wait_us += sysGetSystemTime() - t0;

	/* Keep the picture's display aspect (4:3: ROTT's 320x200 has tall
	   pixels, as on a CRT), inside the screen fit area, centered. On a
	   16:9 TV that means black bars at the sides. The TV modes have
	   square pixels (1280x720, 1920x1080; 720x480 is close enough). */
	avail_w = (int)disp_w * fit_percent / 100;
	avail_h = (int)disp_h * fit_percent / 100;
	out_h = avail_h;
	out_w = out_h * aspect_num / aspect_den;

	if (out_w > avail_w)
	{
		out_w = avail_w;
		out_h = out_w * aspect_den / aspect_num;
	}

	off_x = ((int)disp_w - out_w) / 2;
	off_y = ((int)disp_h - out_h) / 2;

	if (clear_frames > 0)
	{
		/* the picture's rectangle moved: redo the whole framebuffer */
		if (border_argb == 0xff000000u)
		{
			memset(fb[cur], 0, disp_pitch * disp_h);
		}
		else
		{
			FillBars(fb[cur], 0, 0, 0, 0, border_argb);
		}

		fb_border[cur] = border_argb;
		clear_frames--;
	}
	else if (fb_border[cur] != border_argb)
	{
		/* damage flash: only the bars change */
		FillBars(fb[cur], off_x, off_y, out_w, out_h, border_argb);
		fb_border[cur] = border_argb;
	}

	memset(&scale, 0, sizeof(scale));
	scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
	scale.format = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
	scale.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
	scale.clipX = off_x;
	scale.clipY = off_y;
	scale.clipW = out_w;
	scale.clipH = out_h;
	scale.outX = off_x;
	scale.outY = off_y;
	scale.outW = out_w;
	scale.outH = out_h;
	scale.ratioX = rsxGetFixedSint32((float)src_w / (float)out_w);
	scale.ratioY = rsxGetFixedSint32((float)src_h / (float)out_h);
	scale.inW = src_w;
	scale.inH = src_h;
	scale.pitch = src_pitch;
	scale.origin = GCM_TRANSFER_ORIGIN_CORNER;
	scale.interp = filter_linear ? GCM_TRANSFER_INTERPOLATOR_LINEAR :
		GCM_TRANSFER_INTERPOLATOR_NEAREST;
	scale.offset = src_offset[cur];
	scale.inX = 0;
	scale.inY = 0;

	memset(&surface, 0, sizeof(surface));
	surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
	surface.pitch = disp_pitch;
	surface.offset = fb_offset[cur];

	/* The CPU's writes must be in memory before the RSX reads them. */
	__asm__ volatile("sync" ::: "memory");

	rsxSetTransferScaleSurface(ctx, &scale, &surface);

	/* Locked 30: flip every second vblank. The flip below happens on the
	   vblank after it is queued, so queue it once 2 have gone by since the
	   previous one. */
	if (lock30)
	{
		int waited = 0;

		t0 = sysGetSystemTime();

		while ((u32)(vblank_count - last_flip_vblank) < 2 && ++waited < 1000)
		{
			usleep(100);
		}

		prof_lock_us += sysGetSystemTime() - t0;
	}

	last_flip_vblank = vblank_count;

	/* TyrQuakeCell's sequence: flip, flush, wait-flip. */
	gcmSetFlip(ctx, cur);
	rsxFlushBuffer(ctx);
	gcmSetWaitFlip(ctx);

	flip_queued++;
	cur = (cur + 1) % FB_COUNT;

	if (frames < 3)
	{
		PS3_Log("[video] frame %d presented: %dx%d -> %dx%d at %d,%d", frames,
				src_w, src_h, out_w, out_h, off_x, off_y);
	}

	frames++;

	/* Frame rate in the log every 10 seconds. Not more often: every write
	   to the HDD flashes the system's activity icon. */
	{
		static u64 window_start;
		static int window_frames;
		u64 now = sysGetSystemTime();

		if (window_start == 0)
		{
			window_start = now;
		}

		window_frames++;

		if (now - window_start >= 10000000ull && !perf_log)
		{
			/* off: just start a new window */
			window_start = now;
			window_frames = 0;
			prof_copy_us = prof_wait_us = prof_lock_us = prof_idle_us = 0;
		}
		else if (now - window_start >= 10000000ull)
		{
			double ms = (double)(now - window_start) / 1000.0 / window_frames;

			/* frame = everything; copy = palette -> RSX memory; wait =
			   waiting for a free framebuffer; lock = 30 fps pacing; idle
			   = the game waiting for its next tic (35 a second). What a
			   frame really costs (game + renderer) = frame - idle. */
			static unsigned last_pumps;
			unsigned pumps = PS3_PumpCount();

			/* pumps: sysutil queue checks in these 10 s (diagnostics for
			   "Quit Game" from the XMB) */
			PS3_Log("[video] %.1f fps (%dx%d%s): frame %.1f ms, idle %.1f, copy %.1f, wait %.1f, lock %.1f, pumps %u",
					1000.0 / ms, src_w, src_h, lock30 ? ", locked 30" : "", ms,
					prof_idle_us / 1000.0 / window_frames,
					prof_copy_us / 1000.0 / window_frames,
					prof_wait_us / 1000.0 / window_frames,
					prof_lock_us / 1000.0 / window_frames, pumps - last_pumps);

			last_pumps = pumps;
			window_start = now;
			window_frames = 0;
			prof_copy_us = prof_wait_us = prof_lock_us = prof_idle_us = 0;
		}
	}
}

/* ================================================================ */
/* Text screen, drawn by the CPU straight into a framebuffer.         */
/* ================================================================ */

#define TXT_SCALE 2

static void
TxtRect(u32 *dst, int x, int y, int w, int h, u32 c)
{
	int i, j;

	if (x < 0)
	{
		w += x;
		x = 0;
	}

	if (y < 0)
	{
		h += y;
		y = 0;
	}

	if (x + w > (int)disp_w)
	{
		w = (int)disp_w - x;
	}

	if (y + h > (int)disp_h)
	{
		h = (int)disp_h - y;
	}

	for (j = 0; j < h; j++)
	{
		u32 *row = dst + (y + j) * (disp_pitch / 4) + x;

		for (i = 0; i < w; i++)
		{
			row[i] = c;
		}
	}
}

static void
TxtString(u32 *dst, int x, int y, const char *str, u32 c)
{
	for (; *str; str++, x += 8 * TXT_SCALE)
	{
		unsigned ch = (unsigned char)*str;
		int gy, gx;

		if (ch < 32 || ch > 126)
		{
			ch = '?';
		}

		for (gy = 0; gy < 16; gy++)
		{
			unsigned bits = ps3_font8x16[ch - 32][gy];

			if (!bits)
			{
				continue;
			}

			for (gx = 0; gx < 8; gx++)
			{
				if (bits & (0x80u >> gx))
				{
					TxtRect(dst, x + gx * TXT_SCALE, y + gy * TXT_SCALE,
							TXT_SCALE, TXT_SCALE, c);
				}
			}
		}
	}
}

void
PS3_Video_TextScreen(const char *title, const char *const *lines, int nlines)
{
	u32 *dst;
	int i, y;

	if (!ready)
	{
		return;
	}

	WaitFlips();
	PS3_Video_Finish();

	dst = fb[cur];
	TxtRect(dst, 0, 0, (int)disp_w, (int)disp_h, 0xff000000);

	y = (int)disp_h / 5;

	if (title)
	{
		int tw = (int)strlen(title) * 8 * TXT_SCALE;

		TxtString(dst, ((int)disp_w - tw) / 2, y, title, 0xffd02020);
		y += 16 * TXT_SCALE * 2;
	}

	for (i = 0; i < nlines; i++)
	{
		if (lines[i])
		{
			TxtString(dst, (int)disp_w / 10, y, lines[i], 0xffe0e0e0);
		}

		y += 16 * TXT_SCALE + 8;
	}

	__asm__ volatile("sync" ::: "memory");
	gcmSetFlip(ctx, cur);
	rsxFlushBuffer(ctx);
	gcmSetWaitFlip(ctx);

	PS3_Log("[video] text screen on framebuffer %d (flips queued %u, done %u): %s",
			cur, (unsigned)flip_queued + 1, (unsigned)flip_completed, title ? title : "");

	flip_queued++;
	cur = (cur + 1) % FB_COUNT;
	clear_frames = FB_COUNT;   /* the game's frames must wipe the text */

	/* Did the flip actually happen? (diagnostics, stage 2) */
	{
		int i;

		for (i = 0; i < 100 && flip_completed < flip_queued; i++)
		{
			usleep(1000);
		}

		PS3_Log("[video] text screen flip %s after %d ms",
				flip_completed >= flip_queued ? "completed" : "NOT completed", i);
	}
}
