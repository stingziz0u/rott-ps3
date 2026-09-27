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
 * The SDL 2 shim (ps3/compat/SDL.h) on the PS3:
 *
 *  - Video: ROTT draws 8 bit pixels into an SDL_Surface. The screen
 *    texture's "pixels" are the RSX source image of the frame being built
 *    (SDL_LockTexture -> PS3_Video_Source), so SDL_BlitSurface writes the
 *    32 bit pixels (through the palette) straight into RSX memory, and
 *    SDL_RenderPresent scales the image to the TV (PS3_Video_Present).
 *    One copy per frame, no intermediate buffer.
 *  - Events: a small queue. The DualShock 3 fills it with key events
 *    when the game isn't reading the pad directly (ps3_input.c), and
 *    "Quit Game" from the XMB with SDL_QUIT.
 *  - No mouse, no SDL joystick (the pad goes through ps3_input.c).
 *  - Paths: the base path is USRDIR (game data), the pref path is
 *    /dev_hdd0/data/rott (config and saves, survives reinstalling).
 *
 * The host harness (ps3/hosttest) builds this same file without __PPU__.
 *
 * =======================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "SDL.h"
#include "ps3_platform.h"
#include "ps3_video.h"
#include "ps3_font.h"

#ifdef __PPU__
#include <lv2/systime.h>
#else
#include <time.h>
#endif

static const char *last_error = "";

/* ================================================================ */
/* Init, errors, time                                                 */
/* ================================================================ */

static Uint32 init_flags;

int
SDL_Init(Uint32 flags)
{
	init_flags |= flags;
	return 0;
}

int
SDL_InitSubSystem(Uint32 flags)
{
	return SDL_Init(flags);
}

Uint32
SDL_WasInit(Uint32 flags)
{
	return init_flags & (flags ? flags : 0xffffffffu);
}

void
SDL_QuitSubSystem(Uint32 flags)
{
	init_flags &= ~flags;
}

void
SDL_Quit(void)
{
	init_flags = 0;
}

const char *
SDL_GetError(void)
{
	return last_error;
}

SDL_bool
SDL_SetHint(const char *name, const char *value)
{
	(void)name;
	(void)value;
	return SDL_TRUE;
}

void
SDL_free(void *mem)
{
	free(mem);
}

static Uint64
NowMicro(void)
{
#ifdef __PPU__
	return sysGetSystemTime();
#else
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (Uint64)ts.tv_sec * 1000000ull + (Uint64)ts.tv_nsec / 1000ull;
#endif
}

static Uint64 time_base;

unsigned long long
PS3_Micros(void)
{
	return NowMicro();
}

Uint64
SDL_GetTicks64(void)
{
	Uint64 now = NowMicro();

	if (time_base == 0)
	{
		time_base = now;
	}

	return (now - time_base) / 1000ull;
}

Uint32
SDL_GetTicks(void)
{
	return (Uint32)SDL_GetTicks64();
}

void
SDL_Delay(Uint32 ms)
{
	/* Long waits in slices, so the XMB's "Quit Game" is still noticed. */
	while (ms > 0)
	{
		Uint32 slice = ms > 20 ? 20 : ms;

		usleep(slice * 1000);
		ms -= slice;
		PS3_Pump();
	}
}

char *
SDL_GetBasePath(void)
{
	return strdup(PS3_USRDIR "/");
}

char *
SDL_GetPrefPath(const char *org, const char *app)
{
	(void)org;
	(void)app;

	mkdir(PS3_DATADIR, 0777);
	return strdup(PS3_DATADIR "/");
}

int
SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message,
		SDL_Window *window)
{
	(void)flags;
	(void)window;

	PS3_FatalScreen(title, message);
	return 0;
}

/* ================================================================ */
/* Surfaces                                                           */
/* ================================================================ */

