/*
 * input_joy.cpp - Joystick / gamepad processing for the input subsystem.
 *
 * Contains analog dead-zone application, digital direction handling,
 * autofire toggle, per-player key-state tracking, and per-frame state updates.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <linux/input.h>
#include <stdint.h>

#include "input.h"
#include "input/input_internal.h"
#include "cfg.h"
#include "user_io.h"
#include "menu.h"
#include "video.h"
#include "autofire.h"
#include "joymapping.h"
#include "support.h"

/* ========================================================================
 *  Key-state tracking  (defined here, declared extern in input_internal.h)
 * ======================================================================== */

KeyStates key_states[NUMPLAYERS] = {};

/* ========================================================================
 *  Analog helpers
 * ======================================================================== */

static inline void joy_clamp(int* value, const int min, const int max)
{
	if (*value < min) {
		*value = min;
	}
	else if (*value > max) {
		*value = max;
	}
}

static inline float boxradf(const float angle)
{
	return 1.0f / fmaxf(fabsf(sinf(angle)), fabsf(cosf(angle)));
}

static void joy_apply_deadzone(int* x, int* y, const devInput* dev, const int stick) {
	// Don't be fancy with such a small deadzone.
	if (dev->deadzone <= 2) 
	{
		if (dev->deadzone && (abs((*x > *y) == (*x > -*y) ? *x : *y) <= dev->deadzone))
			*x = *y = 0;
		return;
	}

	const float radius = hypotf(*x, *y);
	if (radius <= (float)dev->deadzone)
	{
		*x = *y = 0;
		return;
	}

	const float angle = atan2f(*y, *x);
	const float box_radius = boxradf(angle);

	/* A measure of how "cardinal" the angle is,
	   i.e closeness to [0, 90, 180, 270] degrees (0.0 - 1.0). */
	const float cardinality = (1.4142136f - box_radius) * 2.4142136f;

	// Expected range for the given angle.
	const float max_cardinal = dev->max_cardinal[stick] > (2.0f * dev->deadzone) ? dev->max_cardinal[stick] : 127.0f;
	const float max_diagonal = dev->max_range[stick] > (2.0f * dev->deadzone) ? dev->max_range[stick] : 127.0f;
	const float range = cardinality * max_cardinal + (1.0f - cardinality) * max_diagonal;

	const float weight = 1.0f - fmaxf(range - radius, .0f) / (range - dev->deadzone);
	const float adjusted_radius = fminf(weight * range, max_cardinal * box_radius);

	/* Don't ever return a larger magnitude than that was given.
	   The whole point of this function is to subtract some magnitude, not add. */
	if (adjusted_radius > radius) return;

	*x = nearbyintf(adjusted_radius * cosf(angle));
	*y = nearbyintf(adjusted_radius * sinf(angle));

	// Just to be sure.
	const int min_range = is_psx() ? -128 : -127;
	joy_clamp(x, min_range, INT8_MAX);
	joy_clamp(y, min_range, INT8_MAX);
}

/* ========================================================================
 *  Autofire toggle handling
 * ======================================================================== */

static bool osd_autofire_consumed[NUMPLAYERS] = {};

