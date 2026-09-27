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
 * The launcher: what to play, before the engine starts (the pattern of
 * CrispyCell's launcher, handoff notes 4.5).
 *
 *  GAMES   Rise of the Triad: Dark War   DARKWAR.WAD (registered, GOG,
 *                                        Steam, CD). Its episodes --
 *                                        Dark War, Extreme ROTT, The HUNT
 *                                        Begins -- are picked in the
 *                                        game's own New Game menu.
 *          The HUNT Begins (shareware)   HUNTBGIN.WAD
 *  MODS    (off in 1.0: PS3_LAUNCHER_MODS, ps3_platform.h)
 *          one entry per folder in USRDIR/mods/, with the folder's .RTL
 *          (levels), .RTC (Comm-bat levels) and up to three .WAD files
 *          on top of Dark War. An optional mod.txt with "name = ..."
 *          gives the entry a nicer name.
 *
 * With a single entry there is no menu. L1/R1 switch tabs, X starts.
 * File names are matched without case (FTP clients change it).
 *
 * =======================================================================
 */

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ps3_platform.h"
#include "ps3_launcher.h"
#include "ps3_pad.h"
#include "ps3_video.h"
#include "ps3_font.h"

void PS3_Pad_Poll(ps3_padstate_t *out);

#define LAUNCHER_W 640
#define LAUNCHER_H 480

#define COL_BG      0xff100c0cu
#define COL_TEXT    0xffc8c0b8u
#define COL_DIM     0xff787068u
#define COL_SEL_BG  0xff902018u
#define COL_SEL_TX  0xffffffffu
#define COL_TITLE   0xffe0a040u
#define COL_TAB_ON  0xffe0a040u
#define COL_TAB_OFF 0xff585048u

#define MAX_ENTRIES   64
#define VISIBLE_ROWS  8
#define LABEL_CHARS   34     /* at scale 2, what fits in the row */

typedef struct
{
	char label[48];
	int engine;                       /* PS3_ENGINE_* */
	int is_mod;
	char rtl[256];
	char rtc[256];
	char wads[3][256];
	int num_wads;
} entry_t;

static entry_t entries[MAX_ENTRIES];
static int num_entries;
static int has_registered;

/* What a game found in USRDIR is missing, for the "no data" screen. */
static char missing[256];

static void
Missing(const char *game, const char *files)
{
	PS3_Log("[launcher] %s: missing %s, not listed", game, files);

	if (!missing[0])
	{
		snprintf(missing, sizeof(missing), "%s: missing %s", game, files);
	}
}

const char *
PS3_Launcher_Missing(void)
{
	return missing;
}

/* ================================================================ */
/* Files                                                              */
/* ================================================================ */

static int
EqualNoCase(const char *a, const char *b)
{
	while (*a && *b)
	{
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
		{
			return 0;
		}

		a++;
		b++;
	}

	return *a == *b;
}

static int
HasExtension(const char *name, const char *ext)
{
	size_t n = strlen(name), e = strlen(ext);

	return n > e && EqualNoCase(name + n - e, ext);
}

/* Looks for 'name' in 'dir' without case. Fills 'out' with the full path
   as it is on disk. */
static int
FindFile(const char *dir, const char *name, char *out, size_t size)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int found = 0;

	if (!d)
	{
		return 0;
	}

	while ((e = readdir(d)) != NULL)
	{
		if (EqualNoCase(e->d_name, name))
		{
			snprintf(out, size, "%s/%s", dir, e->d_name);
			found = 1;
			break;
		}
	}

	closedir(d);

	return found;
}

static int
IsDir(const char *path)
{
	/* d_type isn't reliable on the PS3: opendir is the test (notes 4.5) */
	DIR *d = opendir(path);

	if (d)
	{
		closedir(d);
		return 1;
	}

	return 0;
}

/* A WAD's header: "IWAD"/"PWAD", number of lumps (little-endian). */
static int
CheckWad(const char *path)
{
	unsigned char h[12];
	FILE *f = fopen(path, "rb");
	int ok = 0;

	if (!f)
	{
		return 0;
	}

	if (fread(h, 1, 12, f) == 12 && (!memcmp(h, "IWAD", 4) || !memcmp(h, "PWAD", 4)))
	{
		unsigned lumps = h[4] | (h[5] << 8) | ((unsigned)h[6] << 16) | ((unsigned)h[7] << 24);

		PS3_Log("[launcher] %s: %.4s, %u lumps", path, (const char *)h, lumps);
		ok = lumps > 0;
	}
	else
	{
		PS3_Log("[launcher] %s: not a WAD", path);
	}

	fclose(f);

	return ok;
}

static void
Trim(char *s)
{
	size_t n;
	char *p = s;

	while (*p == ' ' || *p == '\t')
	{
		p++;
	}

	memmove(s, p, strlen(p) + 1);
	n = strlen(s);

	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
	{
		s[--n] = '\0';
	}
}