static SDL_Surface *
NewSurface(int w, int h, int bpp, Uint32 format, void *pixels, int pitch)
{
	SDL_Surface *s = calloc(1, sizeof(*s));
	SDL_PixelFormat *f = calloc(1, sizeof(*f));

	if (!s || !f)
	{
		free(s);
		free(f);
		last_error = "out of memory";
		return NULL;
	}

	f->format = format;
	f->BitsPerPixel = (Uint8)bpp;
	f->BytesPerPixel = (Uint8)(bpp / 8);
	s->format = f;
	s->w = w;
	s->h = h;
	s->clip_rect.w = w;
	s->clip_rect.h = h;
	s->refcount = 1;

	if (bpp == 8)
	{
		SDL_Palette *pal = calloc(1, sizeof(*pal));
		SDL_Color *colors = calloc(256, sizeof(SDL_Color));
		int i;

		if (!pal || !colors)
		{
			free(pal);
			free(colors);
			free(f);
			free(s);
			last_error = "out of memory";
			return NULL;
		}

		/* SDL's default: a grey ramp, opaque */
		for (i = 0; i < 256; i++)
		{
			colors[i].r = colors[i].g = colors[i].b = (Uint8)i;
			colors[i].a = 255;
		}

		pal->ncolors = 256;
		pal->colors = colors;
		pal->version = 1;
		pal->refcount = 1;
		f->palette = pal;
	}

	if (pixels)
	{
		s->pixels = pixels;
		s->pitch = pitch;
		s->flags |= 1;   /* SDL_PREALLOC: not ours to free */
	}
	else if (w > 0 && h > 0 && bpp > 0)
	{
		s->pitch = w * (bpp / 8);
		s->pixels = calloc((size_t)s->pitch * h, 1);

		if (!s->pixels)
		{
			SDL_FreeSurface(s);
			last_error = "out of memory";
			return NULL;
		}
	}

	return s;
}

SDL_Surface *
SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
		Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
	(void)flags;
	(void)Rmask;
	(void)Gmask;
	(void)Bmask;
	(void)Amask;

	if (depth != 8 && depth != 32)
	{
		PS3_Log("[sdl] SDL_CreateRGBSurface: %d bpp not supported", depth);
		last_error = "unsupported depth";
		return NULL;
	}

	/* 32 bit surfaces are always ARGB8888 here, whatever the masks say:
	   the only format SDL_GetWindowPixelFormat hands out. */
	return NewSurface(width, height, depth,
			depth == 8 ? SDL_PIXELFORMAT_INDEX8 : SDL_PIXELFORMAT_ARGB8888,
			NULL, 0);
}

SDL_Surface *
SDL_CreateRGBSurfaceWithFormatFrom(void *pixels, int width, int height,
		int depth, int pitch, Uint32 format)
{
	SDL_Surface *s;

	(void)depth;

	if (format != SDL_PIXELFORMAT_ARGB8888)
	{
		last_error = "unsupported format";
		return NULL;
	}

	/* pixels may be NULL: modexlib.c points it at the locked texture */
	s = NewSurface(width, height, 32, format, NULL, 0);

	if (s)
	{
		s->pixels = pixels;
		s->pitch = pitch ? pitch : width * 4;
		s->flags |= 1;
	}

	return s;
}

void
SDL_FreeSurface(SDL_Surface *s)
{
	if (!s)
	{
		return;
	}

	if (!(s->flags & 1))
	{
		free(s->pixels);
	}

	if (s->format)
	{
		if (s->format->palette)
		{
			free(s->format->palette->colors);
			free(s->format->palette);
		}

		free(s->format);
	}

	free(s);
}

int
SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors,
		int firstcolor, int ncolors)
{
	int i;

	if (!palette || !colors || firstcolor < 0)
	{
		return -1;
	}

	for (i = 0; i < ncolors && firstcolor + i < palette->ncolors; i++)
	{
		palette->colors[firstcolor + i] = colors[i];
		palette->colors[firstcolor + i].a = 255;
	}

	palette->version++;

	return 0;
}

