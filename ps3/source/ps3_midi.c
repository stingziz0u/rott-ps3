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

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/**********************************************************************
   Based on MIDI.C and AL_MIDI.C of the Apogee Sound System

   author: James R. Dose
   date:   April 1 and May 25, 1994

   (c) Copyright 1994 James R. Dose.  All Rights Reserved.
**********************************************************************/

/*
 * ROTT's music the way a Sound Blaster played it: the Apogee Sound
 * System's MIDI player and its AdLib driver (the same code and General
 * MIDI instrument bank ROTT shipped with), on an emulated OPL3 (Nuked
 * OPL3, the build CrispyCell uses on the PS3).
 *
 * Ported to C from NBlood's source/audiolib (midi.cpp, driver_adlib.cpp),
 * trimmed to what one song at a time needs. Changes for the PS3:
 *
 *  - the song is read through unsigned bytes and with bounds checks (a
 *    broken song ends its track instead of reading past the buffer);
 *  - the original hibyte() macro read byte 1 of a 32 bit value through a
 *    char pointer: the wrong byte on a big-endian CPU. Replaced with a
 *    shift;
 *  - the voice lists are plain C, no offsetof() tricks;
 *  - timing: the music is rendered by the audio thread; one MIDI tick is
 *    processed every (rate / ticks-per-second) output samples, exactly
 *    as the original AdLibDrv_MIDI_Service did.
 */

#include <stdlib.h>
#include <string.h>

#include "ps3_midi.h"
#include "opl3.h"
#include "tsf.h"

/* ================================================================ */
/* OPL3 registers                                                     */
/* ================================================================ */

#define OPL3_ENABLE_WAVE_SELECT   0x20
#define OPL3_KBD_SPLIT_REGISTER   0x08
#define OPL3_PERCUSSION_REGISTER  0xbd
#define OPL3_MODE_REGISTER        0x05
#define OPL3_KSL_LEVEL            0x40
#define OPL3_KSL_MASK             0xc0
#define OPL3_TOTAL_LEVEL_MASK     0x3f
#define OPL3_ATTACK_DECAY         0x60
#define OPL3_SUSTAIN_RELEASE      0x80
#define OPL3_WAVE_SELECT          0xe0
#define OPL3_FNUM_LOW             0xa0
#define OPL3_KEYON_BLOCK          0xb0
#define OPL3_FEEDBACK_CONNECTION  0xc0
#define OPL3_FEEDBACK_MASK        0x0e
#define OPL3_CONNECTION_BIT       0x01
#define OPL3_STEREO_BITS          0x30

/* ================================================================ */
/* MIDI constants                                                     */
/* ================================================================ */

#define MIDI_MaxVolume            255
#define GENMIDI_DefaultVolume     90
#define NUM_MIDI_CHANNELS         16
#define TIME_PRECISION            16

#define MIDI_VOLUME                7
#define MIDI_PAN                   10
#define MIDI_DETUNE                94
#define MIDI_BANK_SELECT_MSB       0
#define MIDI_BANK_SELECT_LSB       32
#define MIDI_RPN_MSB               100
#define MIDI_RPN_LSB               101
#define MIDI_DATAENTRY_MSB         6
#define MIDI_DATAENTRY_LSB         38
#define MIDI_PITCHBEND_MSB         0
#define MIDI_PITCHBEND_LSB         0
#define MIDI_RUNNING_STATUS        0x80
#define MIDI_NOTE_OFF              0x8
#define MIDI_NOTE_ON               0x9
#define MIDI_POLY_AFTER_TCH        0xA
#define MIDI_CONTROL_CHANGE        0xB
#define MIDI_PROGRAM_CHANGE        0xC
#define MIDI_AFTER_TOUCH           0xD
#define MIDI_PITCH_BEND            0xE
#define MIDI_SPECIAL               0xF
#define MIDI_SYSEX                 0xF0
#define MIDI_SYSEX_CONTINUE        0xF7
#define MIDI_META_EVENT            0xFF
#define MIDI_END_OF_TRACK          0x2F
#define MIDI_HOLD1                 0x40
#define MIDI_SOSTENUTO             0x42
#define MIDI_TEMPO_CHANGE          0x51
#define MIDI_TIME_SIGNATURE        0x58
#define MIDI_REVERB                0x5b
#define MIDI_CHORUS                0x5d
#define MIDI_ALL_SOUNDS_OFF        0x78
#define MIDI_RESET_ALL_CONTROLLERS 0x79
#define MIDI_ALL_NOTES_OFF         0x7b
#define MIDI_MONO_MODE_ON          0x7E

#define EMIDI_INFINITE          -1
#define EMIDI_END_LOOP_VALUE    127
#define EMIDI_ALL_CARDS         127
#define EMIDI_INCLUDE_TRACK     110
#define EMIDI_EXCLUDE_TRACK     111
#define EMIDI_PROGRAM_CHANGE    112
#define EMIDI_VOLUME_CHANGE     113
#define EMIDI_CONTEXT_START     114
#define EMIDI_CONTEXT_END       115
#define EMIDI_LOOP_START        116
#define EMIDI_LOOP_END          117
#define EMIDI_SONG_LOOP_START   118
#define EMIDI_SONG_LOOP_END     119
#define EMIDI_GeneralMIDI       0
#define EMIDI_AdLib             7
#define EMIDI_AffectsCurrentCard(c, type) (((c) == EMIDI_ALL_CARDS) || ((c) == (type)))
#define EMIDI_NUM_CONTEXTS      7

#define GET_MIDI_CHANNEL(event) ((event) & 0xf)
#define GET_MIDI_COMMAND(event) ((event) >> 4)

static const int _MIDI_CommandLengths[16] = {0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 1, 1, 2, 0};

typedef struct
{
	const uint8_t *pos;
	const uint8_t *loopstart;
	int16_t loopcount;
	int16_t RunningStatus;
	unsigned time;
	int FPSecondsPerTick;
	int16_t tick;
	int16_t beat;
	int16_t measure;
	int16_t BeatsPerMeasure;
	int16_t TicksPerBeat;
	int16_t TimeBase;
	int delay;
	int16_t active;
} songcontext;

typedef struct
{
	const uint8_t *start;
	const uint8_t *end;
	const uint8_t *pos;

	int delay;
	int16_t active;
	int16_t RunningStatus;

	int16_t currentcontext;
	songcontext context[EMIDI_NUM_CONTEXTS];

	char EMIDI_IncludeTrack;
	char EMIDI_ProgramChange;
	char EMIDI_VolumeChange;
} track;

/* ================================================================ */
/* AdLib driver state (AL_MIDI.C)                                     */
/* ================================================================ */

#define AL_MaxVolume             127
#define AL_DefaultChannelVolume  90
#define AL_DefaultPitchBendRange 200
#define AL_VoiceNotFound         -1
#define ADLIB_PORT               0x388
#define AL_NumChipSlots          18
#define NUMADLIBVOICES           9
#define NUMADLIBCHANNELS         16
#define NOTE_ON                  0x2000
#define NOTE_OFF                 0x0000
#define MAX_VELOCITY             0x7f
#define MAX_OCTAVE               7
#define MAX_NOTE                 (MAX_OCTAVE * 12 + 11)
#define FINETUNE_MAX             31
#define FINETUNE_RANGE           (FINETUNE_MAX + 1)
#define PITCHBEND_CENTER         1638400

typedef struct AdLibVoice
{
	struct AdLibVoice *next;
	struct AdLibVoice *prev;
	uint32_t num;
	uint32_t key;
	uint32_t velocity;
	uint32_t channel;
	uint32_t pitchleft;
	int timbre;
	int port;
	uint32_t status;
} AdLibVoice;

typedef struct
{
	AdLibVoice *start;
	AdLibVoice *end;
} AdLibVoiceList;

