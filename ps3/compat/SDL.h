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
 * A tiny "SDL 2" for the PS3. It is NOT SDL: it declares only what
 * Taradino uses, and ps3/source/sdl_ps3.c implements it on top of the
 * RSX, ioPad and the audio port (ps3/source/sdl_mixer_ps3.c does the
 * SDL_mixer part). Types, constants and prototypes follow SDL 2.30, so
 * the engine builds with almost no changes.
 *
 * ps3/compat comes first in the include path (ps3/Makefile), so the
 * engine's #include "SDL.h" lands here.
 *
 * =======================================================================
 */

#ifndef PS3_SDL_SHIM_H
#define PS3_SDL_SHIM_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ---- basic types ---------------------------------------------------- */

typedef uint8_t Uint8;
typedef int8_t Sint8;
typedef uint16_t Uint16;
typedef int16_t Sint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef uint64_t Uint64;
typedef int64_t Sint64;

typedef enum
{
	SDL_FALSE = 0,
	SDL_TRUE = 1
} SDL_bool;

#define SDL_memset memset
#define SDL_memcpy memcpy
#define SDL_vsnprintf vsnprintf

void SDL_free(void *mem);

#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define SDL_BYTEORDER SDL_BIG_ENDIAN
#else
#define SDL_BYTEORDER SDL_LIL_ENDIAN
#endif

/* ---- init ----------------------------------------------------------- */

#define SDL_INIT_TIMER    0x00000001u
#define SDL_INIT_AUDIO    0x00000010u
#define SDL_INIT_VIDEO    0x00000020u
#define SDL_INIT_JOYSTICK 0x00000200u

int SDL_Init(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
Uint32 SDL_WasInit(Uint32 flags);
void SDL_QuitSubSystem(Uint32 flags);
void SDL_Quit(void);

const char *SDL_GetError(void);

#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"
SDL_bool SDL_SetHint(const char *name, const char *value);

Uint32 SDL_GetTicks(void);
Uint64 SDL_GetTicks64(void);
void SDL_Delay(Uint32 ms);

char *SDL_GetBasePath(void);
char *SDL_GetPrefPath(const char *org, const char *app);

#define SDL_MESSAGEBOX_ERROR 0x00000010u
typedef struct SDL_Window SDL_Window;
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title,
		const char *message, SDL_Window *window);

/* ---- surfaces and palettes ------------------------------------------ */

typedef struct SDL_Color
{
	Uint8 r;
	Uint8 g;
	Uint8 b;
	Uint8 a;
} SDL_Color;

typedef struct SDL_Palette
{
	int ncolors;
	SDL_Color *colors;
	Uint32 version;
	int refcount;
} SDL_Palette;

typedef struct SDL_PixelFormat
{
	Uint32 format;
	SDL_Palette *palette;
	Uint8 BitsPerPixel;
	Uint8 BytesPerPixel;
} SDL_PixelFormat;

typedef struct SDL_Rect
{
	int x, y;
	int w, h;
} SDL_Rect;

typedef struct SDL_Surface
{
	Uint32 flags;
	SDL_PixelFormat *format;
	int w, h;
	int pitch;
	void *pixels;
	void *userdata;
	int locked;
	void *list_blitmap;
	SDL_Rect clip_rect;
	void *map;
	int refcount;
} SDL_Surface;

#define SDL_PIXELFORMAT_INDEX8   0x13000801u
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height,
		int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(void *pixels, int width,
		int height, int depth, int pitch, Uint32 format);
void SDL_FreeSurface(SDL_Surface *surface);
int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors,
		int firstcolor, int ncolors);
int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color);
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect,
		SDL_Surface *dst, SDL_Rect *dstrect);
int SDL_BlitScaled(SDL_Surface *src, const SDL_Rect *srcrect,
		SDL_Surface *dst, SDL_Rect *dstrect);
int SDL_SoftStretch(SDL_Surface *src, const SDL_Rect *srcrect,
		SDL_Surface *dst, const SDL_Rect *dstrect);