/* ================================================================ */
/* Scanning                                                           */
/* ================================================================ */

static void
AddBaseGames(void)
{
	char wad[256], rtl[256], rtc[256], rts[256];

	if (FindFile(PS3_USRDIR, "darkwar.wad", wad, sizeof(wad)) && CheckWad(wad))
	{
		if (!FindFile(PS3_USRDIR, "darkwar.rtl", rtl, sizeof(rtl)))
		{
			Missing("DARKWAR.WAD", "DARKWAR.RTL");
		}
		else if (!FindFile(PS3_USRDIR, "darkwar.rtc", rtc, sizeof(rtc)) &&
				!FindFile(PS3_USRDIR, "rottcd.rtc", rtc, sizeof(rtc)) &&
				!FindFile(PS3_USRDIR, "rottsite.rtc", rtc, sizeof(rtc)))
		{
			Missing("DARKWAR.WAD", "DARKWAR.RTC");
		}
		else if (!FindFile(PS3_USRDIR, "remote1.rts", rts, sizeof(rts)))
		{
			Missing("DARKWAR.WAD", "REMOTE1.RTS");
		}
		else
		{
			entry_t *e = &entries[num_entries++];

			memset(e, 0, sizeof(*e));
			snprintf(e->label, sizeof(e->label), "Rise of the Triad: Dark War");
			e->engine = PS3_ENGINE_REGISTERED;
			has_registered = 1;
		}
	}

	if (FindFile(PS3_USRDIR, "huntbgin.wad", wad, sizeof(wad)) && CheckWad(wad))
	{
		if (!FindFile(PS3_USRDIR, "huntbgin.rtl", rtl, sizeof(rtl)))
		{
			Missing("HUNTBGIN.WAD", "HUNTBGIN.RTL");
		}
		else if (!FindFile(PS3_USRDIR, "huntbgin.rtc", rtc, sizeof(rtc)))
		{
			Missing("HUNTBGIN.WAD", "HUNTBGIN.RTC");
		}
		else if (!FindFile(PS3_USRDIR, "remote1.rts", rts, sizeof(rts)))
		{
			Missing("HUNTBGIN.WAD", "REMOTE1.RTS");
		}
		else
		{
			entry_t *e = &entries[num_entries++];

			memset(e, 0, sizeof(*e));
			snprintf(e->label, sizeof(e->label), "The HUNT Begins (shareware)");
			e->engine = PS3_ENGINE_SHAREWARE;
		}
	}
}

static void
ReadModTxt(const char *dir, char *label, size_t size)
{
	char path[256], line[256];
	FILE *f;

	if (!FindFile(dir, "mod.txt", path, sizeof(path)))
	{
		return;
	}

	f = fopen(path, "r");

	if (!f)
	{
		return;
	}

	while (fgets(line, sizeof(line), f))
	{
		char *eq = strchr(line, '=');

		if (!eq)
		{
			continue;
		}

		*eq = '\0';
		Trim(line);
		Trim(eq + 1);

		if (EqualNoCase(line, "name") && eq[1])
		{
			snprintf(label, size, "%s", eq + 1);
		}
	}

	fclose(f);
}