typedef struct
{
	AdLibVoiceList Voices;
	int Timbre;
	int Pitchbend;
	int KeyOffset;
	uint32_t KeyDetune;
	uint32_t Volume;
	int Pan;
	int Detune;
	uint32_t RPN;
	int16_t PitchBendRange;
	int16_t PitchBendSemiTones;
	int16_t PitchBendHundreds;
} AdLibChannel;

static opl3_chip AL_Chip;
static int AL_Volume = MIDI_MaxVolume;
static const float AL_PostAmp = 3.0f;
static int AL_Stereo = 1;
static int AL_AdditiveMode = 0;

static const uint32_t OctavePitch[MAX_OCTAVE + 1] = {
	0x0000, 0x0400, 0x0800, 0x0C00, 0x1000, 0x1400, 0x1800, 0x1C00,
};

static uint32_t NoteMod12[MAX_NOTE + 1];
static uint32_t NoteDiv12[MAX_NOTE + 1];

static const uint32_t NotePitch[FINETUNE_MAX + 1][12] = {
	{ 0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca, 0x1e5, 0x202, 0x220, 0x241, 0x263, 0x287 },
	{ 0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca, 0x1e5, 0x202, 0x220, 0x242, 0x264, 0x288 },
	{ 0x158, 0x16c, 0x182, 0x199, 0x1b1, 0x1cb, 0x1e6, 0x203, 0x221, 0x243, 0x265, 0x289 },
	{ 0x158, 0x16c, 0x183, 0x19a, 0x1b2, 0x1cc, 0x1e7, 0x204, 0x222, 0x244, 0x266, 0x28a },
	{ 0x159, 0x16d, 0x183, 0x19a, 0x1b3, 0x1cd, 0x1e8, 0x205, 0x223, 0x245, 0x267, 0x28b },
	{ 0x15a, 0x16e, 0x184, 0x19b, 0x1b3, 0x1ce, 0x1e9, 0x206, 0x224, 0x246, 0x268, 0x28c },
	{ 0x15a, 0x16e, 0x185, 0x19c, 0x1b4, 0x1ce, 0x1ea, 0x207, 0x225, 0x247, 0x269, 0x28e },
	{ 0x15b, 0x16f, 0x185, 0x19d, 0x1b5, 0x1cf, 0x1eb, 0x208, 0x226, 0x248, 0x26a, 0x28f },
	{ 0x15b, 0x170, 0x186, 0x19d, 0x1b6, 0x1d0, 0x1ec, 0x209, 0x227, 0x249, 0x26b, 0x290 },
	{ 0x15c, 0x170, 0x187, 0x19e, 0x1b7, 0x1d1, 0x1ec, 0x20a, 0x228, 0x24a, 0x26d, 0x291 },
	{ 0x15d, 0x171, 0x188, 0x19f, 0x1b7, 0x1d2, 0x1ed, 0x20b, 0x229, 0x24b, 0x26e, 0x292 },
	{ 0x15d, 0x172, 0x188, 0x1a0, 0x1b8, 0x1d3, 0x1ee, 0x20c, 0x22a, 0x24c, 0x26f, 0x293 },
	{ 0x15e, 0x172, 0x189, 0x1a0, 0x1b9, 0x1d4, 0x1ef, 0x20d, 0x22b, 0x24d, 0x270, 0x295 },
	{ 0x15f, 0x173, 0x18a, 0x1a1, 0x1ba, 0x1d4, 0x1f0, 0x20e, 0x22c, 0x24e, 0x271, 0x296 },
	{ 0x15f, 0x174, 0x18a, 0x1a2, 0x1bb, 0x1d5, 0x1f1, 0x20f, 0x22d, 0x24f, 0x272, 0x297 },
	{ 0x160, 0x174, 0x18b, 0x1a3, 0x1bb, 0x1d6, 0x1f2, 0x210, 0x22e, 0x250, 0x273, 0x298 },
	{ 0x161, 0x175, 0x18c, 0x1a3, 0x1bc, 0x1d7, 0x1f3, 0x211, 0x22f, 0x251, 0x274, 0x299 },
	{ 0x161, 0x176, 0x18c, 0x1a4, 0x1bd, 0x1d8, 0x1f4, 0x212, 0x230, 0x252, 0x276, 0x29b },
	{ 0x162, 0x176, 0x18d, 0x1a5, 0x1be, 0x1d9, 0x1f5, 0x212, 0x231, 0x254, 0x277, 0x29c },
	{ 0x162, 0x177, 0x18e, 0x1a6, 0x1bf, 0x1d9, 0x1f5, 0x213, 0x232, 0x255, 0x278, 0x29d },
	{ 0x163, 0x178, 0x18f, 0x1a6, 0x1bf, 0x1da, 0x1f6, 0x214, 0x233, 0x256, 0x279, 0x29e },
	{ 0x164, 0x179, 0x18f, 0x1a7, 0x1c0, 0x1db, 0x1f7, 0x215, 0x235, 0x257, 0x27a, 0x29f },
	{ 0x164, 0x179, 0x190, 0x1a8, 0x1c1, 0x1dc, 0x1f8, 0x216, 0x236, 0x258, 0x27b, 0x2a1 },
	{ 0x165, 0x17a, 0x191, 0x1a9, 0x1c2, 0x1dd, 0x1f9, 0x217, 0x237, 0x259, 0x27c, 0x2a2 },
	{ 0x166, 0x17b, 0x192, 0x1aa, 0x1c3, 0x1de, 0x1fa, 0x218, 0x238, 0x25a, 0x27e, 0x2a3 },
	{ 0x166, 0x17b, 0x192, 0x1aa, 0x1c3, 0x1df, 0x1fb, 0x219, 0x239, 0x25b, 0x27f, 0x2a4 },
	{ 0x167, 0x17c, 0x193, 0x1ab, 0x1c4, 0x1e0, 0x1fc, 0x21a, 0x23a, 0x25c, 0x280, 0x2a6 },
	{ 0x168, 0x17d, 0x194, 0x1ac, 0x1c5, 0x1e0, 0x1fd, 0x21b, 0x23b, 0x25d, 0x281, 0x2a7 },
	{ 0x168, 0x17d, 0x194, 0x1ad, 0x1c6, 0x1e1, 0x1fe, 0x21c, 0x23c, 0x25e, 0x282, 0x2a8 },
	{ 0x169, 0x17e, 0x195, 0x1ad, 0x1c7, 0x1e2, 0x1ff, 0x21d, 0x23d, 0x260, 0x283, 0x2a9 },
	{ 0x16a, 0x17f, 0x196, 0x1ae, 0x1c8, 0x1e3, 0x1ff, 0x21e, 0x23e, 0x261, 0x284, 0x2ab },
	{ 0x16a, 0x17f, 0x197, 0x1af, 0x1c8, 0x1e4, 0x200, 0x21f, 0x23f, 0x262, 0x286, 0x2ac }
};

/* Slot numbers as a function of the voice and the operator (melodic). */
static const int slotVoice[NUMADLIBVOICES][2] = {
	{0, 3}, {1, 4}, {2, 5}, {6, 9}, {7, 10}, {8, 11}, {12, 15}, {13, 16}, {14, 17},
};

static int VoiceLevel[AL_NumChipSlots][2];
static int VoiceKsl[AL_NumChipSlots][2];

/* The offset of each slot within the chip. */
static const int8_t offsetSlot[AL_NumChipSlots] = {
	0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21
};

static AdLibVoice Voice[NUMADLIBVOICES * 2];
static AdLibVoiceList Voice_Pool;
static AdLibChannel Channel[NUMADLIBCHANNELS];

#define AL_LeftPort  ADLIB_PORT
#define AL_RightPort (ADLIB_PORT + 2)

static int
Clamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void
List_Remove(AdLibVoiceList *list, AdLibVoice *item)
{
	if (item->prev == NULL)
		list->start = item->next;
	else
		item->prev->next = item->next;

	if (item->next == NULL)
		list->end = item->prev;
	else
		item->next->prev = item->prev;

	item->next = NULL;
	item->prev = NULL;
}

