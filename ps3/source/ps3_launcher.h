/*
 * Copyright (C) 2026 the ROTT-PS3 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef PS3_LAUNCHER_H
#define PS3_LAUNCHER_H

#define PS3_ENGINE_REGISTERED 0
#define PS3_ENGINE_SHAREWARE  1

typedef struct
{
	int engine;              /* PS3_ENGINE_* */
	char label[48];
	int argc;                /* extra command line for the engine */
	const char *argv[16];
} ps3_launch_t;

/* Scans USRDIR (and USRDIR/mods), shows the menu if there is more than
   one choice. 0 = something to play in *out, -1 = no game data, -2 = the
   XMB asked to quit. Needs the video and the pad up. */
int PS3_Launcher_Run(ps3_launch_t *out);

/* After -1: what the game data found lacks ("" if there was none). */
const char *PS3_Launcher_Missing(void);

#endif