/* Clips a blit of w x h from (sx, sy) to (dx, dy) against both surfaces. */
static int
ClipBlit(const SDL_Surface *src, const SDL_Surface *dst, int *sx, int *sy,
		int *dx, int *dy, int *w, int *h)
{
	if (*sx < 0) { *w += *sx; *dx -= *sx; *sx = 0; }
	if (*sy < 0) { *h += *sy; *dy -= *sy; *sy = 0; }
	if (*dx < 0) { *w += *dx; *sx -= *dx; *dx = 0; }
	if (*dy < 0) { *h += *dy; *sy -= *dy; *dy = 0; }
	if (*sx + *w > src->w) *w = src->w - *sx;
	if (*sy + *h > src->h) *h = src->h - *sy;
	if (*dx + *w > dst->w) *w = dst->w - *dx;
	if (*dy + *h > dst->h) *h = dst->h - *dy;

	return *w > 0 && *h > 0;
}

/* The palette as ready-made 32 bit pixels, rebuilt when it changes. */
static Uint32 lut[256];
static const SDL_Palette *lut_palette;
static Uint32 lut_version;

static void
UpdateLut(const SDL_Palette *pal)
{
	int i;

	if (pal == lut_palette && pal->version == lut_version)
	{
		return;
	}

	for (i = 0; i < 256; i++)
	{
		const SDL_Color *c = &pal->colors[i];

		lut[i] = 0xff000000u | ((Uint32)c->r << 16) | ((Uint32)c->g << 8) | c->b;
	}

	lut_palette = pal;
	lut_version = pal->version;
}

/* ---------------------------------------------------------------- */
/* Large HUD (Video Settings)                                          */
/* ---------------------------------------------------------------- */

/* At 640x480 the engine draws its 320 pixel wide status bars at their
   original size (bna's "fix": the top bar plus filler, and two copies
   of the bottom bar with the icons in the middle erased). The option
   redraws them at 2x while the screen goes out: the top bar is its
   left half, the bottom bar is the left quarter (health) and the right
   quarter (ammo) put back together -- the 320x200 layout, doubled.
   The engine says when (modexlib.c): in game, 640x480, bars on. */
static int hud_top, hud_bottom;

void
PS3_SetHudScale(int top, int bottom)
{
	hud_top = top;
	hud_bottom = bottom;
}

static void
Double8(const Uint8 *s, Uint32 *d0, Uint32 *d1, int n)
{
	int x;

	for (x = 0; x < n; x++)
	{
		Uint32 p = lut[s[x]];

		d0[2 * x] = p; d0[2 * x + 1] = p;
		d1[2 * x] = p; d1[2 * x + 1] = p;
	}
}

/* 640 wide: 320 doubled. WIDESCREEN (848): 424 doubled, the bottom bar
   with 104 more pixels of bar between the health and the ammo halves. */
static void
LargeHud(const SDL_Surface *src, SDL_Surface *dst)
{
	int w = src->w, half = w / 2, mid = half - 320;
	int y;

	if (hud_top)
	{
		for (y = 0; y < 16; y++)
		{
			const Uint8 *s = (const Uint8 *)src->pixels + (size_t)y * src->pitch;
			Uint32 *d0 = (Uint32 *)((Uint8 *)dst->pixels + (size_t)(2 * y) * dst->pitch);
			Uint32 *d1 = (Uint32 *)((Uint8 *)dst->pixels + (size_t)(2 * y + 1) * dst->pitch);

			Double8(s, d0, d1, half);
		}
	}

	if (hud_bottom)
	{
		for (y = 0; y < 16; y++)
		{
			const Uint8 *s = (const Uint8 *)src->pixels + (size_t)(464 + y) * src->pitch;
			Uint32 *d0 = (Uint32 *)((Uint8 *)dst->pixels + (size_t)(448 + 2 * y) * dst->pitch);
			Uint32 *d1 = (Uint32 *)((Uint8 *)dst->pixels + (size_t)(449 + 2 * y) * dst->pitch);
			Uint8 row[512];

			memcpy(row, s, 160);
			memcpy(row + 160, s + 160, mid);
			memcpy(row + 160 + mid, s + w - 160, 160);
			Double8(row, d0, d1, half);
		}
	}
}