/* LL_AddToTail in the original, which (despite its name) adds at the
   head: kept, voice allocation order depends on it. */
static void
List_Add(AdLibVoiceList *list, AdLibVoice *item)
{
	item->prev = NULL;
	item->next = list->start;

	if (list->start)
		list->start->prev = item;
	else
		list->end = item;

	list->start = item;
}

static void
AL_SendOutputToPort(int port, int reg, int data)
{
	OPL3_WriteRegBuffered(&AL_Chip, (uint16_t)(reg + ((port & 2) << 7)), (uint8_t)data);
}

static void
AL_SendOutput(int voice, int reg, int data)
{
	AL_SendOutputToPort(voice ? AL_LeftPort : AL_RightPort, reg, data);
}

static void
AL_SetVoiceTimbre(int voice)
{
	int channel = (int)Voice[voice].channel;
	int patch = (channel == 9) ? (int)Voice[voice].key + 128 : Channel[channel].Timbre;
	const AdLibTimbre *timbre;
	int port, voc, slot, off;

	if (Voice[voice].timbre == patch)
		return;

	Voice[voice].timbre = patch;
	timbre = &ADLIB_TimbreBank[patch & 255];

	port = Voice[voice].port;
	voc = (voice >= NUMADLIBVOICES) ? voice - NUMADLIBVOICES : voice;
	slot = slotVoice[voc][0];
	off = offsetSlot[slot];

	VoiceLevel[slot][port] = OPL3_TOTAL_LEVEL_MASK - (timbre->Level[0] & OPL3_TOTAL_LEVEL_MASK);
	VoiceKsl[slot][port] = timbre->Level[0] & OPL3_KSL_MASK;

	AL_SendOutput(port, OPL3_FNUM_LOW + voc, 0);
	AL_SendOutput(port, OPL3_KEYON_BLOCK + voc, 0);

	/* Let voice clear the release */
	AL_SendOutput(port, OPL3_SUSTAIN_RELEASE + off, 0xff);

	AL_SendOutput(port, OPL3_ATTACK_DECAY + off, timbre->Env1[0]);
	AL_SendOutput(port, OPL3_SUSTAIN_RELEASE + off, timbre->Env2[0]);
	AL_SendOutput(port, OPL3_ENABLE_WAVE_SELECT + off, timbre->SAVEK[0]);
	AL_SendOutput(port, OPL3_WAVE_SELECT + off, timbre->Wave[0]);

	AL_SendOutput(port, OPL3_KSL_LEVEL + off, timbre->Level[0]);
	slot = slotVoice[voc][1];

	AL_SendOutput(port, OPL3_FEEDBACK_CONNECTION + voc,
			(timbre->Feedback & (OPL3_FEEDBACK_MASK | OPL3_CONNECTION_BIT)) | OPL3_STEREO_BITS);

	off = offsetSlot[slot];

	VoiceLevel[slot][port] = OPL3_TOTAL_LEVEL_MASK - (timbre->Level[1] & OPL3_TOTAL_LEVEL_MASK);
	VoiceKsl[slot][port] = timbre->Level[1] & OPL3_KSL_MASK;

	AL_SendOutput(port, OPL3_KSL_LEVEL + off, OPL3_TOTAL_LEVEL_MASK);

	/* Let voice clear the release */
	AL_SendOutput(port, OPL3_SUSTAIN_RELEASE + off, 0xff);

	AL_SendOutput(port, OPL3_ATTACK_DECAY + off, timbre->Env1[1]);
	AL_SendOutput(port, OPL3_SUSTAIN_RELEASE + off, timbre->Env2[1]);
	AL_SendOutput(port, OPL3_ENABLE_WAVE_SELECT + off, timbre->SAVEK[1]);
	AL_SendOutput(port, OPL3_WAVE_SELECT + off, timbre->Wave[1]);
}

static void
AL_SetVoiceVolume(int voice)
{
	int channel = (int)Voice[voice].channel;
	const AdLibTimbre *timbre = &ADLIB_TimbreBank[Voice[voice].timbre & 255];
	int velocity = (int)Voice[voice].velocity + timbre->Velocity;
	int voc = (voice >= NUMADLIBVOICES) ? voice - NUMADLIBVOICES : voice;
	int slot = slotVoice[voc][1];
	int port = Voice[voice].port;
	uint32_t t1, volume;

	if (velocity > MAX_VELOCITY)
		velocity = MAX_VELOCITY;

	/* amplitude */
	t1 = (uint32_t)VoiceLevel[slot][port] * (uint32_t)(velocity + 0x80);
	t1 = (Channel[channel].Volume * t1) >> 15;

	volume = t1 ^ OPL3_TOTAL_LEVEL_MASK;
	volume |= (uint32_t)VoiceKsl[slot][port];

	AL_SendOutput(port, OPL3_KSL_LEVEL + offsetSlot[slot], (int)volume);

	/* Additive timbre: the modulator is heard too */
	if (timbre->Feedback & 0x01)
	{
		int slot0 = slotVoice[voc][0];
		uint32_t t2;

		if (AL_AdditiveMode)
			t1 = (uint32_t)VoiceLevel[slot0][port] * (uint32_t)(velocity + 0x80);

		t2 = (Channel[channel].Volume * t1) >> 15;

		volume = t2 ^ OPL3_TOTAL_LEVEL_MASK;
		volume |= (uint32_t)VoiceKsl[slot0][port];

		AL_SendOutput(port, OPL3_KSL_LEVEL + offsetSlot[slot0], (int)volume);
	}
}

static int
AL_AllocVoice(void)
{
	int voice;

	if (!Voice_Pool.start)
		return AL_VoiceNotFound;

	voice = (int)Voice_Pool.start->num;
	List_Remove(&Voice_Pool, &Voice[voice]);

	return voice;
}

static int
AL_GetVoice(int channel, int key)
{
	const AdLibVoice *voice = Channel[channel].Voices.start;

	while (voice != NULL)
	{
		if (voice->key == (uint32_t)key)
			return (int)voice->num;

		voice = voice->next;
	}

	return AL_VoiceNotFound;
}

static void
AL_SetVoicePitch(int voice)
{
	int port = Voice[voice].port;
	int voc = (voice >= NUMADLIBVOICES) ? voice - NUMADLIBVOICES : voice;
	int channel = (int)Voice[voice].channel;
	int patch, note, detune, ScaleNote, Octave, pitch;

	if (channel == 9)
	{
		patch = (int)Voice[voice].key + 128;
		note = ADLIB_TimbreBank[patch & 255].Transpose;
	}
	else
	{
		patch = Channel[channel].Timbre;
		note = (int)Voice[voice].key + ADLIB_TimbreBank[patch & 255].Transpose;
	}

	note += Channel[channel].KeyOffset - 12;
	note = Clamp(note, 0, MAX_NOTE);

	detune = (int)Channel[channel].KeyDetune;

	ScaleNote = (int)NoteMod12[note];
	Octave = (int)NoteDiv12[note];

	pitch = (int)(OctavePitch[Octave] | NotePitch[detune][ScaleNote]);

	Voice[voice].pitchleft = (uint32_t)pitch;

	pitch |= (int)Voice[voice].status;

	AL_SendOutput(port, OPL3_FNUM_LOW + voc, pitch & 0xff);
	AL_SendOutput(port, OPL3_KEYON_BLOCK + voc, (pitch >> 8) & 0xff);
}

static void
AL_SetVoicePan(int voice)
{
	int port = Voice[voice].port;
	int voc = (voice >= NUMADLIBVOICES) ? voice - NUMADLIBVOICES : voice;
	int channel = (int)Voice[voice].channel;

	/* (only heard with Nuked OPL3's stereo extension, which is off) */
	if (AL_Stereo)
		AL_SendOutput(port, 0xD0 + voc, Channel[channel].Pan << 1);
}

