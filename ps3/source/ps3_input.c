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
 * The DualShock 3, two ways (see ps3_platform.h):
 *
 *  - Game: rt_playr.c calls PS3_Input_Game() every tic and turns buttons
 *    and sticks into ROTT's actions and movement itself. While that
 *    happens the pad types nothing, except START (Escape, the menu).
 *
 *  - Everything else (menus, automap, "press a key" screens, the quit
 *    screen): the pad types keys through the SDL event queue.
 *      d-pad / left stick  arrows       X          Enter
 *      O, START            Escape       triangle,  Tab (leaves the map)
 *                                       SELECT
 *      square              Space        L1 / R1    Page Up / Page Down
 *                                                  (automap zoom)
 *    The menus repeat held arrows by themselves, so there is no key
 *    repeat here: one key down on press, one key up on release.
 *
 * Switching between the two, whatever is held stays masked until it is
 * released: the X that picks "New Game" must not fire the first shot,
 * and the button that opens the menu must not act in it (handoff notes
 * 3.35 and 3.42).
 *
 * =======================================================================
 */

#include <string.h>

#include "SDL.h"
#include "ps3_platform.h"
#include "ps3_pad.h"

void PS3_Pad_Poll(ps3_padstate_t *out);

/* The game counts as "reading the pad" for this long after its last call
   (it polls at 35 Hz; menus and screens don't call at all). */
#define GAME_TIMEOUT_MS 150

/* Left stick as arrows in menus: past this much, with a little hysteresis */
#define STICK_KEY_ON   80
#define STICK_KEY_OFF  56

static ps3_padstate_t pad;
static Uint32 last_poll;
static int polled_once;

static int game_mode;
static Uint32 last_game_call;
static unsigned held_mask;          /* ignored until released */
static unsigned prev_game_buttons;

static unsigned keys_down;          /* menu mode: virtual buttons down */
static int game_escape_down;        /* game mode: START typed Escape */
static int pumped_once;
static int deadzone = 24;           /* of 128 */

/* Menu mode: which key each virtual button types. Bits 0..15 are the
   pad's own; 16..19 the left stick's four directions. */
#define VB_STICK_UP    (1u << 16)
#define VB_STICK_DOWN  (1u << 17)
#define VB_STICK_LEFT  (1u << 18)
#define VB_STICK_RIGHT (1u << 19)

static struct
{
	unsigned bit;
	SDL_Scancode scancode;
	SDL_Keycode sym;
} menu_keys[] = {
	{PS3_PAD_UP, SDL_SCANCODE_UP, SDLK_UP},
	{PS3_PAD_DOWN, SDL_SCANCODE_DOWN, SDLK_DOWN},
	{PS3_PAD_LEFT, SDL_SCANCODE_LEFT, SDLK_LEFT},
	{PS3_PAD_RIGHT, SDL_SCANCODE_RIGHT, SDLK_RIGHT},
	{VB_STICK_UP, SDL_SCANCODE_UP, SDLK_UP},
	{VB_STICK_DOWN, SDL_SCANCODE_DOWN, SDLK_DOWN},
	{VB_STICK_LEFT, SDL_SCANCODE_LEFT, SDLK_LEFT},
	{VB_STICK_RIGHT, SDL_SCANCODE_RIGHT, SDLK_RIGHT},
	{PS3_PAD_CROSS, SDL_SCANCODE_RETURN, SDLK_RETURN},
	{PS3_PAD_CIRCLE, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE},
	{PS3_PAD_START, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE},
	{PS3_PAD_TRIANGLE, SDL_SCANCODE_TAB, SDLK_TAB},
	{PS3_PAD_SELECT, SDL_SCANCODE_TAB, SDLK_TAB}, /* the map button: MAP_KEY_SLOT */
	{PS3_PAD_SQUARE, SDL_SCANCODE_SPACE, SDLK_SPACE},
	{PS3_PAD_L1, SDL_SCANCODE_PAGEUP, SDLK_PAGEUP},
	{PS3_PAD_R1, SDL_SCANCODE_PAGEDOWN, SDLK_PAGEDOWN},
};