int
SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst,
		SDL_Rect *dstrect)
{
	int sx = 0, sy = 0, dx = 0, dy = 0, w, h, y;

	if (!src || !dst || !src->pixels || !dst->pixels)
	{
		last_error = "blit: no pixels";
		return -1;
	}

	w = src->w;
	h = src->h;

	if (srcrect)
	{
		sx = srcrect->x;
		sy = srcrect->y;
		w = srcrect->w;
		h = srcrect->h;
	}

	if (dstrect)
	{
		dx = dstrect->x;
		dy = dstrect->y;
	}

	if (!ClipBlit(src, dst, &sx, &sy, &dx, &dy, &w, &h))
	{
		return 0;
	}

	if (src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 32)
	{
		UpdateLut(src->format->palette);

		for (y = 0; y < h; y++)
		{
			const Uint8 *s = (const Uint8 *)src->pixels + (size_t)(sy + y) * src->pitch + sx;
			Uint32 *d = (Uint32 *)((Uint8 *)dst->pixels + (size_t)(dy + y) * dst->pitch) + dx;
			int x = 0;

			/* 8 at a time: the destination is often RSX memory, where
			   long runs of stores are what the bus likes */
			for (; x + 8 <= w; x += 8)
			{
				Uint32 p0 = lut[s[x + 0]], p1 = lut[s[x + 1]];
				Uint32 p2 = lut[s[x + 2]], p3 = lut[s[x + 3]];
				Uint32 p4 = lut[s[x + 4]], p5 = lut[s[x + 5]];
				Uint32 p6 = lut[s[x + 6]], p7 = lut[s[x + 7]];

				d[x + 0] = p0; d[x + 1] = p1; d[x + 2] = p2; d[x + 3] = p3;
				d[x + 4] = p4; d[x + 5] = p5; d[x + 6] = p6; d[x + 7] = p7;
			}

			for (; x < w; x++)
			{
				d[x] = lut[s[x]];
			}
		}

		if ((w == 640 || w == 848) && h == 480 && sx == 0 && sy == 0 &&
			dx == 0 && dy == 0 && dst->w >= w && dst->h >= 480 &&
			(hud_top || hud_bottom))
		{
			LargeHud(src, dst);
		}

		return 0;
	}

	if (src->format->BitsPerPixel == dst->format->BitsPerPixel)
	{
		int bpp = src->format->BytesPerPixel;

		for (y = 0; y < h; y++)
		{
			memcpy((Uint8 *)dst->pixels + (size_t)(dy + y) * dst->pitch + dx * bpp,
					(const Uint8 *)src->pixels + (size_t)(sy + y) * src->pitch + sx * bpp,
					(size_t)w * bpp);
		}

		return 0;
	}

	PS3_Log("[sdl] SDL_BlitSurface: %d -> %d bpp not supported",
			src->format->BitsPerPixel, dst->format->BitsPerPixel);
	last_error = "unsupported blit";

	return -1;
}

