/*
 * Copyright (C) 2026 the ROTT-PS3 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * DualShock 3 through ioPad (see ps3_pad.c).
 */

#ifndef PS3_PAD_H
#define PS3_PAD_H

/* paddata.button[2..3] as one 16 bit mask */
#define PS3_PAD_LEFT     0x8000
#define PS3_PAD_DOWN     0x4000
#define PS3_PAD_RIGHT    0x2000
#define PS3_PAD_UP       0x1000
#define PS3_PAD_START    0x0800
#define PS3_PAD_R3       0x0400
#define PS3_PAD_L3       0x0200
#define PS3_PAD_SELECT   0x0100
#define PS3_PAD_SQUARE   0x0080
#define PS3_PAD_CROSS    0x0040
#define PS3_PAD_CIRCLE   0x0020
#define PS3_PAD_TRIANGLE 0x0010
#define PS3_PAD_R1       0x0008
#define PS3_PAD_L1       0x0004
#define PS3_PAD_R2       0x0002
#define PS3_PAD_L2       0x0001

typedef struct
{
	int connected;
	unsigned buttons;
	int lx, ly, rx, ry;   /* 0..255, 128 = center, y grows downwards */
} ps3_padstate_t;

int  PS3_Pad_Init(void);
void PS3_Pad_Shutdown(void);
void PS3_Pad_Poll(ps3_padstate_t *out);

#endif