#define NUM_MENU_KEYS ((int)(sizeof(menu_keys) / sizeof(menu_keys[0])))
#define MAP_KEY_SLOT  12

static void PollPad(void);

static int capturing;               /* rebinding: the pad types nothing */
static unsigned capture_prev;

/* The button that opens the map (Options > Controller) also closes it:
   it types Tab in the menus, unless it already types something there. */
void
PS3_Input_SetMapButton(unsigned bit)
{
	int i;

	menu_keys[MAP_KEY_SLOT].bit = 0;

	for (i = 0; i < NUM_MENU_KEYS; i++)
	{
		if (i != MAP_KEY_SLOT && menu_keys[i].bit == bit)
		{
			return;
		}
	}

	menu_keys[MAP_KEY_SLOT].bit = bit;
}

/* Rebinding a button: while on, nothing is typed, and
   PS3_Input_CapturePressed returns the buttons that went down. When it
   ends, whatever is held stays masked until released. */
void
PS3_Input_Capture(int on)
{
	on = on ? 1 : 0;

	if (on == capturing)
	{
		return;
	}

	capturing = on;

	if (on)
	{
		int i;

		for (i = 0; i < NUM_MENU_KEYS; i++)
		{
			if (keys_down & menu_keys[i].bit)
			{
				SDL_Event ev;

				memset(&ev, 0, sizeof(ev));
				ev.type = SDL_KEYUP;
				ev.key.state = SDL_RELEASED;
				ev.key.keysym.scancode = menu_keys[i].scancode;
				ev.key.keysym.sym = menu_keys[i].sym;
				SDL_PushEvent(&ev);
			}
		}

		keys_down = 0;
		capture_prev = pad.buttons & 0xffffu;   /* held now: not a press */
	}
	else
	{
		held_mask = pad.buttons & 0xffffu;
	}
}

unsigned
PS3_Input_CapturePressed(void)
{
	unsigned now, pressed;

	PollPad();
	now = pad.buttons & 0xffffu;
	pressed = now & ~capture_prev;
	capture_prev = now;

	return pressed;
}

void
PS3_Input_SetDeadzone(int percent)
{
	if (percent < 0)
	{
		percent = 0;
	}

	if (percent > 60)
	{
		percent = 60;
	}

	deadzone = percent * 128 / 100;
}

static void
PollPad(void)
{
	Uint32 now = SDL_GetTicks();

	/* the menus poll in tight loops: once every 4 ms is plenty */
	if (polled_once && now - last_poll < 4)
	{
		return;
	}

	last_poll = now;
	polled_once = 1;
	PS3_Pad_Poll(&pad);

	if (!pad.connected)
	{
		pad.buttons = 0;
		pad.lx = pad.ly = pad.rx = pad.ry = 128;
	}
}

static void
SendKey(SDL_Scancode sc, SDL_Keycode sym, int down)
{
	SDL_Event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	ev.key.keysym.scancode = sc;
	ev.key.keysym.sym = sym;
	SDL_PushEvent(&ev);
}

/* Virtual buttons for menu mode: the pad's, plus the left stick as four
   more, with hysteresis. */
static unsigned
MenuButtons(void)
{
	static unsigned stick;
	unsigned b = pad.buttons & 0xffffu;
	int dx = pad.lx - 128, dy = pad.ly - 128;

	if (dy < -STICK_KEY_ON) stick |= VB_STICK_UP;
	else if (dy > -STICK_KEY_OFF) stick &= ~VB_STICK_UP;
	if (dy > STICK_KEY_ON) stick |= VB_STICK_DOWN;
	else if (dy < STICK_KEY_OFF) stick &= ~VB_STICK_DOWN;
	if (dx < -STICK_KEY_ON) stick |= VB_STICK_LEFT;
	else if (dx > -STICK_KEY_OFF) stick &= ~VB_STICK_LEFT;
	if (dx > STICK_KEY_ON) stick |= VB_STICK_RIGHT;
	else if (dx < STICK_KEY_OFF) stick &= ~VB_STICK_RIGHT;

	return b | stick;
}