// returns true if autofire was toggled which also means input was consumed.
static bool handle_autofire_toggle(int num, uint32_t mask, uint32_t code, char press, int bnum, int dont_save)
{
	static uint32_t lastcode[NUMPLAYERS];
	static uint32_t lastmask[NUMPLAYERS];
	static char str[512];

	// if the button is not OSD or BTN_TGL we save it into lastmask/lastcode
	if (bnum != BTN_OSD && bnum != BTN_TGL)
	{
		if (!dont_save)
		{
			if (press)
			{
				if (lastcode[num] == code) { // build up mask if multiple buttons map to same code
					lastmask[num] |= mask; 	 // this can't happen at present but if it ever does it will work correctly
											
				} else {
					lastcode[num] = code;
					lastmask[num] = mask;
				}
			}
			else
			{
				lastcode[num] = 0;
				lastmask[num] = 0;
			}
		}
		return false;
	}

	// we can only get here if the OSD or BTN_TGL keys were pressed
	// in that event we see if lastmask/lastcode tells us we're holding a button
	// and if we are, we toggle autofire for that button
	if (!user_io_osd_is_visible() && press && !cfg.disable_autofire)
	{
		if ((lastcode[num] && lastmask[num] && (lastmask[num] & 0xF) == 0)) // don't allow enabling autofire on directions
		{
			char *strat = str;
			inc_autofire_code(num, lastcode[num], lastmask[num]);
			
			// display autofire status for each button in the mask
			FOR_EACH_SET_BIT(lastmask[num], btn) {
				strat += sprintf(strat, "%s\n", joy_bnames[btn-4]);
			}

			const char *rate = get_autofire_rate_hz(num, lastcode[num]);

			if (!strcmp(rate, "disabled")) {
					strat += sprintf(strat, "Autofire disabled");
			} else {
					strat += sprintf(strat, "Autofire: %s", rate);
			}

			if (hasAPI1_5())
				Info(str);
			else
				InfoMessage(str);
			if (bnum == BTN_OSD && press) osd_autofire_consumed[num] = true;
			return true;
		}
	}
	return false;
}

/* ========================================================================
 *  Key-state helpers  (per-player tracking of which buttons are pressed)
 * ======================================================================== */

uint32_t osdbtn = 0;

// returns the bitmask representing all button states for a given ev->code (key)
// returns 0 if the key is not found or if key has no buttons pressed.
static uint32_t get_key_state(int player, uint32_t key)
{
	for (int i = 0; i < key_states[player].count; i++)
	{
		if (key_states[player].key[i] == key)
		{
			return key_states[player].mask[i];
		}
	}
	return 0u;
}

// updates the bitmask representing all button states for a given ev->code (key)
static void set_key_state(int player, uint32_t key, bool press, uint32_t mask)
{
	for (int i = 0; i < key_states[player].count; i++)
	{
		if (key_states[player].key[i] == key)
		{
			if (press)
			{
				key_states[player].mask[i] |= mask;
				return;
			}
			else
			{
				key_states[player].mask[i] &= ~mask;
				return;
			}
		}
	}
	if (key_states[player].count < MAX_KEY_STATES)
	{
		if (press)
		{
			int idx = key_states[player].count++;
			key_states[player].key[idx] = key;
			key_states[player].mask[idx] = mask;
		}
		return;
	}
}

uint32_t build_joy_mask(int player)
{
	uint32_t mask = 0u;
	for (int i = 0; i < key_states[player].count; i++)
	{
		uint32_t key = key_states[player].key[i];
		if (!is_autofire_enabled(player, key_states[player].key[i]))
			mask |= get_key_state(player, key);
	}
	return mask;
}

uint32_t build_autofire_mask(int player)
{
	uint32_t mask = 0u;
	for (int i = 0; i < key_states[player].count; i++)
	{
		uint32_t key = key_states[player].key[i];
		uint32_t  frames_held = key_states[player].frames_held[i];
		if (is_autofire_enabled(player, key) && get_autofire_bit(player, key, frames_held))
				mask |= get_key_state(player, key);
	}
	return mask;
}

/* ========================================================================
 *  Diagonal detection helper
 * ======================================================================== */

static bool joy_dir_is_diagonal(const int x, const int y)
{
	static const float JOY_DIAG_THRESHOLD = .85f;
	
	return
		((x == 0) || (y == 0)) ? false :
		((x == y) || (x == -y)) ? true :
		abs((x > y) == (x > -y) ? (float)y / x : (float)x / y) >= JOY_DIAG_THRESHOLD;
}

/* ========================================================================
 *  joy_digital - processes a digital button press/release for a player
 * ======================================================================== */

