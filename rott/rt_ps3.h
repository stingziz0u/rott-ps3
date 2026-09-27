/*
Copyright (C) 2026 the ROTT-PS3 contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/

//***************************************************************************
//
// PS3 only: the DualShock 3's settings (rt_playr.c uses them, rt_menu.c
// edits them, rt_cfg.c keeps them in CONFIG.ROT) and the video settings.
//
//***************************************************************************

#ifndef _rt_ps3_public
#define _rt_ps3_public

#ifdef __PS3__

// Video Settings > WIDESCREEN: 16:9 at 480 lines. 848 = 53 * 16 (view
// widths must divide by 16); 848:480 is 16:9 within 0.6%.
#define PS3_WIDE_W 848
#define PS3_WIDE_H 480
// how far the 640x480 layouts (HUD, 4:3 screens) move right to be centered
#define PS3_WIDE_OFS ((iGLOBAL_SCREENWIDTH - 640) / 2)

// What a button can do. The sticks, the d-pad (move and turn) and START
// (menu) are fixed; these are assigned in Options > Controller.
enum
{
	ps3a_fire,
	ps3a_run,
	ps3a_open,
	ps3a_voltface,
	ps3a_toggleweapon,
	ps3a_nextweapon,
	ps3a_dropweapon,
	ps3a_autorun,
	ps3a_centerview,
	ps3a_map,
	ps3a_strafe,
	ps3a_flyup,	  // Mercury Mode (look up/down otherwise); the
	ps3a_flydown, // right stick flies too, while it lasts
	PS3_NUMACTIONS
};

// The buttons that can be assigned, in the order the menu offers them.
#define PS3_NUMBINDABLE 11

extern const unsigned ps3_bindable[PS3_NUMBINDABLE];
extern const char *const ps3_bindable_name[PS3_NUMBINDABLE];

extern int ps3_binding[PS3_NUMACTIONS];		// PS3_PAD_* bit, 0 = none
extern const int ps3_default_binding[PS3_NUMACTIONS];
extern const char *const ps3_action_name[PS3_NUMACTIONS];
extern const char *const ps3_action_cfgname[PS3_NUMACTIONS];

extern int ps3_turnspeed;	// 1..20, 10 = the d-pad's speed
extern int ps3_aimspeed;	// 1..20
extern int ps3_deadzone;	// 5..40, percent of the stick's travel
extern int ps3_invertaim;	// 0/1
extern int ps3_swapsticks;	// 0/1: aim/turn on the left stick

extern int ps3_screenfit;	// 70..100, percent of the TV
extern int ps3_filter;		// 1 = smooth, 0 = sharp
extern int ps3_showfps;		// 0/1
extern int ps3_lowres;		// 320x200 from the next start
extern int ps3_widescreen;	// 848x480 from the next start (not with lowres)
extern int ps3_largehud;	// 0/1: status bars at 2x in 640x480
extern int ps3_musicsf2;	// 0 AdLib, 1 General MIDI through a .sf2

// A saved game that doesn't match the level (rt_door.c): leaves the load
// and goes back to the menu, when one is in progress (rt_menu.c DoLoad)
void PS3_LoadMismatch(const char *what, int level, int saved);

int PS3_LargeHudTop(void);	// top bar drawn at 2x this frame
int PS3_LargeHudBottom(void);	// bottom bar drawn at 2x this frame
int PS3_LargeHudStats(void);	// see-through health/ammo drawn at 2x

const char *PS3_ButtonName(unsigned bit);
void PS3_ApplyControllerSettings(void);
void PS3_ApplyVideoSettings(void);

#endif

#endif