SDL_bool SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp, Uint32 *Rmask,
		Uint32 *Gmask, Uint32 *Bmask, Uint32 *Amask);
int SDL_SaveBMP(SDL_Surface *surface, const char *file);

/* ---- windows, renderers, textures ---------------------------------- */

typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;

#define SDL_WINDOWPOS_CENTERED        0x2FFF0000u
#define SDL_WINDOW_FULLSCREEN         0x00000001u
#define SDL_WINDOW_RESIZABLE          0x00000020u
#define SDL_WINDOW_FULLSCREEN_DESKTOP (SDL_WINDOW_FULLSCREEN | 0x00001000u)
#define SDL_WINDOW_ALLOW_HIGHDPI      0x00002000u

#define SDL_RENDERER_SOFTWARE      0x00000001u
#define SDL_RENDERER_PRESENTVSYNC  0x00000004u
#define SDL_TEXTUREACCESS_STREAMING 1

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h,
		Uint32 flags);
void SDL_DestroyWindow(SDL_Window *window);
void SDL_SetWindowTitle(SDL_Window *window, const char *title);
void SDL_SetWindowMinimumSize(SDL_Window *window, int min_w, int min_h);
int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags);
Uint32 SDL_GetWindowPixelFormat(SDL_Window *window);
const char *SDL_GetCurrentVideoDriver(void);

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
SDL_Renderer *SDL_GetRenderer(SDL_Window *window);
void SDL_DestroyRenderer(SDL_Renderer *renderer);
int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h);
int SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b,
		Uint8 a);
int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture,
		const SDL_Rect *srcrect, const SDL_Rect *dstrect);
void SDL_RenderPresent(SDL_Renderer *renderer);

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format,
		int access, int w, int h);
void SDL_DestroyTexture(SDL_Texture *texture);
int SDL_LockTexture(SDL_Texture *texture, const SDL_Rect *rect,
		void **pixels, int *pitch);
void SDL_UnlockTexture(SDL_Texture *texture);
int SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect,
		const void *pixels, int pitch);

/* ---- keyboard ------------------------------------------------------- */

#include "ps3_sdl_keys.h"

/* ---- mouse ---------------------------------------------------------- */

#define SDL_BUTTON(X)     (1u << ((X) - 1))
#define SDL_BUTTON_LMASK  SDL_BUTTON(1)
#define SDL_BUTTON_MMASK  SDL_BUTTON(2)
#define SDL_BUTTON_RMASK  SDL_BUTTON(3)

Uint32 SDL_GetMouseState(int *x, int *y);
Uint32 SDL_GetRelativeMouseState(int *x, int *y);
int SDL_SetRelativeMouseMode(SDL_bool enabled);

/* ---- joystick (none: the DualShock 3 goes through ps3_input.c) ------- */

typedef struct _SDL_Joystick SDL_Joystick;

#define SDL_JOYSTICK_AXIS_MAX 32767
#define SDL_QUERY   -1
#define SDL_IGNORE   0
#define SDL_DISABLE  0
#define SDL_ENABLE   1

int SDL_NumJoysticks(void);
SDL_Joystick *SDL_JoystickOpen(int device_index);
void SDL_JoystickClose(SDL_Joystick *joystick);
int SDL_JoystickEventState(int state);
Sint16 SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis);

/* ---- events --------------------------------------------------------- */

#define SDL_RELEASED 0
#define SDL_PRESSED  1

typedef enum
{
	SDL_FIRSTEVENT = 0,
	SDL_QUIT = 0x100,
	SDL_KEYDOWN = 0x300,
	SDL_KEYUP,
	SDL_TEXTEDITING,
	SDL_TEXTINPUT,
	SDL_MOUSEMOTION = 0x400,
	SDL_MOUSEBUTTONDOWN,
	SDL_MOUSEBUTTONUP,
	SDL_MOUSEWHEEL,
	SDL_JOYAXISMOTION = 0x600,
	SDL_JOYBALLMOTION,
	SDL_JOYHATMOTION,
	SDL_JOYBUTTONDOWN,
	SDL_JOYBUTTONUP
} SDL_EventType;