/* Nearest neighbour stretch between surfaces of the same depth. */
int
SDL_SoftStretch(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst,
		const SDL_Rect *dstrect)
{
	SDL_Rect sr, dr;
	int bpp, x, y;
	unsigned stepx, stepy, fy;

	if (!src || !dst || !src->pixels || !dst->pixels ||
			src->format->BitsPerPixel != dst->format->BitsPerPixel)
	{
		last_error = "stretch: bad surfaces";
		return -1;
	}

	sr = srcrect ? *srcrect : (SDL_Rect){0, 0, src->w, src->h};
	dr = dstrect ? *dstrect : (SDL_Rect){0, 0, dst->w, dst->h};

	if (sr.w <= 0 || sr.h <= 0 || dr.w <= 0 || dr.h <= 0 ||
			sr.x < 0 || sr.y < 0 || sr.x + sr.w > src->w || sr.y + sr.h > src->h ||
			dr.x < 0 || dr.y < 0 || dr.x + dr.w > dst->w || dr.y + dr.h > dst->h)
	{
		last_error = "stretch: bad rectangles";
		return -1;
	}

	bpp = src->format->BytesPerPixel;
	stepx = ((unsigned)sr.w << 16) / (unsigned)dr.w;
	stepy = ((unsigned)sr.h << 16) / (unsigned)dr.h;
	fy = 0;

	for (y = 0; y < dr.h; y++, fy += stepy)
	{
		const Uint8 *srow = (const Uint8 *)src->pixels +
			(size_t)(sr.y + (fy >> 16)) * src->pitch + (size_t)sr.x * bpp;
		Uint8 *drow = (Uint8 *)dst->pixels + (size_t)(dr.y + y) * dst->pitch +
			(size_t)dr.x * bpp;
		unsigned fx = 0;

		if (bpp == 1)
		{
			for (x = 0; x < dr.w; x++, fx += stepx)
			{
				drow[x] = srow[fx >> 16];
			}
		}
		else
		{
			for (x = 0; x < dr.w; x++, fx += stepx)
			{
				((Uint32 *)drow)[x] = ((const Uint32 *)srow)[fx >> 16];
			}
		}
	}

	return 0;
}

int
SDL_BlitScaled(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst,
		SDL_Rect *dstrect)
{
	return SDL_SoftStretch(src, srcrect, dst, dstrect);
}

int
SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color)
{
	SDL_Rect r;
	int x, y;

	if (!dst || !dst->pixels)
	{
		return -1;
	}

	r = rect ? *rect : (SDL_Rect){0, 0, dst->w, dst->h};

	if (r.x < 0) { r.w += r.x; r.x = 0; }
	if (r.y < 0) { r.h += r.y; r.y = 0; }
	if (r.x + r.w > dst->w) r.w = dst->w - r.x;
	if (r.y + r.h > dst->h) r.h = dst->h - r.y;

	for (y = 0; y < r.h; y++)
	{
		Uint8 *row = (Uint8 *)dst->pixels + (size_t)(r.y + y) * dst->pitch;

		if (dst->format->BitsPerPixel == 8)
		{
			memset(row + r.x, (int)(color & 0xff), (size_t)(r.w > 0 ? r.w : 0));
		}
		else
		{
			for (x = 0; x < r.w; x++)
			{
				((Uint32 *)row)[r.x + x] = color;
			}
		}
	}

	return 0;
}

SDL_bool
SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp, Uint32 *Rmask,
		Uint32 *Gmask, Uint32 *Bmask, Uint32 *Amask)
{
	if (format != SDL_PIXELFORMAT_ARGB8888)
	{
		return SDL_FALSE;
	}

	*bpp = 32;
	*Rmask = 0x00ff0000u;
	*Gmask = 0x0000ff00u;
	*Bmask = 0x000000ffu;
	*Amask = 0xff000000u;

	return SDL_TRUE;
}

static void
PutLE16(FILE *f, unsigned v)
{
	fputc((int)(v & 0xff), f);
	fputc((int)((v >> 8) & 0xff), f);
}

static void
PutLE32(FILE *f, unsigned v)
{
	PutLE16(f, v & 0xffff);
	PutLE16(f, v >> 16);
}

/* 8 bit surfaces only (the game's screen): an 8 bit BMP with its palette.
   Screenshots end up next to the savegames. */