static void
AL_SetChannelVolume(int channel, int volume)
{
	AdLibVoice *voice;

	Channel[channel].Volume = (uint32_t)Clamp(volume, 0, AL_MaxVolume);

	for (voice = Channel[channel].Voices.start; voice != NULL; voice = voice->next)
		AL_SetVoiceVolume((int)voice->num);
}

static void
AL_SetChannelPan(int channel, int pan)
{
	AdLibVoice *voice;

	/* Don't pan drum sounds */
	if (channel != 9)
		Channel[channel].Pan = pan;

	for (voice = Channel[channel].Voices.start; voice != NULL; voice = voice->next)
		AL_SetVoicePan((int)voice->num);
}

static void
AL_ResetVoices(void)
{
	int index;

	Voice_Pool.start = NULL;
	Voice_Pool.end = NULL;

	for (index = 0; index < NUMADLIBVOICES * 2; index++)
	{
		Voice[index].num = (uint32_t)index;
		Voice[index].key = 0;
		Voice[index].velocity = 0;
		Voice[index].channel = (uint32_t)-1;
		Voice[index].timbre = -1;
		Voice[index].port = (index < NUMADLIBVOICES) ? 0 : 1;
		Voice[index].status = NOTE_OFF;
		List_Add(&Voice_Pool, &Voice[index]);
	}

	for (index = 0; index < NUMADLIBCHANNELS; index++)
	{
		memset(&Channel[index], 0, sizeof(Channel[index]));
		Channel[index].Volume = AL_DefaultChannelVolume;
		Channel[index].Pan = 64;
		Channel[index].PitchBendRange = AL_DefaultPitchBendRange;
		Channel[index].PitchBendSemiTones = AL_DefaultPitchBendRange / 100;
		Channel[index].PitchBendHundreds = AL_DefaultPitchBendRange % 100;
	}
}

static void
AL_CalcPitchInfo(void)
{
	int note;

	for (note = 0; note <= MAX_NOTE; note++)
	{
		NoteMod12[note] = (uint32_t)(note % 12);
		NoteDiv12[note] = (uint32_t)(note / 12);
	}
}

static void
AL_FlushCard(int port)
{
	int i;

	for (i = 0; i < NUMADLIBVOICES; i++)
	{
		int slot1 = offsetSlot[slotVoice[i][0]];
		int slot2 = offsetSlot[slotVoice[i][1]];

		AL_SendOutputToPort(port, OPL3_FNUM_LOW + i, 0);
		AL_SendOutputToPort(port, OPL3_KEYON_BLOCK + i, 0);

		AL_SendOutputToPort(port, OPL3_WAVE_SELECT + slot1, 0);
		AL_SendOutputToPort(port, OPL3_WAVE_SELECT + slot2, 0);

		/* Set the envelope to be fast and quiet */
		AL_SendOutputToPort(port, OPL3_ATTACK_DECAY + slot1, 0xff);
		AL_SendOutputToPort(port, OPL3_ATTACK_DECAY + slot2, 0xff);
		AL_SendOutputToPort(port, OPL3_SUSTAIN_RELEASE + slot1, 0xff);
		AL_SendOutputToPort(port, OPL3_SUSTAIN_RELEASE + slot2, 0xff);

		/* Maximum attenuation */
		AL_SendOutputToPort(port, OPL3_KSL_LEVEL + slot1, 0xff);
		AL_SendOutputToPort(port, OPL3_KSL_LEVEL + slot2, 0xff);
	}
}

static void
AL_SetStereo(int stereo)
{
	AL_SendOutputToPort(AL_RightPort, OPL3_MODE_REGISTER, (stereo << 1) + 1);
}

static void
AL_Reset(void)
{
	AL_SendOutputToPort(ADLIB_PORT, 1, OPL3_ENABLE_WAVE_SELECT);
	AL_SendOutputToPort(ADLIB_PORT, OPL3_KBD_SPLIT_REGISTER, 0);

	/* Set the values: AM Depth, VIB depth & Rhythm */
	AL_SendOutputToPort(ADLIB_PORT, OPL3_PERCUSSION_REGISTER, 0);

	AL_SetStereo(AL_Stereo);

	AL_FlushCard(AL_LeftPort);
	AL_FlushCard(AL_RightPort);
}

static void
AL_NoteOff(int channel, int key, int velocity)
{
	int voice, port, voc;

	(void)velocity;

	if (channel >= NUMADLIBCHANNELS)
		return;

	voice = AL_GetVoice(channel, key);

	if (voice == AL_VoiceNotFound)
		return;

	Voice[voice].status = NOTE_OFF;

	port = Voice[voice].port;
	voc = (voice >= NUMADLIBVOICES) ? voice - NUMADLIBVOICES : voice;

	/* hibyte() of the pitch, without the key-on bit (see the header) */
	AL_SendOutput(port, OPL3_KEYON_BLOCK + voc, (int)((Voice[voice].pitchleft >> 8) & 0xff));

	List_Remove(&Channel[channel].Voices, &Voice[voice]);
	List_Add(&Voice_Pool, &Voice[voice]);
}

static void
AL_NoteOn(int channel, int key, int velocity)
{
	int voice;

	if (channel >= NUMADLIBCHANNELS)
		return;

	if (velocity == 0)
	{
		AL_NoteOff(channel, key, velocity);
		return;
	}

	voice = AL_AllocVoice();

	if (voice == AL_VoiceNotFound)
	{
		if (Channel[9].Voices.start)
		{
			AL_NoteOff(9, (int)Channel[9].Voices.start->key, 0);
			voice = AL_AllocVoice();
		}

		if (voice == AL_VoiceNotFound)
			return;
	}

	Voice[voice].key = (uint32_t)key;
	Voice[voice].channel = (uint32_t)channel;
	Voice[voice].velocity = (uint32_t)velocity;
	Voice[voice].status = NOTE_ON;

	List_Add(&Channel[channel].Voices, &Voice[voice]);

	AL_SetVoiceTimbre(voice);
	AL_SetVoiceVolume(voice);
	AL_SetVoicePitch(voice);
	AL_SetVoicePan(voice);
}

static void
AL_AllNotesOff(int channel)
{
	while (Channel[channel].Voices.start != NULL)
		AL_NoteOff(channel, (int)Channel[channel].Voices.start->key, 0);
}

static void
AL_ControlChange(int channel, int type, int data)
{
	if (channel >= NUMADLIBCHANNELS)
		return;

	switch (type)
	{
		case MIDI_VOLUME:
			AL_SetChannelVolume(channel, data);
			break;

		case MIDI_PAN:
			AL_SetChannelPan(channel, data);
			break;

		case MIDI_DETUNE:
			Channel[channel].Detune = data;
			break;

		case MIDI_ALL_NOTES_OFF:
			AL_AllNotesOff(channel);
			break;

		case MIDI_RESET_ALL_CONTROLLERS:
			AL_ResetVoices();
			AL_SetChannelVolume(channel, AL_DefaultChannelVolume);
			AL_SetChannelPan(channel, 64);
			Channel[channel].Detune = 0;
			break;

		case MIDI_RPN_MSB:
			Channel[channel].RPN &= 0x00FF;
			Channel[channel].RPN |= (uint32_t)(data & 0xFF) << 8;
			break;

		case MIDI_RPN_LSB:
			Channel[channel].RPN &= 0xFF00;
			Channel[channel].RPN |= (uint32_t)(data & 0xFF);
			break;

		case MIDI_DATAENTRY_MSB:
			if (Channel[channel].RPN == MIDI_PITCHBEND_MSB)
			{
				Channel[channel].PitchBendSemiTones = (int16_t)data;
				Channel[channel].PitchBendRange = (int16_t)(Channel[channel].PitchBendSemiTones * 100 +
						Channel[channel].PitchBendHundreds);
			}
			break;

		case MIDI_DATAENTRY_LSB:
			if (Channel[channel].RPN == MIDI_PITCHBEND_LSB)
			{
				Channel[channel].PitchBendHundreds = (int16_t)data;
				Channel[channel].PitchBendRange = (int16_t)(Channel[channel].PitchBendSemiTones * 100 +
						Channel[channel].PitchBendHundreds);
			}
			break;
	}
}