static int
CompareNames(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

static void
AddMod(const char *folder, const char *path)
{
	char names[64][64];
	int n = 0, i;
	DIR *d;
	struct dirent *de;
	entry_t *e;

	if (num_entries >= MAX_ENTRIES)
	{
		return;
	}

	d = opendir(path);

	if (!d)
	{
		return;
	}

	while ((de = readdir(d)) != NULL && n < 64)
	{
		if (de->d_name[0] == '.')
		{
			continue;
		}

		snprintf(names[n++], sizeof(names[0]), "%s", de->d_name);
	}

	closedir(d);

	/* alphabetical, so the order of the .WAD files is predictable */
	qsort(names, (size_t)n, sizeof(names[0]), CompareNames);

	e = &entries[num_entries];
	memset(e, 0, sizeof(*e));
	e->engine = PS3_ENGINE_REGISTERED;
	e->is_mod = 1;

	for (i = 0; i < n; i++)
	{
		const char *nm = names[i];

		if ((HasExtension(nm, ".rtl") || HasExtension(nm, ".rxl")) && !e->rtl[0])
		{
			snprintf(e->rtl, sizeof(e->rtl), "%s/%s", path, nm);
		}
		else if ((HasExtension(nm, ".rtc") || HasExtension(nm, ".rxc")) && !e->rtc[0])
		{
			snprintf(e->rtc, sizeof(e->rtc), "%s/%s", path, nm);
		}
		else if (HasExtension(nm, ".wad") && e->num_wads < 3)
		{
			snprintf(e->wads[e->num_wads++], sizeof(e->wads[0]), "%s/%s", path, nm);
		}
	}

	if (!e->rtl[0] && !e->rtc[0] && e->num_wads == 0)
	{
		PS3_Log("[launcher] mods/%s: no .RTL, .RTC or .WAD, skipped", folder);
		return;
	}

	snprintf(e->label, sizeof(e->label), "%s", folder);
	ReadModTxt(path, e->label, sizeof(e->label));

	PS3_Log("[launcher] mod '%s': rtl=%s rtc=%s wads=%d", e->label,
			e->rtl[0] ? e->rtl : "-", e->rtc[0] ? e->rtc : "-", e->num_wads);

	num_entries++;
}

static void
AddMods(void)
{
	char folders[MAX_ENTRIES][64];
	int n = 0, i;
	DIR *d = opendir(PS3_MODSDIR);
	struct dirent *de;

	if (!d)
	{
		return;
	}

	while ((de = readdir(d)) != NULL && n < MAX_ENTRIES)
	{
		char path[256];

		if (de->d_name[0] == '.')
		{
			continue;
		}

		snprintf(path, sizeof(path), "%s/%s", PS3_MODSDIR, de->d_name);

		if (IsDir(path))
		{
			snprintf(folders[n++], sizeof(folders[0]), "%s", de->d_name);
		}
	}

	closedir(d);

	if (n == 0)
	{
		return;
	}

	if (!has_registered)
	{
		PS3_Log("[launcher] %d mod folder(s), but mods need DARKWAR.WAD: none listed", n);
		return;
	}

	qsort(folders, (size_t)n, sizeof(folders[0]), CompareNames);

	for (i = 0; i < n; i++)
	{
		char path[256];

		snprintf(path, sizeof(path), "%s/%s", PS3_MODSDIR, folders[i]);
		AddMod(folders[i], path);
	}
}

/* ================================================================ */
/* Drawing                                                            */
/* ================================================================ */

static uint32_t *fb;

static void
FillRect(int x, int y, int w, int h, uint32_t c)
{
	int i, j;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > LAUNCHER_W) w = LAUNCHER_W - x;
	if (y + h > LAUNCHER_H) h = LAUNCHER_H - y;

	for (j = 0; j < h; j++)
	{
		uint32_t *row = fb + (y + j) * LAUNCHER_W + x;

		for (i = 0; i < w; i++)
		{
			row[i] = c;
		}
	}
}

static int
TextWidth(const char *s, int scale)
{
	return (int)strlen(s) * 8 * scale;
}

static void
DrawText(int x, int y, const char *s, uint32_t c, int scale)
{
	for (; *s; s++, x += 8 * scale)
	{
		unsigned ch = (unsigned char)*s;
		int gy, gx;

		if (ch < 32 || ch > 126)
		{
			ch = '?';
		}

		for (gy = 0; gy < 16; gy++)
		{
			unsigned bits = ps3_font8x16[ch - 32][gy];

			for (gx = 0; gx < 8; gx++)
			{
				if (bits & (0x80u >> gx))
				{
					FillRect(x + gx * scale, y + gy * scale, scale, scale, c);
				}
			}
		}
	}
}

/* Present at the end: the launcher's image goes to the TV like the
   game's (4:3, screen fit). */
static void
Present(void)
{
	uint32_t *dst;

	if (PS3_Video_SetSource(LAUNCHER_W, LAUNCHER_H) != 0)
	{
		return;
	}

	dst = PS3_Video_Source();

	if (dst)
	{
		memcpy(dst, fb, LAUNCHER_W * LAUNCHER_H * 4);
		PS3_Video_Present();
	}
}

static void
DrawMenu(int tab, const int *list, int count, int selected, int top, int has_mods)
{
	static const char *title = "RISE OF THE TRIAD";
	int row_h = 38;
	int i;

	FillRect(0, 0, LAUNCHER_W, LAUNCHER_H, COL_BG);
	DrawText((LAUNCHER_W - TextWidth(title, 3)) / 2, 30, title, COL_TITLE, 3);

	if (has_mods)
	{
		DrawText(200, 92, "GAMES", tab == 0 ? COL_TAB_ON : COL_TAB_OFF, 2);
		DrawText(360, 92, "MODS", tab == 1 ? COL_TAB_ON : COL_TAB_OFF, 2);
		FillRect(tab == 0 ? 200 : 360, 126, tab == 0 ? 80 : 64, 3, COL_TAB_ON);
	}

	for (i = 0; i < VISIBLE_ROWS && top + i < count; i++)
	{
		int idx = top + i;
		int y = 150 + i * row_h;
		uint32_t colour = COL_TEXT;

		char label[LABEL_CHARS + 1];

		if (idx == selected)
		{
			FillRect(26, y - 3, LAUNCHER_W - 52, row_h - 4, COL_SEL_BG);
			colour = COL_SEL_TX;
		}

		snprintf(label, sizeof(label), "%s", entries[list[idx]].label);
		DrawText(44, y, label, colour, 2);
	}

	if (count > VISIBLE_ROWS)
	{
		char counter[24];

		snprintf(counter, sizeof(counter), "%d / %d", selected + 1, count);
		DrawText(LAUNCHER_W - TextWidth(counter, 1) - 24, LAUNCHER_H - 30, counter, COL_DIM, 1);
	}

	DrawText(24, LAUNCHER_H - 30,
			has_mods ? "UP/DOWN: CHOOSE   L1/R1: TAB   X: PLAY" : "UP/DOWN: CHOOSE   X: PLAY",
			COL_DIM, 1);

	Present();
}