int
SDL_SaveBMP(SDL_Surface *s, const char *file)
{
	FILE *f;
	int y, i, rowbytes, pad;

	if (!s || !s->pixels || s->format->BitsPerPixel != 8)
	{
		last_error = "SDL_SaveBMP: 8 bit surfaces only";
		return -1;
	}

	f = fopen(file, "wb");

	if (!f)
	{
		last_error = "SDL_SaveBMP: can't create the file";
		return -1;
	}

	rowbytes = s->w;
	pad = (4 - (rowbytes & 3)) & 3;

	/* BITMAPFILEHEADER + BITMAPINFOHEADER + 256 color palette */
	fputc('B', f);
	fputc('M', f);
	PutLE32(f, 14 + 40 + 1024 + (unsigned)(rowbytes + pad) * s->h);
	PutLE32(f, 0);
	PutLE32(f, 14 + 40 + 1024);
	PutLE32(f, 40);
	PutLE32(f, (unsigned)s->w);
	PutLE32(f, (unsigned)s->h);
	PutLE16(f, 1);
	PutLE16(f, 8);
	PutLE32(f, 0);
	PutLE32(f, (unsigned)(rowbytes + pad) * s->h);
	PutLE32(f, 2835);
	PutLE32(f, 2835);
	PutLE32(f, 256);
	PutLE32(f, 0);

	for (i = 0; i < 256; i++)
	{
		const SDL_Color *c = &s->format->palette->colors[i];

		fputc(c->b, f);
		fputc(c->g, f);
		fputc(c->r, f);
		fputc(0, f);
	}

	/* bottom-up rows */
	for (y = s->h - 1; y >= 0; y--)
	{
		fwrite((const Uint8 *)s->pixels + (size_t)y * s->pitch, 1, (size_t)rowbytes, f);

		for (i = 0; i < pad; i++)
		{
			fputc(0, f);
		}
	}

	if (fclose(f) != 0)
	{
		last_error = "SDL_SaveBMP: write failed";
		return -1;
	}

	PS3_Log("[sdl] screenshot %s", file);

	return 0;
}

/* ================================================================ */
/* Window, renderer, textures                                         */
/* ================================================================ */

struct SDL_Window
{
	int w, h;
};

struct SDL_Renderer
{
	SDL_Window *window;
};

struct SDL_Texture
{
	int w, h;
	int locked;
};

static SDL_Texture *the_texture;   /* the last one locked: the screen */
static SDL_Window the_window;
static SDL_Renderer the_renderer;
static int window_open, renderer_open;

SDL_Window *
SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
	(void)title;
	(void)x;
	(void)y;
	(void)flags;

	if (PS3_Video_Init() != 0)
	{
		last_error = "the RSX didn't start (see the log)";
		return NULL;
	}

	the_window.w = w;
	the_window.h = h;
	window_open = 1;

	PS3_Log("[sdl] window %dx%d", w, h);

	return &the_window;
}

void
SDL_DestroyWindow(SDL_Window *window)
{
	(void)window;

	/* The RSX stays up: the video is shut down once, at exit
	   (PS3_Exit). Here only the game's image goes. */
	window_open = 0;
}

void
SDL_SetWindowTitle(SDL_Window *window, const char *title)
{
	(void)window;
	(void)title;
}

void
SDL_SetWindowMinimumSize(SDL_Window *window, int min_w, int min_h)
{
	(void)window;
	(void)min_w;
	(void)min_h;
}

int
SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags)
{
	(void)window;
	(void)flags;
	return 0;
}

Uint32
SDL_GetWindowPixelFormat(SDL_Window *window)
{
	(void)window;
	return SDL_PIXELFORMAT_ARGB8888;
}

const char *
SDL_GetCurrentVideoDriver(void)
{
	return "ps3";
}

SDL_Renderer *
SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags)
{
	(void)index;
	(void)flags;

	if (!window)
	{
		return NULL;
	}

	the_renderer.window = window;
	renderer_open = 1;

	return &the_renderer;
}

SDL_Renderer *
SDL_GetRenderer(SDL_Window *window)
{
	(void)window;
	return renderer_open ? &the_renderer : NULL;
}

void
SDL_DestroyRenderer(SDL_Renderer *renderer)
{
	(void)renderer;
	renderer_open = 0;
}

int
SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h)
{
	(void)renderer;
	(void)w;
	(void)h;
	return 0;
}

int
SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	(void)renderer;
	(void)r;
	(void)g;
	(void)b;
	(void)a;
	return 0;
}