static void
AL_ProgramChange(int channel, int patch)
{
	if (channel >= NUMADLIBCHANNELS)
		return;

	Channel[channel].Timbre = patch;
}

static void
AL_SetPitchBend(int channel, int lsb, int msb)
{
	int pitchbend, TotalBend;
	AdLibVoice *voice;

	if (channel >= NUMADLIBCHANNELS)
		return;

	pitchbend = lsb + (msb << 8);
	TotalBend = pitchbend * Channel[channel].PitchBendRange / (PITCHBEND_CENTER / FINETUNE_RANGE);

	Channel[channel].Pitchbend = pitchbend;
	Channel[channel].KeyOffset = TotalBend / FINETUNE_RANGE;
	Channel[channel].KeyOffset -= Channel[channel].PitchBendSemiTones;
	Channel[channel].KeyDetune = (uint32_t)(TotalBend % FINETUNE_RANGE);

	for (voice = Channel[channel].Voices.start; voice != NULL; voice = voice->next)
		AL_SetVoicePitch((int)voice->num);
}

/* ================================================================ */
/* MIDI song player (MIDI.C)                                          */
/* ================================================================ */

static int out_rate = 48000;

static track *_MIDI_TrackPtr;
static int _MIDI_NumTracks;

static int _MIDI_SongActive;
static int _MIDI_SongLoaded;
static int _MIDI_Paused;
static int _MIDI_Loop;

static int _MIDI_Division;
static int _MIDI_Tick;
static int _MIDI_Beat = 1;
static int _MIDI_Measure = 1;
static uint32_t _MIDI_Time;
static int _MIDI_BeatsPerMeasure;
static int _MIDI_TicksPerBeat;
static int _MIDI_TimeBase;
static int _MIDI_FPSecondsPerTick;
static uint32_t _MIDI_PositionInTicks;
static int _MIDI_Context;
static int _MIDI_ActiveTracks;
static int _MIDI_ChannelVolume[NUM_MIDI_CHANNELS];

/* ticks per second * ... (the original's MV_MIDIRenderTempo/Timer) */
/* ================================================================ */
/* Output: the AdLib driver, or a SoundFont (Options > Music Synth)   */
/* ================================================================ */

/* With a SoundFont loaded (ps3_sf2.c) the same player sends its events
   to TinySoundFont instead of the AdLib driver: General MIDI, the way a
   Sound Canvas or an AWE32 played ROTT. */
static tsf *SF_Synth;
static int EMIDI_Card = EMIDI_AdLib;

/* Everything off, the channels back to defaults. Not tsf_reset(): that
   frees the channels, and the audio thread would allocate them again. */
static void
SF_Silence(void)
{
	int channel;

	if (!SF_Synth)
		return;

	for (channel = 0; channel < 16; channel++)
	{
		tsf_channel_sounds_off_all(SF_Synth, channel);
		tsf_channel_midi_control(SF_Synth, channel, 121, 0);
		tsf_channel_set_pitchwheel(SF_Synth, channel, 8192);
		tsf_channel_set_presetnumber(SF_Synth, channel, 0, channel == 9);
	}
}

static void
OUT_NoteOn(int channel, int key, int velocity)
{
	if (SF_Synth)
		tsf_channel_note_on(SF_Synth, channel, key, (float)velocity * (1.0f / 127.0f));
	else
		AL_NoteOn(channel, key, velocity);
}

static void
OUT_NoteOff(int channel, int key, int velocity)
{
	if (SF_Synth)
		tsf_channel_note_off(SF_Synth, channel, key);
	else
		AL_NoteOff(channel, key, velocity);
}

static void
OUT_ControlChange(int channel, int type, int data)
{
	if (SF_Synth)
		tsf_channel_midi_control(SF_Synth, channel, type, data);
	else
		AL_ControlChange(channel, type, data);
}

static void
OUT_ProgramChange(int channel, int patch)
{
	if (SF_Synth)
		tsf_channel_set_presetnumber(SF_Synth, channel, patch, channel == 9);
	else
		AL_ProgramChange(channel, patch);
}

static void
OUT_SetPitchBend(int channel, int lsb, int msb)
{
	if (SF_Synth)
		tsf_channel_set_pitchwheel(SF_Synth, channel, (msb << 7) | lsb);
	else
		AL_SetPitchBend(channel, lsb, msb);
}

static int render_tempo = -1;
static int render_timer;

static void MIDI_SetTempo(int tempo);

static int
NextByte(track *t)
{
	if (t->pos >= t->end)
	{
		/* ran off the end of the track: stop it there */
		if (t->active)
		{
			t->active = 0;
			_MIDI_ActiveTracks--;
		}

		return 0;
	}

	return *t->pos++;
}

static int
_MIDI_ReadNumber(const uint8_t *from, size_t size)
{
	int value = 0;

	if (size > 4)
		size = 4;

	while (size--)
	{
		value <<= 8;
		value += *from++;
	}

	return value;
}

static int
_MIDI_ReadDelta(track *ptr)
{
	int value = NextByte(ptr);

	if (value & 0x80)
	{
		int c, n = 0;

		value &= 0x7f;

		do
		{
			c = NextByte(ptr);
			value = (value << 7) + (c & 0x7f);
		}
		while ((c & 0x80) && ++n < 4);
	}

	return value;
}

static void
_MIDI_ResetTracks(void)
{
	track *ptr = _MIDI_TrackPtr;
	int i;

	_MIDI_Tick = 0;
	_MIDI_Beat = 1;
	_MIDI_Measure = 1;
	_MIDI_Time = 0;
	_MIDI_BeatsPerMeasure = 4;
	_MIDI_TicksPerBeat = _MIDI_Division;
	_MIDI_TimeBase = 4;
	_MIDI_PositionInTicks = 0;
	_MIDI_ActiveTracks = 0;
	_MIDI_Context = 0;

	for (i = 0; i < _MIDI_NumTracks; ++i, ++ptr)
	{
		ptr->pos = ptr->start;
		ptr->active = ptr->EMIDI_IncludeTrack;

		if (ptr->active)
			_MIDI_ActiveTracks++;

		ptr->delay = _MIDI_ReadDelta(ptr);
		ptr->RunningStatus = 0;
		ptr->currentcontext = 0;
		ptr->context[0].loopstart = ptr->start;
		ptr->context[0].loopcount = 0;
	}
}

static void
_MIDI_AdvanceTick(void)
{
	_MIDI_PositionInTicks++;
	_MIDI_Time += (uint32_t)_MIDI_FPSecondsPerTick;

	_MIDI_Tick++;

	while (_MIDI_Tick > _MIDI_TicksPerBeat && _MIDI_TicksPerBeat > 0)
	{
		_MIDI_Tick -= _MIDI_TicksPerBeat;
		_MIDI_Beat++;
	}

	while (_MIDI_Beat > _MIDI_BeatsPerMeasure && _MIDI_BeatsPerMeasure > 0)
	{
		_MIDI_Beat -= _MIDI_BeatsPerMeasure;
		_MIDI_Measure++;
	}
}

static void
_MIDI_SysEx(track *Track)
{
	int length = _MIDI_ReadDelta(Track);

	if (length > Track->end - Track->pos)
		length = (int)(Track->end - Track->pos);

	Track->pos += length;
}