void joy_digital(int jnum, uint32_t mask, uint32_t code, char press, int bnum, int dont_save)
{
	int num = jnum - 1;
	if (num < NUMPLAYERS)
	{
		// autofire handler moved to helper function for clarity
		if (handle_autofire_toggle(num, mask, code, press, bnum, dont_save)) {
			return;
		}

		if (bnum == BTN_TGL)
		{
			if(press) kbd_toggle = !kbd_toggle;
			return;
		}

		if (!user_io_osd_is_visible() && (bnum == BTN_OSD) && (mouse_emu & 1))
		{
			if (press)
			{
				mouse_sniper = 0;
				mouse_timer = 0;
				mouse_btn = 0;
				mouse_emu_x = 0;
				mouse_emu_y = 0;
				mouse_cb();
				mouse_btn_req();

				mouse_emu ^= 2;
				if (hasAPI1_5()) Info((mouse_emu & 2) ? "Mouse mode ON" : "Mouse mode OFF");
				else InfoMessage((mouse_emu & 2) ? "\n\n       Mouse mode lock\n             ON" :
					"\n\n       Mouse mode lock\n             OFF");
			}
			return;
		}

		// toggling autofire via OSD button consumes the "press" event of OSD but not the release event
		// this suppresses that so toggling autofire on a button doesn't immediately disable the button
		if (bnum == BTN_OSD && !press && osd_autofire_consumed[num]) {
			osd_autofire_consumed[num] = false;
			return;
		}

		// clear OSD button state if not in the OSD.  this avoids problems where buttons are still held
		// on OSD exit and causes combinations to match when partial buttons are pressed.
		if (!user_io_osd_is_visible()) osdbtn = 0;

		if (user_io_osd_is_visible() || (bnum == BTN_OSD))
		{
			mask &= ~JOY_BTN3;
			if (press)
			{
				osdbtn |= mask;
				if (mask & (JOY_BTN1 | JOY_BTN2)) {
					if ((osdbtn & (JOY_BTN1 | JOY_BTN2)) == (JOY_BTN1 | JOY_BTN2))
					{
						osdbtn |= JOY_BTN3;
						mask = JOY_BTN3;
					}
				}
			}
			else
			{
				int old_osdbtn = osdbtn;
				osdbtn &= ~mask;

				if (mask & (JOY_BTN1 | JOY_BTN2)) {
					if ((old_osdbtn & (JOY_BTN1 | JOY_BTN2 | JOY_BTN3)) == (JOY_BTN1 | JOY_BTN2 | JOY_BTN3))
					{
						mask = JOY_BTN3;
					}
					else if (old_osdbtn & JOY_BTN3)
					{
						if (!(osdbtn & (JOY_BTN1 | JOY_BTN2))) osdbtn &= ~JOY_BTN3;
						mask = 0;
					}
				}

				if((mask & JOY_BTN2) && !(old_osdbtn & JOY_BTN2)) mask = 0;
			}

			memset(key_states, 0, sizeof(key_states));
			struct input_event ev;
			ev.type = EV_KEY;
			ev.value = press;

			int cfg_switch = menu_allow_cfg_switch() && (osdbtn & JOY_BTN2) && press;
			
			switch (mask)
			{
			case JOY_RIGHT:
				if (cfg_switch)
				{
					user_io_set_ini(0);
					osdbtn = 0;
					return;
				}
				ev.code = KEY_RIGHT;
				break;

			case JOY_LEFT:
				if (cfg_switch)
				{
					user_io_set_ini(1);
					osdbtn = 0;
					return;
				}
				ev.code = KEY_LEFT;
				break;

			case JOY_UP:
				if (cfg_switch)
				{
					user_io_set_ini(2);
					osdbtn = 0;
					return;
				}
				ev.code = KEY_UP;
				break;

			case JOY_DOWN:
				if (cfg_switch)
				{
					user_io_set_ini(3);
					osdbtn = 0;
					return;
				}
				ev.code = KEY_DOWN;
				break;

			case JOY_BTN1:
				ev.code = KEY_ENTER;
				break;

			case JOY_BTN2:
				ev.code = KEY_BACK;
				break;

			case JOY_BTN3:
				ev.code = KEY_BACKSPACE;
				break;

			case JOY_BTN4:
				ev.code = KEY_TAB;
				break;

			case JOY_L:
				ev.code = KEY_MINUS;
				break;

			case JOY_R:
				ev.code = KEY_EQUAL;
				break;

			case JOY_R2:
				ev.code = KEY_GRAVE;
				break;

			default:
				ev.code = (bnum == BTN_OSD) ? KEY_MENU : 0;
			}

			input_cb(&ev, 0, 0);
		}
		else if (video_fb_state())
		{
			switch (mask)
			{
			case JOY_RIGHT:
				uinp_send_key(KEY_RIGHT, press);
				break;

			case JOY_LEFT:
				uinp_send_key(KEY_LEFT, press);
				break;

			case JOY_UP:
				uinp_send_key(KEY_UP, press);
				break;

			case JOY_DOWN:
				uinp_send_key(KEY_DOWN, press);
				break;

			case JOY_BTN1:
				uinp_send_key(KEY_ENTER, press);
				break;

			case JOY_BTN2:
				uinp_send_key(KEY_ESC, press);
				break;

			case JOY_BTN3:
				uinp_send_key(KEY_SPACE, press);
				break;

			case JOY_BTN4:
				uinp_send_key(KEY_TAB, press);
				break;

			case JOY_L:
				uinp_send_key(KEY_PAGEUP, press);
				break;

			case JOY_R:
				uinp_send_key(KEY_PAGEDOWN, press);
				break;
			}
		}
		else if(jnum)
		{
			set_key_state(num, code, press, mask);
		}
	}
}

