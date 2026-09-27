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
 * Options > Music Synth > SOUNDFONT: ROTT's music in General MIDI, the
 * way a Sound Canvas or an AWE32 played it, through a SoundFont (.sf2)
 * the player copies to USRDIR. TinySoundFont (tsf.h, MIT, with a
 * big-endian fix) is the synth; the MIDI player stays the Apogee one
 * (ps3_midi.c), it just sends its events here instead of to the AdLib
 * driver.
 *
 * The first .sf2 in USRDIR (by name) is used. TinySoundFont keeps the
 * samples as floats: about twice the file's size in memory.
 *
 * =======================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "ps3_platform.h"
#include "ps3_midi.h"

#define TSF_IMPLEMENTATION
#include "tsf.h"

#define SF_RATE   48000    /* the audio port's (sdl_mixer_ps3.c) */
#define SF_VOICES 64

/* sdl_mixer_ps3.c: hands the synth to the audio thread under its lock */
void PS3_Mixer_SetSoundFont(void *sf);

static tsf *loaded;
static char loaded_name[128];

static int
IsSoundFont(const char *name)
{
	size_t n = strlen(name);

	return n > 4 && !strcasecmp(name + n - 4, ".sf2");
}

/* The first .sf2 in USRDIR, by name (readdir's order is the disk's). */
static int
FindSoundFont(char *path, size_t size, char *name, size_t namesize)
{
	DIR *d = opendir(PS3_USRDIR);
	struct dirent *e;
	int found = 0;

	if (!d)
	{
		return 0;
	}

	while ((e = readdir(d)) != NULL)
	{
		if (!IsSoundFont(e->d_name))
		{
			continue;
		}

		if (!found || strcasecmp(e->d_name, name) < 0)
		{
			snprintf(name, namesize, "%s", e->d_name);
			found = 1;
		}
	}

	closedir(d);

	if (found)
	{
		snprintf(path, size, "%s/%s", PS3_USRDIR, name);
	}

	return found;
}

static tsf *
LoadSoundFont(void)
{
	char path[512], name[128];
	struct stat st;
	unsigned long long t0;
	tsf *f;
	int ch;

	if (!FindSoundFont(path, sizeof(path), name, sizeof(name)))
	{
		PS3_Log("[music] no .sf2 in %s", PS3_USRDIR);
		return NULL;
	}

	if (stat(path, &st) == 0)
	{
		PS3_Log("[music] loading %s (%.1f MB)", name, st.st_size / (1024.0 * 1024.0));
	}

	t0 = PS3_Micros();
	f = tsf_load_filename(path);

	if (!f)
	{
		PS3_Log("[music] %s: not a SoundFont, or out of memory", name);
		return NULL;
	}

	tsf_set_output(f, TSF_STEREO_INTERLEAVED, SF_RATE, 0.0f);

	/* everything allocated here, nothing in the audio thread: the
	   voices up front (none are added later), and all 16 channels
	   (setting channel 15 first allocates 0..15 in one go) */
	if (!tsf_set_max_voices(f, SF_VOICES))
	{
		PS3_Log("[music] out of memory for the voices");
		tsf_close(f);
		return NULL;
	}

	for (ch = 15; ch >= 0; ch--)
	{
		tsf_channel_set_presetnumber(f, ch, 0, ch == 9);
	}

	PS3_Log("[music] %s: %d presets, loaded in %u ms", name,
			tsf_get_presetcount(f),
			(unsigned)((PS3_Micros() - t0) / 1000ull));

	snprintf(loaded_name, sizeof(loaded_name), "%s", name);

	return f;
}

/* on = 1: load the SoundFont (if it isn't) and play through it.
   Returns what is in use afterwards: 1 SoundFont, 0 AdLib. */
int
PS3_Music_UseSoundFont(int on)
{
	if (on)
	{
		if (!loaded)
		{
			loaded = LoadSoundFont();
		}

		if (!loaded)
		{
			PS3_Mixer_SetSoundFont(NULL);
			return 0;
		}

		PS3_Mixer_SetSoundFont(loaded);
		return 1;
	}

	PS3_Mixer_SetSoundFont(NULL);

	/* the memory back: a big SoundFont is a good part of the PS3's */
	if (loaded)
	{
		tsf_close(loaded);
		loaded = NULL;
		loaded_name[0] = 0;
		PS3_Log("[music] SoundFont unloaded, AdLib");
	}

	return 0;
}

const char *
PS3_Music_SoundFontName(void)
{
	return loaded ? loaded_name : NULL;
}