static void
_MIDI_MetaEvent(track *Track)
{
	int command = NextByte(Track);
	int length = _MIDI_ReadDelta(Track);

	if (length > Track->end - Track->pos)
		length = (int)(Track->end - Track->pos);

	switch (command)
	{
		case MIDI_END_OF_TRACK:
			if (Track->active)
			{
				Track->active = 0;
				_MIDI_ActiveTracks--;
			}
			break;

		case MIDI_TEMPO_CHANGE:
			if (length >= 3)
			{
				int us = _MIDI_ReadNumber(Track->pos, 3);

				if (us > 0)
					MIDI_SetTempo(60000000 / us);
			}
			break;

		case MIDI_TIME_SIGNATURE:
			if (length >= 2)
			{
				int denominator;

				if ((_MIDI_Tick > 0) || (_MIDI_Beat > 1))
					_MIDI_Measure++;

				_MIDI_Tick = 0;
				_MIDI_Beat = 1;
				_MIDI_TimeBase = 1;
				_MIDI_BeatsPerMeasure = Track->pos[0];
				denominator = Track->pos[1];

				while (denominator > 0 && _MIDI_TimeBase < 256)
				{
					_MIDI_TimeBase += _MIDI_TimeBase;
					denominator--;
				}

				_MIDI_TicksPerBeat = _MIDI_Division * 4 / _MIDI_TimeBase;
			}
			break;
	}

	Track->pos += length;
}

static void
_MIDI_SetChannelVolume(int channel, int volume)
{
	_MIDI_ChannelVolume[channel] = volume;
	OUT_ControlChange(channel, MIDI_VOLUME, volume);
}

static int
_MIDI_InterpretControllerInfo(track *Track, int TimeSet, int channel, int c1, int c2)
{
	track *trackptr;
	int tracknum;
	int loopcount;

	switch (c1)
	{
		case MIDI_MONO_MODE_ON:
			Track->pos++;
			break;

		case MIDI_VOLUME:
			if (!Track->EMIDI_VolumeChange)
				_MIDI_SetChannelVolume(channel, c2);
			break;

		case EMIDI_INCLUDE_TRACK:
		case EMIDI_EXCLUDE_TRACK:
			break;

		case EMIDI_PROGRAM_CHANGE:
			if (Track->EMIDI_ProgramChange)
				OUT_ProgramChange(channel, c2 & 0x7f);
			break;

		case EMIDI_VOLUME_CHANGE:
			if (Track->EMIDI_VolumeChange)
				_MIDI_SetChannelVolume(channel, c2);
			break;

		case EMIDI_CONTEXT_START:
			break;

		case EMIDI_CONTEXT_END:
			if ((Track->currentcontext == _MIDI_Context) || (_MIDI_Context < 0) ||
					(Track->context[_MIDI_Context].pos == NULL))
				break;

			Track->currentcontext = (int16_t)_MIDI_Context;
			Track->context[0].loopstart = Track->context[_MIDI_Context].loopstart;
			Track->context[0].loopcount = Track->context[_MIDI_Context].loopcount;
			Track->pos = Track->context[_MIDI_Context].pos;
			Track->RunningStatus = Track->context[_MIDI_Context].RunningStatus;

			if (TimeSet)
				break;

			_MIDI_Time = Track->context[_MIDI_Context].time;
			_MIDI_FPSecondsPerTick = Track->context[_MIDI_Context].FPSecondsPerTick;
			_MIDI_Tick = Track->context[_MIDI_Context].tick;
			_MIDI_Beat = Track->context[_MIDI_Context].beat;
			_MIDI_Measure = Track->context[_MIDI_Context].measure;
			_MIDI_BeatsPerMeasure = Track->context[_MIDI_Context].BeatsPerMeasure;
			_MIDI_TicksPerBeat = Track->context[_MIDI_Context].TicksPerBeat;
			_MIDI_TimeBase = Track->context[_MIDI_Context].TimeBase;
			TimeSet = 1;
			break;

		case EMIDI_LOOP_START:
		case EMIDI_SONG_LOOP_START:
			loopcount = (c2 == 0) ? EMIDI_INFINITE : c2;

			if (c1 == EMIDI_SONG_LOOP_START)
			{
				trackptr = _MIDI_TrackPtr;
				tracknum = _MIDI_NumTracks;
			}
			else
			{
				trackptr = Track;
				tracknum = 1;
			}

			while (tracknum > 0)
			{
				trackptr->context[0].loopcount = (int16_t)loopcount;
				trackptr->context[0].pos = trackptr->pos;
				trackptr->context[0].loopstart = trackptr->pos;
				trackptr->context[0].RunningStatus = trackptr->RunningStatus;
				trackptr->context[0].active = trackptr->active;
				trackptr->context[0].delay = trackptr->delay;
				trackptr->context[0].time = _MIDI_Time;
				trackptr->context[0].FPSecondsPerTick = _MIDI_FPSecondsPerTick;
				trackptr->context[0].tick = (int16_t)_MIDI_Tick;
				trackptr->context[0].beat = (int16_t)_MIDI_Beat;
				trackptr->context[0].measure = (int16_t)_MIDI_Measure;
				trackptr->context[0].BeatsPerMeasure = (int16_t)_MIDI_BeatsPerMeasure;
				trackptr->context[0].TicksPerBeat = (int16_t)_MIDI_TicksPerBeat;
				trackptr->context[0].TimeBase = (int16_t)_MIDI_TimeBase;
				trackptr++;
				tracknum--;
			}
			break;

		case EMIDI_LOOP_END:
		case EMIDI_SONG_LOOP_END:
			if ((c2 != EMIDI_END_LOOP_VALUE) || (Track->context[0].loopstart == NULL) ||
					(Track->context[0].loopcount == 0))
				break;

			if (c1 == EMIDI_SONG_LOOP_END)
			{
				trackptr = _MIDI_TrackPtr;
				tracknum = _MIDI_NumTracks;
				_MIDI_ActiveTracks = 0;
			}
			else
			{
				trackptr = Track;
				tracknum = 1;
				_MIDI_ActiveTracks--;
			}

			while (tracknum > 0)
			{
				if (trackptr->context[0].loopcount != EMIDI_INFINITE)
					trackptr->context[0].loopcount--;

				trackptr->pos = trackptr->context[0].loopstart;
				trackptr->RunningStatus = trackptr->context[0].RunningStatus;
				trackptr->delay = trackptr->context[0].delay;
				trackptr->active = trackptr->context[0].active;

				if (trackptr->active)
					_MIDI_ActiveTracks++;

				if (!TimeSet)
				{
					_MIDI_Time = trackptr->context[0].time;
					_MIDI_FPSecondsPerTick = trackptr->context[0].FPSecondsPerTick;
					_MIDI_Tick = trackptr->context[0].tick;
					_MIDI_Beat = trackptr->context[0].beat;
					_MIDI_Measure = trackptr->context[0].measure;
					_MIDI_BeatsPerMeasure = trackptr->context[0].BeatsPerMeasure;
					_MIDI_TicksPerBeat = trackptr->context[0].TicksPerBeat;
					_MIDI_TimeBase = trackptr->context[0].TimeBase;
					TimeSet = 1;
				}

				trackptr++;
				tracknum--;
			}
			break;

		default:
			OUT_ControlChange(channel, c1, c2);
	}

	return TimeSet;
}