int
SDL_RenderClear(SDL_Renderer *renderer)
{
	/* the borders are cleared by PS3_Video_Present; the image is always
	   written whole */
	(void)renderer;
	return 0;
}

SDL_Texture *
SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access, int w, int h)
{
	SDL_Texture *t;

	(void)renderer;
	(void)access;

	if (format != SDL_PIXELFORMAT_ARGB8888)
	{
		last_error = "unsupported texture format";
		return NULL;
	}

	t = calloc(1, sizeof(*t));

	if (!t)
	{
		return NULL;
	}

	t->w = w;
	t->h = h;

	return t;
}

void
SDL_DestroyTexture(SDL_Texture *texture)
{
	if (texture == the_texture)
	{
		the_texture = NULL;
	}

	free(texture);
}

static Uint64 lock_time;

int
SDL_LockTexture(SDL_Texture *texture, const SDL_Rect *rect, void **pixels,
		int *pitch)
{
	uint32_t *img;

	(void)rect;

	if (!texture || PS3_Video_SetSource(texture->w, texture->h) != 0)
	{
		last_error = "no source image";
		return -1;
	}

	/* 320x200, 640x480, 640x400 (the end screen): 4:3 on the TV.
	   848x480 (Video Settings > WIDESCREEN): 16:9. */
	if (texture->w >= 800)
		PS3_Video_SetDisplayAspect(16, 9);
	else
		PS3_Video_SetDisplayAspect(4, 3);

	img = PS3_Video_Source();   /* waits until the RSX is done with it */

	if (!img)
	{
		last_error = "no source image";
		return -1;
	}

	lock_time = NowMicro();
	texture->locked = 1;
	the_texture = texture;
	*pixels = img;
	*pitch = texture->w * 4;

	return 0;
}

void
SDL_UnlockTexture(SDL_Texture *texture)
{
	if (texture && texture->locked)
	{
		texture->locked = 0;
		PS3_Video_ProfileCopy((long long)(NowMicro() - lock_time));
	}
}

int
SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect,
		const void *pixels, int pitch)
{
	void *dst;
	int dpitch, y;

	(void)rect;

	if (SDL_LockTexture(texture, NULL, &dst, &dpitch) != 0)
	{
		return -1;
	}

	for (y = 0; y < texture->h; y++)
	{
		memcpy((Uint8 *)dst + (size_t)y * dpitch,
				(const Uint8 *)pixels + (size_t)y * pitch, (size_t)texture->w * 4);
	}

	SDL_UnlockTexture(texture);

	return 0;
}

int
SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture,
		const SDL_Rect *srcrect, const SDL_Rect *dstrect)
{
	/* the texture IS the source image: nothing to copy */
	(void)renderer;
	(void)texture;
	(void)srcrect;
	(void)dstrect;
	return 0;
}

/* ---- frame rate counter (Options > Video Settings) ---- */

static int show_fps;
static int fps_value;
static Uint64 fps_window;
static int fps_frames;

void
PS3_Video_SetShowFPS(int on)
{
	show_fps = on ? 1 : 0;
}

/* Into the image about to be shown (RSX memory: only written). */
static void
DrawFPS(void)
{
	uint32_t *img = PS3_Video_Source();
	char text[8];
	int w, x0, y0, i, n;

	if (!img || !the_texture)
	{
		return;
	}

	w = the_texture->w;
	n = snprintf(text, sizeof(text), "%d", fps_value);
	x0 = w - n * 8 - 4;
	y0 = 4;

	if (x0 < 0 || the_texture->h < 24)
	{
		return;
	}

	for (i = 0; i < n; i++)
	{
		unsigned ch = (unsigned char)text[i];
		int gx, gy;

		for (gy = -1; gy < 17; gy++)
		{
			uint32_t *row = img + (size_t)(y0 + gy) * w + x0 + i * 8;

			for (gx = -1; gx < 9; gx++)
			{
				int on = gy >= 0 && gy < 16 && gx >= 0 && gx < 8 &&
					(ps3_font8x16[ch - 32][gy] & (0x80u >> gx));

				row[gx] = on ? 0xffffff40u : 0xff000000u;
			}
		}
	}
}

