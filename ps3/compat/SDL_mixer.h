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
 * A tiny "SDL_mixer 2" for the PS3, implemented by
 * ps3/source/sdl_mixer_ps3.c: the sound effect channels, and MIDI music
 * played by the Apogee Sound System's own AdLib driver on an emulated
 * OPL3 (ps3/source/ps3_midi.c), which is what Rise of the Triad sounded
 * like on a Sound Blaster.
 *
 * =======================================================================
 */

#ifndef PS3_SDL_MIXER_SHIM_H
#define PS3_SDL_MIXER_SHIM_H

#include "SDL.h"

#define SDL_MIXER_MAJOR_VERSION 2
#define SDL_MIXER_MINOR_VERSION 8
#define SDL_MIXER_PATCHLEVEL    0
#define SDL_MIXER_VERSION_ATLEAST(X, Y, Z) \
	((SDL_MIXER_MAJOR_VERSION * 1000 + SDL_MIXER_MINOR_VERSION * 100 + SDL_MIXER_PATCHLEVEL) >= \
	 ((X) * 1000 + (Y) * 100 + (Z)))

#define MIX_MAX_VOLUME 128

typedef struct Mix_Chunk
{
	int allocated;
	Uint8 *abuf;       /* here: signed 16 bit mono samples */
	Uint32 alen;       /* bytes */
	Uint8 volume;
	int rate;          /* sample rate of abuf */
} Mix_Chunk;

typedef struct _Mix_Music Mix_Music;

typedef enum
{
	MIX_NO_FADING,
	MIX_FADING_OUT,
	MIX_FADING_IN
} Mix_Fading;

int Mix_OpenAudioDevice(int frequency, Uint16 format, int channels,
		int chunksize, const char *device, int allowed_changes);
int Mix_QuerySpec(int *frequency, Uint16 *format, int *channels);
void Mix_CloseAudio(void);
int Mix_AllocateChannels(int numchans);

Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc);
void Mix_FreeChunk(Mix_Chunk *chunk);

int Mix_PlayChannelTimed(int channel, Mix_Chunk *chunk, int loops, int ticks);
int Mix_HaltChannel(int channel);
int Mix_Playing(int channel);
int Mix_SetPanning(int channel, Uint8 left, Uint8 right);
int Mix_Volume(int channel, int volume);
int Mix_MasterVolume(int volume);

Mix_Music *Mix_LoadMUS_RW(SDL_RWops *src, int freesrc);
void Mix_FreeMusic(Mix_Music *music);
int Mix_PlayMusic(Mix_Music *music, int loops);
void Mix_HaltMusic(void);
void Mix_PauseMusic(void);
void Mix_ResumeMusic(void);
int Mix_PausedMusic(void);
int Mix_PlayingMusic(void);
int Mix_VolumeMusic(int volume);
int Mix_FadeOutMusic(int ms);
Mix_Fading Mix_FadingMusic(void);
void Mix_HookMusic(void (*mix_func)(void *udata, Uint8 *stream, int len),
		void *arg);

const char *Mix_GetSoundFonts(void);
int Mix_SetSoundFonts(const char *paths);

#endif