/* One MIDI tick. */
static void
MIDI_ServiceRoutine(void)
{
	track *Track;
	int tracknum = 0;
	int TimeSet = 0;
	int c1 = 0;
	int c2 = 0;

	if (!_MIDI_SongActive || _MIDI_Paused)
		return;

	Track = _MIDI_TrackPtr;

	while (tracknum < _MIDI_NumTracks)
	{
		int guard = 0;

		while ((Track->active) && (Track->delay == 0) && ++guard < 4096)
		{
			int event = NextByte(Track);
			int channel, command;

			if (!Track->active)
				break;

			if (GET_MIDI_COMMAND(event) == MIDI_SPECIAL)
			{
				switch (event)
				{
					case MIDI_SYSEX:
					case MIDI_SYSEX_CONTINUE:
						_MIDI_SysEx(Track);
						break;
					case MIDI_META_EVENT:
						_MIDI_MetaEvent(Track);
						break;
				}

				if (Track->active)
					Track->delay = _MIDI_ReadDelta(Track);

				continue;
			}

			if (event & MIDI_RUNNING_STATUS)
				Track->RunningStatus = (int16_t)event;
			else
			{
				event = Track->RunningStatus;
				Track->pos--;
			}

			channel = GET_MIDI_CHANNEL(event);
			command = GET_MIDI_COMMAND(event);

			if (_MIDI_CommandLengths[command] > 0)
			{
				c1 = NextByte(Track);

				if (_MIDI_CommandLengths[command] > 1)
					c2 = NextByte(Track);
			}

			switch (command)
			{
				case MIDI_NOTE_OFF:
					OUT_NoteOff(channel, c1, c2);
					break;

				case MIDI_NOTE_ON:
					OUT_NoteOn(channel, c1, c2);
					break;

				case MIDI_CONTROL_CHANGE:
					TimeSet = _MIDI_InterpretControllerInfo(Track, TimeSet, channel, c1, c2);
					break;

				case MIDI_PROGRAM_CHANGE:
					if (!Track->EMIDI_ProgramChange)
						OUT_ProgramChange(channel, c1 & 0x7f);
					break;

				case MIDI_PITCH_BEND:
					OUT_SetPitchBend(channel, c1, c2);
					break;

				default:
					break;
			}

			if (Track->active)
				Track->delay = _MIDI_ReadDelta(Track);
		}

		Track->delay--;
		Track++;
		tracknum++;

		if (_MIDI_ActiveTracks <= 0)
		{
			_MIDI_ResetTracks();

			if (_MIDI_Loop)
			{
				tracknum = 0;
				Track = _MIDI_TrackPtr;
			}
			else
			{
				_MIDI_SongActive = 0;
				break;
			}
		}
	}

	_MIDI_AdvanceTick();
}

static void
MIDI_AllNotesOff(void)
{
	int channel;

	for (channel = 0; channel < NUM_MIDI_CHANNELS; channel++)
	{
		OUT_ControlChange(channel, MIDI_HOLD1, 0);
		OUT_ControlChange(channel, MIDI_SOSTENUTO, 0);
		OUT_ControlChange(channel, MIDI_ALL_NOTES_OFF, 0);
		OUT_ControlChange(channel, MIDI_ALL_SOUNDS_OFF, 0);
	}
}

static void
MIDI_Reset(void)
{
	int channel;

	MIDI_AllNotesOff();

	for (channel = 0; channel < NUM_MIDI_CHANNELS; channel++)
	{
		OUT_ControlChange(channel, MIDI_RESET_ALL_CONTROLLERS, 0);
		OUT_ControlChange(channel, MIDI_RPN_MSB, MIDI_PITCHBEND_MSB);
		OUT_ControlChange(channel, MIDI_RPN_LSB, MIDI_PITCHBEND_LSB);
		OUT_ControlChange(channel, MIDI_DATAENTRY_MSB, 2);  /* bend range MSB */
		OUT_ControlChange(channel, MIDI_DATAENTRY_LSB, 0);  /* bend range LSB */
		_MIDI_ChannelVolume[channel] = GENMIDI_DefaultVolume;
		OUT_ControlChange(channel, MIDI_PAN, 64);
		OUT_ControlChange(channel, MIDI_REVERB, 40);
		OUT_ControlChange(channel, MIDI_CHORUS, 0);
		OUT_ControlChange(channel, MIDI_BANK_SELECT_MSB, 0);
		OUT_ControlChange(channel, MIDI_BANK_SELECT_LSB, 0);
		OUT_ProgramChange(channel, 0);
	}

	for (channel = 0; channel < NUM_MIDI_CHANNELS; channel++)
		_MIDI_SetChannelVolume(channel, _MIDI_ChannelVolume[channel]);
}

static void
MIDI_SetTempo(int tempo)
{
	int tickspersecond;

	if (tempo <= 0)
		tempo = 120;

	render_tempo = tempo * _MIDI_Division / 60;
	render_timer = 0;

	tickspersecond = tempo * _MIDI_Division / 60;

	if (tickspersecond <= 0)
		tickspersecond = 1;

	_MIDI_FPSecondsPerTick = (1 << TIME_PRECISION) / tickspersecond;
}

/* EMIDI pre-scan: which tracks play on an AdLib card, loop points. */
static void
_MIDI_InitEMIDI(void)
{
	track *Track = _MIDI_TrackPtr;
	int tracknum = 0;

	_MIDI_ResetTracks();

	while ((tracknum < _MIDI_NumTracks) && (Track != NULL))
	{
		int IncludeFound = 0;
		int guard = 0;

		_MIDI_Tick = 0;
		_MIDI_Beat = 1;
		_MIDI_Measure = 1;
		_MIDI_Time = 0;
		_MIDI_BeatsPerMeasure = 4;
		_MIDI_TicksPerBeat = _MIDI_Division;
		_MIDI_TimeBase = 4;
		_MIDI_PositionInTicks = 0;
		_MIDI_ActiveTracks = 1;   /* NextByte decrements it at the end */
		_MIDI_Context = -1;

		Track->RunningStatus = 0;
		Track->active = 1;
		Track->EMIDI_ProgramChange = 0;
		Track->EMIDI_VolumeChange = 0;
		Track->EMIDI_IncludeTrack = 1;
		memset(Track->context, 0, sizeof(Track->context));

		while (Track->active && ++guard < 1000000)
		{
			int event = NextByte(Track);
			int command, length;

			if (!Track->active)
				break;

			if (GET_MIDI_COMMAND(event) == MIDI_SPECIAL)
			{
				switch (event)
				{
					case MIDI_SYSEX:
					case MIDI_SYSEX_CONTINUE:
						_MIDI_SysEx(Track);
						break;
					case MIDI_META_EVENT:
						_MIDI_MetaEvent(Track);
						break;
				}

				if (Track->active)
					Track->delay = _MIDI_ReadDelta(Track);

				continue;
			}

			if (event & MIDI_RUNNING_STATUS)
				Track->RunningStatus = (int16_t)event;
			else
			{
				event = Track->RunningStatus;
				Track->pos--;
			}

			command = GET_MIDI_COMMAND(event);
			length = _MIDI_CommandLengths[command];

			if (command == MIDI_CONTROL_CHANGE)
			{
				int c1, c2;

				if (Track->pos < Track->end && *Track->pos == MIDI_MONO_MODE_ON)
					length++;

				c1 = NextByte(Track);
				c2 = NextByte(Track);
				length -= 2;

				switch (c1)
				{
					case EMIDI_LOOP_START:
					case EMIDI_SONG_LOOP_START:
						Track->context[0].loopcount = (int16_t)((c2 == 0) ? EMIDI_INFINITE : c2);
						Track->context[0].pos = Track->pos;
						Track->context[0].loopstart = Track->pos;
						Track->context[0].RunningStatus = Track->RunningStatus;
						break;

					case EMIDI_LOOP_END:
					case EMIDI_SONG_LOOP_END:
						if (c2 == EMIDI_END_LOOP_VALUE)
						{
							Track->context[0].loopstart = NULL;
							Track->context[0].loopcount = 0;
						}
						break;

					case EMIDI_INCLUDE_TRACK:
						if (EMIDI_AffectsCurrentCard(c2, EMIDI_Card))
						{
							IncludeFound = 1;
							Track->EMIDI_IncludeTrack = 1;
						}
						else if (!IncludeFound)
						{
							IncludeFound = 1;
							Track->EMIDI_IncludeTrack = 0;
						}
						break;

					case EMIDI_EXCLUDE_TRACK:
						if (EMIDI_AffectsCurrentCard(c2, EMIDI_Card))
							Track->EMIDI_IncludeTrack = 0;
						break;

					case EMIDI_PROGRAM_CHANGE:
						Track->EMIDI_ProgramChange = 1;
						break;

					case EMIDI_VOLUME_CHANGE:
						Track->EMIDI_VolumeChange = 1;
						break;

					case EMIDI_CONTEXT_START:
						if ((c2 > 0) && (c2 < EMIDI_NUM_CONTEXTS))
						{
							Track->context[c2].pos = Track->pos;
							Track->context[c2].loopstart = Track->context[0].loopstart;
							Track->context[c2].loopcount = Track->context[0].loopcount;
							Track->context[c2].RunningStatus = Track->RunningStatus;
						}
						break;
				}
			}

			if (length > 0)
			{
				if (length > Track->end - Track->pos)
					length = (int)(Track->end - Track->pos);

				Track->pos += length;
			}

			if (Track->active)
				Track->delay = _MIDI_ReadDelta(Track);
		}

		Track++;
		tracknum++;
	}

	_MIDI_ResetTracks();
}