/* ================================================================ */
/* Menu                                                               */
/* ================================================================ */

static int
RunMenu(void)
{
	int lists[2][MAX_ENTRIES];
	int counts[2] = {0, 0};
	int tab = 0, selected[2] = {0, 0}, top[2] = {0, 0};
	unsigned last = 0xffffu;   /* what's held at start doesn't count */
	int i;

	for (i = 0; i < num_entries; i++)
	{
		int t = entries[i].is_mod ? 1 : 0;

		lists[t][counts[t]++] = i;
	}

	if (counts[0] == 0)
	{
		tab = 1;
	}

	for (;;)
	{
		ps3_padstate_t pad;
		unsigned now, pressed;
		int n;

		PS3_Pad_Poll(&pad);
		now = pad.connected ? pad.buttons : 0;
		pressed = now & ~last;
		last = now;

		if (PS3_ExitRequested())
		{
			return -1;
		}

		if ((pressed & (PS3_PAD_L1 | PS3_PAD_R1 | PS3_PAD_LEFT | PS3_PAD_RIGHT)) &&
				counts[0] > 0 && counts[1] > 0)
		{
			tab ^= 1;
		}

		n = counts[tab];

		if (pressed & PS3_PAD_UP)
		{
			selected[tab] = (selected[tab] + n - 1) % n;
		}

		if (pressed & PS3_PAD_DOWN)
		{
			selected[tab] = (selected[tab] + 1) % n;
		}

		if (pressed & (PS3_PAD_CROSS | PS3_PAD_START))
		{
			return lists[tab][selected[tab]];
		}

		if (selected[tab] < top[tab])
		{
			top[tab] = selected[tab];
		}
		else if (selected[tab] >= top[tab] + VISIBLE_ROWS)
		{
			top[tab] = selected[tab] - VISIBLE_ROWS + 1;
		}

		DrawMenu(tab, lists[tab], n, selected[tab], top[tab], counts[1] > 0);
		PS3_Pump();
	}
}

/* ================================================================ */
/* Entry point                                                        */
/* ================================================================ */

int
PS3_Launcher_Run(ps3_launch_t *out)
{
	int chosen;
	const entry_t *e;

	memset(out, 0, sizeof(*out));
	missing[0] = '\0';
	num_entries = 0;
	has_registered = 0;

	AddBaseGames();
#if PS3_LAUNCHER_MODS
	AddMods();
#endif

	PS3_Log("[launcher] %d entries", num_entries);

	if (num_entries == 0)
	{
		return -1;
	}

	if (num_entries == 1)
	{
		PS3_Log("[launcher] one entry, no menu");
		chosen = 0;
	}
	else
	{
		fb = malloc(LAUNCHER_W * LAUNCHER_H * 4);

		if (!fb)
		{
			PS3_Log("[launcher] out of memory, using the first entry");
			chosen = 0;
		}
		else
		{
			chosen = RunMenu();
			free(fb);
			fb = NULL;

			if (chosen < 0)
			{
				return -2;   /* quit from the XMB */
			}
		}
	}

	e = &entries[chosen];
	out->engine = e->engine;
	snprintf(out->label, sizeof(out->label), "%s", e->label);

	if (e->rtl[0])
	{
		out->argv[out->argc++] = "filertl";
		out->argv[out->argc++] = e->rtl;
	}

	if (e->rtc[0])
	{
		out->argv[out->argc++] = "filertc";
		out->argv[out->argc++] = e->rtc;
	}

	for (chosen = 0; chosen < e->num_wads; chosen++)
	{
		static const char *const file_parm[3] = {"file", "file1", "file2"};

		out->argv[out->argc++] = file_parm[chosen];
		out->argv[out->argc++] = e->wads[chosen];
	}

	PS3_Log("[launcher] starting '%s' (%s engine, %d extra args)", out->label,
			out->engine == PS3_ENGINE_SHAREWARE ? "shareware" : "registered",
			out->argc);

	return 0;
}
