/*
Copyright (C) 1994-1995 Apogee Software, Ltd.
Copyright (C) 2009 Jonathon Fowler <jf@jonof.id.au>
Copyright (C) 2010-2019 EDuke32 developers and contributors
Copyright (C) 2019 Nuke.YKT
Copyright (C) 2026 the ROTT-PS3 contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License version 2
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/

#ifndef PS3_MIDI_H
#define PS3_MIDI_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
	uint8_t SAVEK[2];
	uint8_t Level[2];
	uint8_t Env1[2];
	uint8_t Env2[2];
	uint8_t Wave[2];
	uint8_t Feedback;
	int8_t  Transpose;
	int8_t  Velocity;
} AdLibTimbre;

extern const AdLibTimbre ADLIB_TimbreBank[256];

/* All of these run under the mixer's lock (sdl_mixer_ps3.c). */

void PS3MIDI_Init(int rate);                       /* output sample rate */
int  PS3MIDI_Play(const uint8_t *song, size_t size, int loop);  /* 0 = ok */
void PS3MIDI_Stop(void);
void PS3MIDI_Pause(int paused);
int  PS3MIDI_Active(void);                         /* loaded and not over */
void PS3MIDI_SetVolume(int volume);                /* 0..255 */

/* A TinySoundFont synth (ps3_sf2.c) to play through, or NULL for the
   AdLib driver. The song playing starts over on the new one. */
void PS3MIDI_SetSoundFont(void *sf);

/* Adds 'frames' stereo frames of music (16 bit) into 'mix', scaled by
   gain (1.0 = as is). Does nothing when nothing plays. */
void PS3MIDI_Render(int32_t *mix, int frames, float gain);

#endif
