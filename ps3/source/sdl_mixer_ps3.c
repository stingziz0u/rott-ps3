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
 * The SDL_mixer shim (ps3/compat/SDL_mixer.h): sound effects and music
 * mixed by one thread into the PS3's audio port.
 *
 * The port (the scheme of Doom64-PS3, CrispyCell and the Quake II port,
 * proven on hardware -- handoff notes 3.5 to 3.8, 3.40, 3.41):
 *
 *  - ONE port for everything (audioSetNotifyEventQueue is global), 48000
 *    Hz stereo float, 16 blocks of 256 frames, audioPortStart;
 *  - a thread woken by the port's notify queue keeps the blocks up to
 *    AUDIO_AHEAD in front of the one being played (readIndex) filled; if
 *    it falls behind it jumps back in front instead of writing late;
 *  - the engine's calls and the thread's mixing are serialized by one
 *    mutex; shutdown lowers a flag and joins the thread, and never
 *    destroys a mutex the thread may still hold.
 *
 * Sound effects: ROTT's lumps are VOC (and a few WAV) files, 8 bit mono
 * at ~7-11 kHz. They are decoded once to 16 bit mono at their own rate,
 * and resampled to 48 kHz while mixing. The resampler's position and
 * step are 64 bit (a 16.16 position in 32 bits wraps after 65536 input
 * samples: the bug that restarted Strife's voices, notes 3.40).
 *
 * Music: MIDI songs, played by the Apogee Sound System's AdLib driver on
 * an emulated OPL3 (ps3_midi.c).
 *
 * The host harness builds this file without __PPU__: the "port" is then
 * a thread that renders at real-time pace, optionally into a WAV file.
 *
 * =======================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL_mixer.h"
#include "ps3_platform.h"
#include "ps3_midi.h"

#define AUDIO_RATE          48000
#define AUDIO_CHANNELS      2
#define AUDIO_BLOCK_FRAMES  256
#define AUDIO_AHEAD         4       /* blocks, ~21 ms */
#define MAX_MIX_CHANNELS    32

#ifdef __PPU__
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/event_queue.h>
#include <audio/audio.h>

static sys_mutex_t mix_mutex;
#define MIX_LOCK()   sysMutexLock(mix_mutex, 0)
#define MIX_UNLOCK() sysMutexUnlock(mix_mutex)
#else
#include <pthread.h>
#include <time.h>

static pthread_mutex_t mix_mutex = PTHREAD_MUTEX_INITIALIZER;
#define MIX_LOCK()   pthread_mutex_lock(&mix_mutex)
#define MIX_UNLOCK() pthread_mutex_unlock(&mix_mutex)
#endif

/* ================================================================ */
/* State                                                              */
/* ================================================================ */

typedef struct
{
	Mix_Chunk *chunk;
	int playing;
	uint64_t pos;          /* 32.16 fixed point, in the chunk's samples */
	uint64_t step;
	int left, right;       /* panning 0..255 */
	int volume;            /* 0..128 */
} mixchannel_t;

struct _Mix_Music
{
	Uint8 *data;
	size_t size;
};

static int audio_open;
static volatile int audio_quit;
static int num_channels = 8;
static mixchannel_t channels[MAX_MIX_CHANNELS];
static int master_volume = MIX_MAX_VOLUME;

static Mix_Music *music_current;
static int music_volume = MIX_MAX_VOLUME;
static int music_paused;
static int music_fade_total;        /* samples; 0 = not fading */
static int music_fade_left;
static void (*music_hook)(void *udata, Uint8 *stream, int len);
static void *music_hook_arg;

static unsigned audio_late;         /* times the thread fell behind */

/* ================================================================ */
/* Mixing (under the lock)                                            */
/* ================================================================ */

static int32_t mixbuf[AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS];
static int16_t hookbuf[AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS];

static void
MixChannels(void)
{
	int c, i;

	for (c = 0; c < num_channels; c++)
	{
		mixchannel_t *ch = &channels[c];
		const int16_t *samples;
		uint64_t len, pos, step;
		int32_t vl, vr;
		int32_t *out = mixbuf;

		if (!ch->playing || !ch->chunk)
		{
			continue;
		}

		samples = (const int16_t *)ch->chunk->abuf;
		len = (uint64_t)(ch->chunk->alen / 2) << 16;
		pos = ch->pos;
		step = ch->step;

		/* 0..255 pan x 0..128 volume x 0..128 master -> 0..(1<<16) */
		vl = (int32_t)(((int64_t)ch->left * ch->volume * master_volume) >> 8);
		vr = (int32_t)(((int64_t)ch->right * ch->volume * master_volume) >> 8);

		for (i = 0; i < AUDIO_BLOCK_FRAMES; i++)
		{
			int32_t s;

			if (pos >= len)
			{
				ch->playing = 0;
				break;
			}

			s = samples[pos >> 16];
			out[0] += (s * vl) >> 14;
			out[1] += (s * vr) >> 14;
			out += 2;
			pos += step;
		}

		ch->pos = pos;
	}
}

static void
MixMusic(void)
{
	float gain = 1.0f;

	if (!music_current && !music_hook)
	{
		return;
	}

	if (music_fade_total > 0)
	{
		if (music_fade_left <= 0)
		{
			/* faded out: stop, as SDL_mixer does */
			music_fade_total = 0;
			PS3MIDI_Stop();
			music_current = NULL;
			return;
		}

		gain = (float)music_fade_left / (float)music_fade_total;
		music_fade_left -= AUDIO_BLOCK_FRAMES;
	}

	if (music_hook)
	{
		int i;

		memset(hookbuf, 0, sizeof(hookbuf));
		music_hook(music_hook_arg, (Uint8 *)hookbuf, (int)sizeof(hookbuf));

		for (i = 0; i < AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS; i++)
		{
			mixbuf[i] += (int32_t)(hookbuf[i] * gain);
		}

		return;
	}

	if (!music_paused)
	{
		PS3MIDI_Render(mixbuf, AUDIO_BLOCK_FRAMES, gain);
	}

	if (!PS3MIDI_Active())
	{
		/* a song played once has ended */
		music_current = NULL;
	}
}

static float dc_x[2], dc_y[2];

/* One block of 256 stereo frames, as floats (the port's format). */
static void
MixBlock(float *dst)
{
	int i;

	memset(mixbuf, 0, sizeof(mixbuf));

	MIX_LOCK();
	MixChannels();
	MixMusic();
	MIX_UNLOCK();

	for (i = 0; i < AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS; i++)
	{
		/* DC blocker (one pole high-pass at ~8 Hz): the OPL's FM with
		   feedback drifts off zero, which eats headroom and clicks when
		   the music stops */
		float x = (float)mixbuf[i];
		float y = x - dc_x[i & 1] + 0.999f * dc_y[i & 1];
		int32_t s;

		dc_x[i & 1] = x;
		dc_y[i & 1] = y;
		s = (int32_t)y;

		if (s > 32767)
		{
			s = 32767;
		}
		else if (s < -32768)
		{
			s = -32768;
		}

		dst[i] = (float)s * (1.0f / 32768.0f);
	}
}

/* ================================================================ */
/* The port                                                           */
/* ================================================================ */

#ifdef __PPU__

static u32 audio_port;
static audioPortConfig audio_cfg;
static sys_event_queue_t audio_queue;
static sys_ipc_key_t audio_key;
static sys_ppu_thread_t audio_tid;
static int mutex_ok;

static float *
AudioBlock(u32 index)
{
	return (float *)(u64)audio_cfg.audioDataStart +
		(u64)index * AUDIO_BLOCK_FRAMES * audio_cfg.channelCount;
}

static void
AudioThread(void *arg)
{
	u32 blocks = (u32)audio_cfg.numBlocks;
	u32 next = 1;   /* next block to fill */
	sys_event_t ev;

	(void)arg;

	while (!audio_quit)
	{
		u32 playing, dist;

		/* one event per block played; also fill on timeout (notes 3.41) */
		sysEventQueueReceive(audio_queue, &ev, 20 * 1000);

		if (audio_quit)
		{
			break;
		}

		__asm__ volatile("lwsync" ::: "memory");

		playing = (u32)(*(volatile u64 *)(u64)audio_cfg.readIndex % blocks);
		dist = (next + blocks - playing) % blocks;

		if (dist == 0 || dist > AUDIO_AHEAD + 1)
		{
			/* the port caught up with us: start again right after it */
			if (dist == 0)
			{
				audio_late++;
			}

			next = (playing + 1) % blocks;
			dist = 1;
		}

		while (dist <= AUDIO_AHEAD)
		{
			MixBlock(AudioBlock(next));
			next = (next + 1) % blocks;
			dist++;
		}
	}

	sysThreadExit(0);
}

static int
PortOpen(void)
{
	audioPortParam param;
	sys_mutex_attr_t attr;
	s32 r;

	if (!mutex_ok)
	{
		memset(&attr, 0, sizeof(attr));
		attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
		attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
		attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
		attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
		strcpy(attr.name, "rottmix");

		if (sysMutexCreate(&mix_mutex, &attr) != 0)
		{
			PS3_Log("[audio] sysMutexCreate failed");
			return -1;
		}

		/* never destroyed: the thread may hold it at shutdown (notes 3.8) */
		mutex_ok = 1;
	}

	r = audioInit();

	if (r != 0)
	{
		PS3_Log("[audio] audioInit failed (%d)", (int)r);
		return -1;
	}

	memset(&param, 0, sizeof(param));
	param.numChannels = AUDIO_PORT_2CH;
	param.numBlocks = AUDIO_BLOCK_16;
	param.attrib = 0;
	param.level = 1.0f;

	r = audioPortOpen(&param, &audio_port);

	if (r != 0)
	{
		PS3_Log("[audio] audioPortOpen failed (%d)", (int)r);
		audioQuit();
		return -1;
	}

	audioGetPortConfig(audio_port, &audio_cfg);
	audioCreateNotifyEventQueue(&audio_queue, &audio_key);
	audioSetNotifyEventQueue(audio_key);
	sysEventQueueDrain(audio_queue);
	memset((void *)(u64)audio_cfg.audioDataStart, 0, audio_cfg.portSize);

	audio_quit = 0;
	audio_late = 0;

	audioPortStart(audio_port);

	/* priority 500: runs before the game thread (1000) */
	if (sysThreadCreate(&audio_tid, AudioThread, NULL, 500, 64 * 1024,
				THREAD_JOINABLE, "rottaudio") != 0)
	{
		PS3_Log("[audio] sysThreadCreate failed");
		audioPortStop(audio_port);
		audioRemoveNotifyEventQueue(audio_key);
		audioPortClose(audio_port);
		sysEventQueueDestroy(audio_queue, 0);
		audioQuit();
		return -1;
	}

	PS3_Log("[audio] port %u open: %u blocks, %u channels, %d Hz",
			(unsigned)audio_port, (unsigned)audio_cfg.numBlocks,
			(unsigned)audio_cfg.channelCount, AUDIO_RATE);

	return 0;
}

static void
PortClose(void)
{
	u64 rv;

	audio_quit = 1;
	sysThreadJoin(audio_tid, &rv);

	audioPortStop(audio_port);
	audioRemoveNotifyEventQueue(audio_key);
	audioPortClose(audio_port);
	sysEventQueueDestroy(audio_queue, 0);
	audioQuit();
}

#else /* host harness */

static pthread_t audio_thread;
static FILE *wav_out;
static unsigned long wav_frames;

static void
WavHeader(FILE *f, unsigned long frames)
{
	unsigned long data = frames * 4;
	unsigned char h[44] = {
		'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
		'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
		0x80, 0xbb, 0, 0, 0, 0xee, 2, 0, 4, 0, 16, 0,
		'd', 'a', 't', 'a', 0, 0, 0, 0
	};

	h[4] = (unsigned char)((data + 36) & 0xff);
	h[5] = (unsigned char)(((data + 36) >> 8) & 0xff);
	h[6] = (unsigned char)(((data + 36) >> 16) & 0xff);
	h[7] = (unsigned char)(((data + 36) >> 24) & 0xff);
	h[40] = (unsigned char)(data & 0xff);
	h[41] = (unsigned char)((data >> 8) & 0xff);
	h[42] = (unsigned char)((data >> 16) & 0xff);
	h[43] = (unsigned char)((data >> 24) & 0xff);
	fseek(f, 0, SEEK_SET);
	fwrite(h, 1, 44, f);
}

static void *
HostAudioThread(void *arg)
{
	float block[AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS];
	struct timespec ts = {0, 5333333};   /* 256 frames at 48 kHz */

	(void)arg;

	while (!audio_quit)
	{
		MixBlock(block);

		if (wav_out)
		{
			int i;

			for (i = 0; i < AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS; i++)
			{
				int v = (int)(block[i] * 32767.0f);

				fputc(v & 0xff, wav_out);
				fputc((v >> 8) & 0xff, wav_out);
			}

			wav_frames += AUDIO_BLOCK_FRAMES;
		}

		nanosleep(&ts, NULL);
	}

	return NULL;
}

static int
PortOpen(void)
{
	const char *wav = getenv("ROTT_HOST_WAV");

	if (wav)
	{
		wav_out = fopen(wav, "wb");

		if (wav_out)
		{
			WavHeader(wav_out, 0);
		}
	}

	audio_quit = 0;

	if (pthread_create(&audio_thread, NULL, HostAudioThread, NULL) != 0)
	{
		return -1;
	}

	PS3_Log("[audio] host audio thread started%s", wav_out ? " (recording)" : "");

	return 0;
}

static void
PortClose(void)
{
	audio_quit = 1;
	pthread_join(audio_thread, NULL);

	if (wav_out)
	{
		WavHeader(wav_out, wav_frames);
		fclose(wav_out);
		wav_out = NULL;
	}
}

#endif

/* ================================================================ */
/* Device                                                             */
/* ================================================================ */

int
Mix_OpenAudioDevice(int frequency, Uint16 format, int nchannels, int chunksize,
		const char *device, int allowed_changes)
{
	(void)frequency;
	(void)format;
	(void)nchannels;
	(void)chunksize;
	(void)device;
	(void)allowed_changes;

	if (audio_open)
	{
		return 0;
	}

	PS3MIDI_Init(AUDIO_RATE);

	if (PortOpen() != 0)
	{
		return -1;
	}

	audio_open = 1;

	return 0;
}

int
Mix_QuerySpec(int *frequency, Uint16 *format, int *nchannels)
{
	if (!audio_open)
	{
		return 0;
	}

	if (frequency) *frequency = AUDIO_RATE;
	if (format) *format = AUDIO_S16SYS;
	if (nchannels) *nchannels = AUDIO_CHANNELS;

	return 1;
}

void
Mix_CloseAudio(void)
{
	if (!audio_open)
	{
		return;
	}

	PortClose();
	audio_open = 0;

	MIX_LOCK();
	memset(channels, 0, sizeof(channels));
	PS3MIDI_Stop();
	music_current = NULL;
	MIX_UNLOCK();

	PS3_Log("[audio] closed (the mixer fell behind %u times)", audio_late);
}

int
Mix_AllocateChannels(int numchans)
{
	if (numchans < 0)
	{
		return num_channels;
	}

	if (numchans > MAX_MIX_CHANNELS)
	{
		numchans = MAX_MIX_CHANNELS;
	}

	MIX_LOCK();
	num_channels = numchans;
	MIX_UNLOCK();

	return num_channels;
}

/* ================================================================ */
/* Sound files                                                        */
/* ================================================================ */

static unsigned
LE16(const Uint8 *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned
LE24(const Uint8 *p)
{
	return LE16(p) | ((unsigned)p[2] << 16);
}

static unsigned
LE32(const Uint8 *p)
{
	return LE24(p) | ((unsigned)p[3] << 24);
}

/* Appends 'count' samples (8 bit unsigned or 16 bit little-endian signed,
   'chans' interleaved channels mixed down to one) to a growing buffer. */
typedef struct
{
	int16_t *s;
	size_t n, cap;
	int rate;
} pcmbuf_t;

static int
PcmAppend(pcmbuf_t *b, const Uint8 *data, size_t bytes, int bits, int chans)
{
	size_t frame = (size_t)(bits / 8) * (size_t)chans;
	size_t count, i;

	if (frame == 0)
	{
		return -1;
	}

	count = bytes / frame;

	if (b->n + count > b->cap)
	{
		size_t cap = (b->n + count) * 2 + 1024;
		int16_t *s = realloc(b->s, cap * sizeof(int16_t));

		if (!s)
		{
			return -1;
		}

		b->s = s;
		b->cap = cap;
	}

	for (i = 0; i < count; i++)
	{
		int c, sum = 0;

		for (c = 0; c < chans; c++)
		{
			const Uint8 *p = data + i * frame + (size_t)c * (size_t)(bits / 8);

			sum += (bits == 8) ? ((int)p[0] - 128) << 8 : (int16_t)LE16(p);
		}

		b->s[b->n++] = (int16_t)(sum / chans);
	}

	return 0;
}

static int
DecodeWav(const Uint8 *d, size_t size, pcmbuf_t *out)
{
	size_t pos = 12;
	int chans = 0, bits = 0, fmt_ok = 0;

	while (pos + 8 <= size)
	{
		unsigned id_len = LE32(d + pos + 4);
		const Uint8 *body = d + pos + 8;
		size_t len = id_len;

		if (len > size - pos - 8)
		{
			len = size - pos - 8;
		}

		if (!memcmp(d + pos, "fmt ", 4) && len >= 16)
		{
			if (LE16(body) != 1)
			{
				PS3_Log("[audio] WAV: compressed format %u not supported", LE16(body));
				return -1;
			}

			chans = (int)LE16(body + 2);
			out->rate = (int)LE32(body + 4);
			bits = (int)LE16(body + 14);
			fmt_ok = (chans >= 1 && chans <= 2 && (bits == 8 || bits == 16) &&
					out->rate > 0);
		}
		else if (!memcmp(d + pos, "data", 4))
		{
			if (!fmt_ok)
			{
				return -1;
			}

			return PcmAppend(out, body, len, bits, chans);
		}

		pos += 8 + id_len + (id_len & 1);
	}

	return -1;
}

static int
DecodeVoc(const Uint8 *d, size_t size, pcmbuf_t *out)
{
	size_t pos = LE16(d + 20);
	int ext_rate = 0, ext_chans = 1;

	if (pos < 26 || pos > size)
	{
		pos = 26;
	}

	while (pos < size)
	{
		int type = d[pos];
		size_t len;
		const Uint8 *body;

		if (type == 0 || pos + 4 > size)
		{
			break;   /* terminator */
		}

		len = LE24(d + pos + 1);
		body = d + pos + 4;

		if (len > size - pos - 4)
		{
			len = size - pos - 4;
		}

		switch (type)
		{
			case 1:   /* sound data */
				if (len >= 2)
				{
					int rate = ext_rate ? ext_rate : 1000000 / (256 - body[0]);

					if (body[1] != 0)
					{
						PS3_Log("[audio] VOC: codec %d not supported", body[1]);
						return out->n ? 0 : -1;
					}

					if (!out->rate)
					{
						out->rate = rate;
					}

					if (PcmAppend(out, body + 2, len - 2, 8, ext_chans) != 0)
					{
						return -1;
					}

					ext_rate = 0;
					ext_chans = 1;
				}
				break;

			case 2:   /* continuation */
				if (PcmAppend(out, body, len, 8, 1) != 0)
				{
					return -1;
				}
				break;

			case 3:   /* silence */
				if (len >= 3)
				{
					size_t n = LE16(body) + 1, i;
					Uint8 *zero = malloc(n);

					if (!zero)
					{
						return -1;
					}

					for (i = 0; i < n; i++)
					{
						zero[i] = 0x80;
					}

					if (!out->rate)
					{
						out->rate = 1000000 / (256 - body[2]);
					}

					PcmAppend(out, zero, n, 8, 1);
					free(zero);
				}
				break;

			case 8:   /* extended: rate and channels of the next block 1 */
				if (len >= 4)
				{
					unsigned tc = LE16(body);

					ext_chans = body[3] ? 2 : 1;
					ext_rate = (int)(256000000u / (65536u - tc) / (unsigned)ext_chans);
				}
				break;

			case 9:   /* new format */
				if (len >= 12)
				{
					int rate = (int)LE32(body);
					int bits = body[4];
					int chans = body[5];
					unsigned codec = LE16(body + 6);

					if ((codec != 0 && codec != 4) || (bits != 8 && bits != 16) ||
							chans < 1 || chans > 2)
					{
						PS3_Log("[audio] VOC: format %u/%d bit not supported", codec, bits);
						return out->n ? 0 : -1;
					}

					if (!out->rate)
					{
						out->rate = rate;
					}

					if (PcmAppend(out, body + 12, len - 12, bits, chans) != 0)
					{
						return -1;
					}
				}
				break;

			default:  /* text, repeat markers...: skipped */
				break;
		}

		pos += 4 + len;
	}

	return out->n ? 0 : -1;
}

Mix_Chunk *
Mix_LoadWAV_RW(SDL_RWops *src, int freesrc)
{
	pcmbuf_t pcm;
	Mix_Chunk *chunk = NULL;
	int r = -1;

	if (!src)
	{
		return NULL;
	}

	memset(&pcm, 0, sizeof(pcm));

	if (src->size >= 44 && !memcmp(src->base, "RIFF", 4) && !memcmp(src->base + 8, "WAVE", 4))
	{
		r = DecodeWav(src->base, src->size, &pcm);
	}
	else if (src->size >= 26 && !memcmp(src->base, "Creative Voice File", 19))
	{
		r = DecodeVoc(src->base, src->size, &pcm);
	}

	if (r == 0 && pcm.n > 0 && pcm.rate > 0)
	{
		chunk = calloc(1, sizeof(*chunk));

		if (chunk)
		{
			chunk->allocated = 1;
			chunk->abuf = (Uint8 *)pcm.s;
			chunk->alen = (Uint32)(pcm.n * 2);
			chunk->volume = MIX_MAX_VOLUME;
			chunk->rate = pcm.rate;
			pcm.s = NULL;
		}
	}

	free(pcm.s);

	if (freesrc)
	{
		SDL_RWclose(src);
	}

	return chunk;
}

void
Mix_FreeChunk(Mix_Chunk *chunk)
{
	int c;

	if (!chunk)
	{
		return;
	}

	MIX_LOCK();

	for (c = 0; c < MAX_MIX_CHANNELS; c++)
	{
		if (channels[c].chunk == chunk)
		{
			channels[c].playing = 0;
			channels[c].chunk = NULL;
		}
	}

	MIX_UNLOCK();

	free(chunk->abuf);
	free(chunk);
}

/* ================================================================ */
/* Channels                                                           */
/* ================================================================ */

int
Mix_PlayChannelTimed(int channel, Mix_Chunk *chunk, int loops, int ticks)
{
	(void)loops;   /* ROTT never loops a sound through the mixer */
	(void)ticks;

	if (!chunk || !audio_open)
	{
		return -1;
	}

	MIX_LOCK();

	if (channel < 0)
	{
		for (channel = 0; channel < num_channels; channel++)
		{
			if (!channels[channel].playing)
			{
				break;
			}
		}
	}

	if (channel < 0 || channel >= num_channels)
	{
		MIX_UNLOCK();
		return -1;
	}

	channels[channel].chunk = chunk;
	channels[channel].pos = 0;
	channels[channel].step = ((uint64_t)chunk->rate << 16) / AUDIO_RATE;
	channels[channel].playing = 1;

	if (channels[channel].volume == 0 && channels[channel].left == 0 &&
			channels[channel].right == 0)
	{
		/* never set up: SDL_mixer's defaults */
		channels[channel].volume = MIX_MAX_VOLUME;
		channels[channel].left = channels[channel].right = 255;
	}

	MIX_UNLOCK();

	return channel;
}

int
Mix_HaltChannel(int channel)
{
	int c;

	MIX_LOCK();

	for (c = 0; c < num_channels; c++)
	{
		if (channel < 0 || c == channel)
		{
			channels[c].playing = 0;
		}
	}

	MIX_UNLOCK();

	return 0;
}

int
Mix_Playing(int channel)
{
	int c, n = 0;

	MIX_LOCK();

	for (c = 0; c < num_channels; c++)
	{
		if ((channel < 0 || c == channel) && channels[c].playing)
		{
			n++;
		}
	}

	MIX_UNLOCK();

	return n;
}

int
Mix_SetPanning(int channel, Uint8 left, Uint8 right)
{
	if (channel < 0 || channel >= MAX_MIX_CHANNELS)
	{
		return 0;
	}

	MIX_LOCK();
	channels[channel].left = left;
	channels[channel].right = right;

	if (channels[channel].volume == 0 && !channels[channel].playing)
	{
		channels[channel].volume = MIX_MAX_VOLUME;
	}

	MIX_UNLOCK();

	return 1;
}

int
Mix_Volume(int channel, int volume)
{
	int c, prev;

	if (channel >= MAX_MIX_CHANNELS)
	{
		return 0;
	}

	prev = channels[channel < 0 ? 0 : channel].volume;

	if (volume < 0)
	{
		return prev;
	}

	if (volume > MIX_MAX_VOLUME)
	{
		volume = MIX_MAX_VOLUME;
	}

	MIX_LOCK();

	for (c = 0; c < MAX_MIX_CHANNELS; c++)
	{
		if (channel < 0 || c == channel)
		{
			channels[c].volume = volume;
		}
	}

	MIX_UNLOCK();

	return prev;
}

int
Mix_MasterVolume(int volume)
{
	int prev = master_volume;

	if (volume >= 0)
	{
		master_volume = volume > MIX_MAX_VOLUME ? MIX_MAX_VOLUME : volume;
	}

	return prev;
}

/* ================================================================ */
/* Music                                                              */
/* ================================================================ */

Mix_Music *
Mix_LoadMUS_RW(SDL_RWops *src, int freesrc)
{
	Mix_Music *m = NULL;

	if (!src)
	{
		return NULL;
	}

	if (src->size >= 14 && !memcmp(src->base, "MThd", 4))
	{
		m = calloc(1, sizeof(*m));

		if (m)
		{
			/* a copy: the engine may free its lump while we play */
			m->data = malloc(src->size);

			if (m->data)
			{
				memcpy(m->data, src->base, src->size);
				m->size = src->size;
			}
			else
			{
				free(m);
				m = NULL;
			}
		}
	}
	else
	{
		PS3_Log("[audio] Mix_LoadMUS_RW: not a MIDI song (%u bytes)", (unsigned)src->size);
	}

	if (freesrc)
	{
		SDL_RWclose(src);
	}

	return m;
}

void
Mix_FreeMusic(Mix_Music *music)
{
	if (!music)
	{
		return;
	}

	MIX_LOCK();

	if (music_current == music)
	{
		PS3MIDI_Stop();
		music_current = NULL;
	}

	MIX_UNLOCK();

	free(music->data);
	free(music);
}

int
Mix_PlayMusic(Mix_Music *music, int loops)
{
	int r;

	if (!music)
	{
		return -1;
	}

	MIX_LOCK();

	music_fade_total = 0;
	music_paused = 0;
	PS3MIDI_SetVolume(music_volume * 2);
	r = PS3MIDI_Play(music->data, music->size, loops != 0);
	music_current = (r == 0) ? music : NULL;

	MIX_UNLOCK();

	if (r != 0)
	{
		PS3_Log("[audio] Mix_PlayMusic: the song didn't load (%u bytes)",
				(unsigned)music->size);
		return -1;
	}

	return 0;
}

/* Options > Music Synth (ps3_sf2.c): the synth the music goes through.
   Before the audio is open there's no thread to race with. */
void
PS3_Mixer_SetSoundFont(void *sf)
{
	if (!audio_open)
	{
		PS3MIDI_SetSoundFont(sf);
		return;
	}

	MIX_LOCK();
	PS3MIDI_SetSoundFont(sf);
	MIX_UNLOCK();
}

void
Mix_HaltMusic(void)
{
	MIX_LOCK();
	PS3MIDI_Stop();
	music_current = NULL;
	music_fade_total = 0;
	music_paused = 0;
	MIX_UNLOCK();
}

void
Mix_PauseMusic(void)
{
	MIX_LOCK();

	if (music_current)
	{
		music_paused = 1;
		PS3MIDI_Pause(1);
	}

	MIX_UNLOCK();
}

void
Mix_ResumeMusic(void)
{
	MIX_LOCK();

	if (music_current)
	{
		music_paused = 0;
		PS3MIDI_Pause(0);
	}

	MIX_UNLOCK();
}

int
Mix_PausedMusic(void)
{
	return music_current && music_paused;
}

int
Mix_PlayingMusic(void)
{
	int r;

	MIX_LOCK();
	r = (music_current != NULL) || (music_hook != NULL);
	MIX_UNLOCK();

	return r;
}

int
Mix_VolumeMusic(int volume)
{
	int prev = music_volume;

	if (volume >= 0)
	{
		music_volume = volume > MIX_MAX_VOLUME ? MIX_MAX_VOLUME : volume;

		MIX_LOCK();
		PS3MIDI_SetVolume(music_volume * 2);
		MIX_UNLOCK();
	}

	return prev;
}

int
Mix_FadeOutMusic(int ms)
{
	MIX_LOCK();

	if (!music_current || ms <= 0)
	{
		if (music_current)
		{
			PS3MIDI_Stop();
			music_current = NULL;
		}

		MIX_UNLOCK();
		return 0;
	}

	music_fade_total = ms * (AUDIO_RATE / 1000);
	music_fade_left = music_fade_total;

	MIX_UNLOCK();

	return 1;
}

Mix_Fading
Mix_FadingMusic(void)
{
	return (music_current && music_fade_total > 0) ? MIX_FADING_OUT : MIX_NO_FADING;
}

void
Mix_HookMusic(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg)
{
	MIX_LOCK();
	music_hook = mix_func;
	music_hook_arg = arg;
	MIX_UNLOCK();
}

const char *
Mix_GetSoundFonts(void)
{
	return NULL;
}

int
Mix_SetSoundFonts(const char *paths)
{
	(void)paths;
	return 0;
}