typedef struct SDL_Keysym
{
	SDL_Scancode scancode;
	SDL_Keycode sym;
	Uint16 mod;
	Uint32 unused;
} SDL_Keysym;

typedef struct SDL_KeyboardEvent
{
	Uint32 type;
	Uint32 timestamp;
	Uint32 windowID;
	Uint8 state;
	Uint8 repeat;
	Uint8 padding2;
	Uint8 padding3;
	SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef struct SDL_MouseMotionEvent
{
	Uint32 type;
	Uint32 timestamp;
	Uint32 windowID;
	Uint32 which;
	Uint32 state;
	Sint32 x, y;
	Sint32 xrel, yrel;
} SDL_MouseMotionEvent;

typedef struct SDL_MouseButtonEvent
{
	Uint32 type;
	Uint32 timestamp;
	Uint32 windowID;
	Uint32 which;
	Uint8 button;
	Uint8 state;
	Uint8 clicks;
	Uint8 padding1;
	Sint32 x, y;
} SDL_MouseButtonEvent;

typedef struct SDL_MouseWheelEvent
{
	Uint32 type;
	Uint32 timestamp;
	Uint32 windowID;
	Uint32 which;
	Sint32 x, y;
	Uint32 direction;
} SDL_MouseWheelEvent;

typedef struct SDL_JoyBallEvent
{
	Uint32 type;
	Uint32 timestamp;
	Sint32 which;
	Uint8 ball;
	Uint8 padding1, padding2, padding3;
	Sint16 xrel, yrel;
} SDL_JoyBallEvent;

typedef struct SDL_JoyButtonEvent
{
	Uint32 type;
	Uint32 timestamp;
	Sint32 which;
	Uint8 button;
	Uint8 state;
	Uint8 padding1, padding2;
} SDL_JoyButtonEvent;

typedef union SDL_Event
{
	Uint32 type;
	SDL_KeyboardEvent key;
	SDL_MouseMotionEvent motion;
	SDL_MouseButtonEvent button;
	SDL_MouseWheelEvent wheel;
	SDL_JoyBallEvent jball;
	SDL_JoyButtonEvent jbutton;
	Uint8 padding[56];
} SDL_Event;

int SDL_PollEvent(SDL_Event *event);
int SDL_PushEvent(SDL_Event *event);
SDL_bool SDL_QuitRequested(void);

/* ---- memory "files" (SDL_mixer loads sounds and songs from these) --- */

typedef struct SDL_RWops
{
	const Uint8 *base;
	size_t size;
	size_t pos;
	int owned;
} SDL_RWops;

SDL_RWops *SDL_RWFromMem(void *mem, int size);
SDL_RWops *SDL_RWFromConstMem(const void *mem, int size);
void SDL_RWclose(SDL_RWops *rw);

/* ---- audio formats (only for printing what the mixer opened) -------- */

#define AUDIO_U8     0x0008
#define AUDIO_S16LSB 0x8010
#define AUDIO_S16MSB 0x9010
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
#define AUDIO_S16SYS AUDIO_S16LSB
#else
#define AUDIO_S16SYS AUDIO_S16MSB
#endif

#define SDL_AUDIO_MASK_BITSIZE   (0xFF)
#define SDL_AUDIO_MASK_DATATYPE  (1 << 8)
#define SDL_AUDIO_MASK_ENDIAN    (1 << 12)
#define SDL_AUDIO_MASK_SIGNED    (1 << 15)
#define SDL_AUDIO_BITSIZE(x)     ((x) & SDL_AUDIO_MASK_BITSIZE)
#define SDL_AUDIO_ISFLOAT(x)     ((x) & SDL_AUDIO_MASK_DATATYPE)
#define SDL_AUDIO_ISBIGENDIAN(x) ((x) & SDL_AUDIO_MASK_ENDIAN)
#define SDL_AUDIO_ISSIGNED(x)    ((x) & SDL_AUDIO_MASK_SIGNED)

#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x00000001

#endif