/* DOS ROTT flashed the VGA overscan border red when you got hurt;
   taradino paints a frame inside the view instead. Here it goes where
   the overscan was: outside the picture. */
static int border_index;

void
PS3_SetBorderIndex(int idx)
{
	border_index = idx & 0xff;
}

void
SDL_RenderPresent(SDL_Renderer *renderer)
{
	Uint64 now = NowMicro();

	(void)renderer;

	fps_frames++;

	if (now - fps_window >= 1000000ull)
	{
		fps_value = fps_window ? fps_frames : 0;
		fps_window = now;
		fps_frames = 0;
	}

	if (show_fps)
	{
		DrawFPS();
	}

	/* the damage flash, on the TV's side bars (rt_vid.c SetBorderColor) */
	PS3_Video_SetBorderColor(border_index ? lut[border_index] : 0xff000000u);

	PS3_Video_Present();
	PS3_Pump();
}

/* ================================================================ */
/* Mouse and joystick: none                                           */
/* ================================================================ */

Uint32
SDL_GetMouseState(int *x, int *y)
{
	if (x) *x = 0;
	if (y) *y = 0;
	return 0;
}

Uint32
SDL_GetRelativeMouseState(int *x, int *y)
{
	if (x) *x = 0;
	if (y) *y = 0;
	return 0;
}

int
SDL_SetRelativeMouseMode(SDL_bool enabled)
{
	(void)enabled;
	return 0;
}

int
SDL_NumJoysticks(void)
{
	return 0;
}

SDL_Joystick *
SDL_JoystickOpen(int device_index)
{
	(void)device_index;
	return NULL;
}

void
SDL_JoystickClose(SDL_Joystick *joystick)
{
	(void)joystick;
}

int
SDL_JoystickEventState(int state)
{
	return state;
}

Sint16
SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis)
{
	(void)joystick;
	(void)axis;
	return 0;
}

/* ================================================================ */
/* Events                                                             */
/* ================================================================ */

#define EVENT_QUEUE 64

static SDL_Event queue[EVENT_QUEUE];
static int queue_head, queue_tail;
static int quit_sent;

int
SDL_PushEvent(SDL_Event *event)
{
	int next = (queue_tail + 1) % EVENT_QUEUE;

	if (next == queue_head)
	{
		return 0;   /* full: dropped */
	}

	queue[queue_tail] = *event;
	queue_tail = next;

	return 1;
}

int
SDL_PollEvent(SDL_Event *event)
{
	PS3_Pump();

	if (PS3_ExitRequested() && !quit_sent)
	{
		SDL_Event q;

		memset(&q, 0, sizeof(q));
		q.type = SDL_QUIT;
		quit_sent = 1;
		SDL_PushEvent(&q);
	}

	PS3_Input_Pump();

	if (queue_head == queue_tail)
	{
		return 0;
	}

	if (event)
	{
		*event = queue[queue_head];
		queue_head = (queue_head + 1) % EVENT_QUEUE;
	}

	return 1;
}

SDL_bool
SDL_QuitRequested(void)
{
	PS3_Pump();
	return PS3_ExitRequested() ? SDL_TRUE : SDL_FALSE;
}

/* ================================================================ */
/* Memory "files"                                                     */
/* ================================================================ */

SDL_RWops *
SDL_RWFromMem(void *mem, int size)
{
	return SDL_RWFromConstMem(mem, size);
}

SDL_RWops *
SDL_RWFromConstMem(const void *mem, int size)
{
	SDL_RWops *rw;

	if (!mem || size < 0)
	{
		return NULL;
	}

	rw = calloc(1, sizeof(*rw));

	if (rw)
	{
		rw->base = mem;
		rw->size = (size_t)size;
	}

	return rw;
}

void
SDL_RWclose(SDL_RWops *rw)
{
	free(rw);
}