/* Releases every key menu mode is holding down. */
static void
ReleaseMenuKeys(void)
{
	int i;

	for (i = 0; i < NUM_MENU_KEYS; i++)
	{
		if (keys_down & menu_keys[i].bit)
		{
			SendKey(menu_keys[i].scancode, menu_keys[i].sym, 0);
		}
	}

	keys_down = 0;
}

void
PS3_Input_Pump(void)
{
	unsigned now_buttons, changed;
	int i;

	PollPad();

	if (capturing)
	{
		return;
	}

	if (game_mode && SDL_GetTicks() - last_game_call > GAME_TIMEOUT_MS)
	{
		/* the game stopped reading the pad: a menu or a screen is up */
		game_mode = 0;
		held_mask = MenuButtons();
		keys_down = 0;

		if (game_escape_down)
		{
			SendKey(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 0);
			game_escape_down = 0;
		}

		PS3_Log("[input] menu mode");
	}

	if (game_mode)
	{
		/* START opens the menu from the game */
		int start = (pad.buttons & PS3_PAD_START & ~held_mask) != 0;

		if (start != game_escape_down)
		{
			SendKey(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, start);
			game_escape_down = start;
		}

		return;
	}

	now_buttons = MenuButtons();

	if (!pumped_once)
	{
		/* what's held when the game starts (the X that picked it in the
		   launcher) is not a key press */
		pumped_once = 1;
		held_mask = now_buttons;
	}

	held_mask &= now_buttons;          /* released: no longer masked */
	now_buttons &= ~held_mask;
	changed = now_buttons ^ keys_down;

	if (!changed)
	{
		return;
	}

	for (i = 0; i < NUM_MENU_KEYS; i++)
	{
		unsigned bit = menu_keys[i].bit;

		if (!(changed & bit))
		{
			continue;
		}

		/* two buttons typing the same key (d-pad and stick): only the
		   first press and the last release count */
		if (now_buttons & bit)
		{
			int j, already = 0;

			for (j = 0; j < NUM_MENU_KEYS; j++)
			{
				if (j != i && menu_keys[j].scancode == menu_keys[i].scancode &&
						(keys_down & menu_keys[j].bit))
				{
					already = 1;
				}
			}

			keys_down |= bit;

			if (!already)
			{
				SendKey(menu_keys[i].scancode, menu_keys[i].sym, 1);
			}
		}
		else
		{
			int j, still = 0;

			keys_down &= ~bit;

			for (j = 0; j < NUM_MENU_KEYS; j++)
			{
				if (menu_keys[j].scancode == menu_keys[i].scancode &&
						(keys_down & menu_keys[j].bit))
				{
					still = 1;
				}
			}

			if (!still)
			{
				SendKey(menu_keys[i].scancode, menu_keys[i].sym, 0);
			}
		}
	}
}

/* Stick axis, 0..255 with 128 at rest, to -32767..32767 past the dead
   zone (rescaled so the useful range starts at 0). */
static int
Axis(int raw)
{
	int v = raw - 128;
	int sign = v < 0 ? -1 : 1;
	int mag = v * sign;

	if (mag <= deadzone)
	{
		return 0;
	}

	mag = (mag - deadzone) * 32767 / (127 - deadzone);

	if (mag > 32767)
	{
		mag = 32767;
	}

	return sign * mag;
}

void
PS3_Input_Game(ps3_gamepad_t *out)
{
	unsigned b;

	PollPad();

	if (!game_mode)
	{
		/* coming from a menu: what's held stays out until released */
		ReleaseMenuKeys();
		game_mode = 1;
		game_escape_down = 0;
		held_mask = pad.buttons & 0xffffu;
		prev_game_buttons = 0;
		PS3_Log("[input] game mode");
	}

	last_game_call = SDL_GetTicks();

	held_mask &= pad.buttons;
	b = pad.buttons & 0xffffu & ~held_mask;

	out->connected = pad.connected;
	out->buttons = b;
	out->pressed = b & ~prev_game_buttons;
	out->lx = Axis(pad.lx);
	out->ly = Axis(pad.ly);
	out->rx = Axis(pad.rx);
	out->ry = Axis(pad.ry);

	prev_game_buttons = b;
}