/* ================================================================ */
/* Interface                                                          */
/* ================================================================ */

static int initialized;

/* the song playing, to start it again on the other synth */
static const uint8_t *cur_song;
static size_t cur_size;
static int cur_loop;

void
PS3MIDI_Init(int rate)
{
	out_rate = rate > 0 ? rate : 48000;

	OPL3_Reset(&AL_Chip, (uint32_t)out_rate);
	AL_Reset();
	AL_ResetVoices();
	AL_CalcPitchInfo();

	initialized = 1;
}

void
PS3MIDI_Stop(void)
{
	if (!_MIDI_SongLoaded)
		return;

	_MIDI_SongActive = 0;
	_MIDI_SongLoaded = 0;
	render_tempo = -1;

	MIDI_Reset();

	free(_MIDI_TrackPtr);
	_MIDI_TrackPtr = NULL;
	_MIDI_NumTracks = 0;

	/* silence whatever is still ringing */
	OPL3_Reset(&AL_Chip, (uint32_t)out_rate);
	AL_Reset();
	AL_ResetVoices();

	SF_Silence();

	cur_song = NULL;
}

void
PS3MIDI_SetSoundFont(void *sf)
{
	const uint8_t *song = cur_song;
	size_t size = cur_size;
	int loop = cur_loop, playing = _MIDI_SongLoaded && _MIDI_SongActive;
	int paused = _MIDI_Paused;

	if ((tsf *)sf == SF_Synth)
		return;

	/* the song starts over on the new synth: the old one's channel
	   state (programs, volumes) is not the new one's */
	PS3MIDI_Stop();

	SF_Synth = (tsf *)sf;
	EMIDI_Card = SF_Synth ? EMIDI_GeneralMIDI : EMIDI_AdLib;

	SF_Silence();

	if (song && playing)
	{
		PS3MIDI_Play(song, size, loop);

		if (paused)
			PS3MIDI_Pause(1);
	}
}

int
PS3MIDI_Play(const uint8_t *song, size_t size, int loop)
{
	const uint8_t *ptr, *end;
	int headersize, format, numtracks, division, i;
	track *tracks;

	if (!initialized)
		PS3MIDI_Init(out_rate);

	if (!song || size < 14 || memcmp(song, "MThd", 4) != 0)
		return -1;

	end = song + size;
	headersize = _MIDI_ReadNumber(song + 4, 4);
	format = _MIDI_ReadNumber(song + 8, 2);
	numtracks = _MIDI_ReadNumber(song + 10, 2);
	division = _MIDI_ReadNumber(song + 12, 2);

	/* division is signed 16 bit: negative = SMPTE, just use 96 */
	if (division & 0x8000)
		division = 96;

	if (format > 1 || numtracks <= 0 || division <= 0 || headersize < 6 ||
			(size_t)headersize > size - 8)
		return -1;

	tracks = calloc((size_t)numtracks, sizeof(track));

	if (!tracks)
		return -1;

	ptr = song + 8 + headersize;

	for (i = 0; i < numtracks; i++)
	{
		int tracklength;

		if (end - ptr < 8 || memcmp(ptr, "MTrk", 4) != 0)
		{
			free(tracks);
			return -1;
		}

		tracklength = _MIDI_ReadNumber(ptr + 4, 4);
		ptr += 8;

		if (tracklength < 0 || tracklength > end - ptr)
			tracklength = (int)(end - ptr);

		tracks[i].start = ptr;
		tracks[i].end = ptr + tracklength;
		tracks[i].EMIDI_IncludeTrack = 0;
		ptr += tracklength;
	}

	PS3MIDI_Stop();

	cur_song = song;
	cur_size = size;
	cur_loop = loop;

	_MIDI_Loop = loop;
	_MIDI_NumTracks = numtracks;
	_MIDI_Division = division;
	_MIDI_TrackPtr = tracks;
	_MIDI_Paused = 0;

	_MIDI_InitEMIDI();
	_MIDI_ResetTracks();
	MIDI_Reset();

	MIDI_SetTempo(120);

	_MIDI_SongLoaded = 1;
	_MIDI_SongActive = 1;

	return 0;
}

void
PS3MIDI_Pause(int paused)
{
	if (!_MIDI_SongLoaded)
		return;

	if (paused && !_MIDI_Paused)
	{
		_MIDI_Paused = 1;
		MIDI_AllNotesOff();
	}
	else if (!paused)
	{
		_MIDI_Paused = 0;
	}
}

int
PS3MIDI_Active(void)
{
	return _MIDI_SongLoaded && _MIDI_SongActive;
}

void
PS3MIDI_SetVolume(int volume)
{
	AL_Volume = Clamp(volume, 0, MIDI_MaxVolume);
}

/* SoundFont output level: TinySoundFont gives about +-1.0 per loud
   voice; this puts a General MIDI song near the AdLib driver's level. */
#define SF_LEVEL 15000.0f

#define SF_CHUNK 256

void
PS3MIDI_Render(int32_t *mix, int frames, float gain)
{
	static float sfbuf[SF_CHUNK * 2];
	float scale;

	if (!_MIDI_SongLoaded || !initialized)
		return;

	if (SF_Synth)
		scale = SF_LEVEL * (float)AL_Volume * (1.0f / MIDI_MaxVolume) * gain;
	else
		scale = AL_PostAmp * (float)AL_Volume * (1.0f / MIDI_MaxVolume) * gain;

	while (frames > 0)
	{
		int n = frames, i;

		/* one MIDI tick every (rate / ticks per second) samples: render
		   up to the next one in one go */
		if (render_tempo > 0 && _MIDI_SongActive && !_MIDI_Paused)
		{
			int left;

			while (render_timer >= out_rate)
			{
				MIDI_ServiceRoutine();
				render_timer -= out_rate;
			}

			if (render_tempo > 0)
			{
				left = (out_rate - render_timer + render_tempo - 1) / render_tempo;

				if (left < 1)
					left = 1;

				if (left < n)
					n = left;

				render_timer += n * render_tempo;
			}
		}

		if (SF_Synth)
		{
			if (n > SF_CHUNK)
				n = SF_CHUNK;

			tsf_render_float(SF_Synth, sfbuf, n, 0);

			for (i = 0; i < n; i++)
			{
				mix[0] += (int32_t)(sfbuf[2 * i] * scale);
				mix[1] += (int32_t)(sfbuf[2 * i + 1] * scale);
				mix += 2;
			}
		}
		else
		{
			for (i = 0; i < n; i++)
			{
				int16_t buf[2];

				OPL3_GenerateResampled(&AL_Chip, buf);

				mix[0] += (int32_t)(buf[0] * scale);
				mix[1] += (int32_t)(buf[1] * scale);
				mix += 2;
			}
		}

		frames -= n;
	}
}