/* ========================================================================
 *  joy_analog - processes an analog stick axis update for a player
 * ======================================================================== */

void joy_analog(int dev, int axis, int offset, int stick)
{
	int num = input[dev].num;
	static int pos[2][NUMPLAYERS][2] = {};

	if (grabbed && num > 0 && --num < NUMPLAYERS)
	{
		pos[stick][num][axis] = offset;
		int x = pos[stick][num][0], y = pos[stick][num][1];

		if (joy_dir_is_diagonal(x, y))
		{
			// Update maximum observed diag
			// Use sum of squares and only calc sqrt() when necessary
			const int ss_range_curr = x * x + y * y;
			if ((ss_range_curr > input[dev].ss_range[stick]))
			{
				input[dev].ss_range[stick] = ss_range_curr;
				input[dev].max_range[stick] = sqrtf(ss_range_curr);
			}
		}

		// Update maximum observed cardinal distance
		const int c_dist = abs((x > y) == (x > -y) ? x : y);
		if (c_dist > input[dev].max_cardinal[stick])
		{
			input[dev].max_cardinal[stick] = c_dist;
		}

		joy_apply_deadzone(&x, &y, &input[dev], stick);

		if (is_n64())
		{
			// Emulate N64 joystick range and shape for regular -127-+127 controllers
			n64_joy_emu(x, y, &x, &y, input[dev].max_cardinal[stick], input[dev].max_range[stick]);
			stick_swap(num, stick, &num, &stick);
		}

		if (stick)
		{
			user_io_r_analog_joystick(num, (char)x, (char)y);
		}
		else
		{
			user_io_l_analog_joystick(num, (char)x, (char)y);
		}
	}
}

/* ========================================================================
 *  Per-frame key-state update (advances autofire frame counters)
 * ======================================================================== */

void key_update_frames_held()
{
	for (int i = 0; i < NUMPLAYERS; i++) {
		for (int k = 0; k < key_states[i].count; k++) {
			if (key_states[i].mask[k] != 0) {
				key_states[i].frames_held[k]++;
			} else {
				key_states[i].frames_held[k]= 0;
			}
		}
	}
}
