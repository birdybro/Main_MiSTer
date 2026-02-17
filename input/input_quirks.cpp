/*
 * input_quirks.cpp - Device-specific quirk handlers for the input subsystem.
 *
 * Contains processing routines for controllers that require special handling:
 * Keyrah keyboard adapter, touchscreen light-gun emulation, DualShock mouse
 * throttle, Atari VCS paddle/spinner, Nintendo JoyCon pairing/combining,
 * OpenFIRE signal, force-feedback rumble, steering wheel configuration,
 * and JAMMA arcade encoder mappings.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdint.h>

#include "input.h"
#include "input/input_internal.h"
#include "cfg.h"
#include "file_io.h"
#include "support.h"
#include "user_io.h"
#include "menu.h"
#include "video.h"
#include "spi.h"
#include "hardware.h"

/* ========================================================================
 *  Keyrah v2 keyboard adapter translation
 * ======================================================================== */

static unsigned char kr_fn_table[] =
{
	KEY_KPSLASH,    KEY_PAUSE,
	KEY_KPASTERISK, KEY_PRINT,
	KEY_LEFT,       KEY_HOME,
	KEY_RIGHT,      KEY_END,
	KEY_UP,         KEY_PAGEUP,
	KEY_DOWN,       KEY_PAGEDOWN,
	KEY_F1,         KEY_F11,
	KEY_F2,         KEY_F12,

	KEY_F3,         KEY_F17, // EMU_MOUSE
	KEY_F4,         KEY_F18, // EMU_JOY0
	KEY_F5,         KEY_F19, // EMU_JOY1
	KEY_F6,         KEY_F20, // EMU_NONE

    //Emulate keypad for A600
	KEY_1,          KEY_KP1,
	KEY_2,          KEY_KP2,
	KEY_3,          KEY_KP3,
	KEY_4,          KEY_KP4,
	KEY_5,          KEY_KP5,
	KEY_6,          KEY_KP6,
	KEY_7,          KEY_KP7,
	KEY_8,          KEY_KP8,
	KEY_9,          KEY_KP9,
	KEY_0,          KEY_KP0,
	KEY_MINUS,      KEY_KPMINUS,
	KEY_EQUAL,      KEY_KPPLUS,
	KEY_BACKSLASH,  KEY_KPASTERISK,
	KEY_LEFTBRACE,  KEY_F13,    //KP(
	KEY_RIGHTBRACE, KEY_F14,    //KP)
	KEY_DOT,        KEY_KPDOT,
	KEY_ENTER,      KEY_KPENTER
};

int keyrah_trans(int key, int press)
{
	static int fn = 0;

	if (key == KEY_NUMLOCK)    return KEY_F13; // numlock -> f13
	if (key == KEY_SCROLLLOCK) return KEY_F14; // scrlock -> f14
	if (key == KEY_INSERT)     return KEY_F16; // insert -> f16. workaround!

	if (key == KEY_102ND)
	{
		if (!press && fn == 1) menu_key_set(KEY_MENU);
		fn = press ? 1 : 0;
		return 0;
	}
	else if (fn)
	{
		fn |= 2;
		for (uint32_t n = 0; n<(sizeof(kr_fn_table) / (2 * sizeof(kr_fn_table[0]))); n++)
		{
			if ((key&255) == kr_fn_table[n * 2]) return kr_fn_table[(n * 2) + 1];
		}
	}

	return key;
}

/* ========================================================================
 *  Mouse throttling helper
 * ======================================================================== */

/* Rate-limit mouse movement reports from DualShock trackpads and similar
 * high-resolution devices to avoid flooding the FPGA core. */
void send_mouse_with_throttle(int dev, int xval, int yval, int8_t wval)
{
	int i = dev;
	if (input[dev].bind >= 0) dev = input[dev].bind;

	if (is_menu() && !video_fb_state()) printf("%s: dx=%d, dy=%d, scroll=%d\n", input[i].devname, xval, yval, wval);

	int throttle = cfg.mouse_throttle ? cfg.mouse_throttle : 1;
	if (input[dev].ds_mouse_emu) throttle *= 4;
	if (input[dev].quirk == QUIRK_TOUCHGUN) throttle *= 12;

	input[i].accx += xval;
	xval = input[i].accx / throttle;
	input[i].accx -= xval * throttle;

	input[i].accy -= yval;
	yval = input[i].accy / throttle;
	input[i].accy -= yval * throttle;

	mouse_cb(xval, yval, wval);
}

/* ========================================================================
 *  Touchscreen processing (RasPad3 etc.)
 * ======================================================================== */

uint32_t touch_rel = 0;

/* Release emulated mouse button after touchscreen lift-off timeout. */
void check_touch_release()
{
	if (touch_rel && CheckTimer(touch_rel))
	{
		touch_rel = 0;
		mice_btn = 0;
		mouse_btn_req();
	}
}

/* Translate touchscreen ABS_X/ABS_Y into light-gun coordinates or
 * emulated mouse movement depending on core configuration. */
void touchscreen_proc(int dev, input_event *ev)
{
	struct input_absinfo absinfo;
	int i = dev;
	if (input[dev].bind >= 0) dev = input[dev].bind;

	if (ev->type == EV_KEY)
	{
		if (ev->value == 1)
		{
			input[i].misc_flags = 0xC0;
			touch_rel = 0;

			ioctl(pool[i].fd, EVIOCGABS(ABS_X), &absinfo);
			input[i].lastx = absinfo.value;
			input[i].startx = absinfo.value;

			ioctl(pool[i].fd, EVIOCGABS(ABS_Y), &absinfo);
			input[i].lasty = absinfo.value;
			input[i].starty = absinfo.value;
		}
		else
		{
			input[i].misc_flags = 0;
			mice_btn = 0;

			if (input[dev].lightgun)
			{
				menu_lightgun_cb(i, EV_KEY, 0x131, 0);
			}
			else
			{
				if (abs(input[i].lastx - input[i].startx) < 8 && abs(input[i].lasty - input[i].starty) < 8)
				{
					mice_btn |= 1;
					touch_rel = GetTimer(100);
				}
			}

			mouse_btn_req();
		}
	}
	else if (ev->type == EV_ABS && ev->code == ABS_MT_SLOT && ev->value == 3 && (input[i].misc_flags & 0x80))
	{
		input[i].misc_flags = 0;
		mice_btn = 0;
		mouse_btn_req();
		input[dev].lightgun = !input[dev].lightgun;
		Info(input[dev].lightgun ? "Light Gun mode is ON" : "Light Gun mode is OFF");
	}

	if (input[dev].lightgun)
	{
		if (ev->type == EV_KEY && ev->value == 1)
		{
			mice_btn |= 1;
			mouse_btn_req();
			menu_lightgun_cb(i, EV_KEY, 0x131, 1);
		}
		else if (ev->type == EV_ABS)
		{
			if (ev->code == ABS_MT_POSITION_X)
			{
				ev->code = ABS_X;
				absinfo.minimum = input[i].guncal[2];
				absinfo.maximum = input[i].guncal[3];
				menu_lightgun_cb(i, ev->type, ev->code, ev->value);
				input_cb(ev, &absinfo, i);
			}
			else if (ev->code == ABS_MT_POSITION_Y)
			{
				ev->code = ABS_Y;
				absinfo.minimum = input[i].guncal[0];
				absinfo.maximum = input[i].guncal[1];
				menu_lightgun_cb(i, ev->type, ev->code, ev->value);
				input_cb(ev, &absinfo, i);
			}
			else if (ev->code == ABS_MT_SLOT && (input[i].misc_flags & 0x80))
			{
				if (ev->value == 1) input[i].misc_flags |= 1;
				if (ev->value == 2) input[i].misc_flags |= 2;

				if (input[i].misc_flags & 2) mice_btn = 4;
				else if (input[i].misc_flags & 1) mice_btn = 2;
				else mice_btn = 1;

				mouse_btn_req();
			}
		}
	}
	else
	{
		if (ev->type == EV_ABS)
		{
			if (input[i].misc_flags & 0x80)
			{
				if (ev->code == ABS_MT_SLOT)
				{
					if (ev->value) input[i].misc_flags &= ~0x40;
					else input[i].misc_flags |= 0x40;

					if (ev->value == 1) input[i].misc_flags |= 1;
					if (ev->value == 2) input[i].misc_flags |= 2;

					if (input[i].misc_flags & 2) mice_btn = 4;
					else if (input[i].misc_flags & 1) mice_btn = 2;

					mouse_btn_req();
				}
				else if (input[i].misc_flags & 0x40)
				{
					if (ev->code == ABS_MT_POSITION_X)
					{
						int dx = ev->value - input[i].lastx;
						if (dx > 255) dx = 255;
						if (dx < -256) dx = -256;
						input[i].lastx = ev->value;
						send_mouse_with_throttle(i, dx, 0, 0);
					}
					else if (ev->code == ABS_MT_POSITION_Y)
					{
						int dy = ev->value - input[i].lasty;
						if (dy > 255) dy = 255;
						if (dy < -256) dy = -256;
						input[i].lasty = ev->value;
						send_mouse_with_throttle(i, 0, -dy, 0);
					}
				}
			}
		}
	}
}

/* ========================================================================
 *  Atari VCS controller processing
 * ======================================================================== */

/* Handle Atari VCS controller quirks: 5/8-button mode toggle via combo,
 * spinner-to-paddle translation, and button remapping. Returns 0 to
 * suppress the event or 1 to pass it through to normal processing. */
int vcs_proc(int dev, input_event *ev)
{
	devInput *inp = &input[dev];

	if (ev->type == EV_KEY)
	{
		int flg = 0;
		int alt = inp->mod && (inp->misc_flags & 2);
		switch (ev->code)
		{
		case 0x130: // red top
			if (!ev->value)
			{
				ev->code = !alt ? 0x135 : 0x130;
				input_cb(ev, 0, dev);
			}
			ev->code = alt ? 0x135 : 0x130;
			flg = 1;
			break;

		case 0x131: // red bottom
			flg = 2;
			break;

		case 0x0AC: // atari
			if (!ev->value)
			{
				ev->code = !alt ? 0x136 : 0x132;
				input_cb(ev, 0, dev);
			}
			ev->code = alt ? 0x136 : 0x132;
			flg = 4;
			break;

		case 0x09E: // back
			if (!ev->value)
			{
				ev->code = !alt ? 0x137 : 0x133;
				input_cb(ev, 0, dev);
			}
			ev->code = alt ? 0x137 : 0x133;
			flg = 8;
			break;

		case 0x08B: // menu
			if (!ev->value)
			{
				ev->code = !alt ? 0x138 : 0x134;
				input_cb(ev, 0, dev);
			}
			ev->code = alt ? 0x138 : 0x134;
			flg = 16;
			break;
		}

		if (flg)
		{
			if (ev->value) inp->misc_flags |= flg;
			else inp->misc_flags &= ~flg;

			if ((inp->misc_flags & 0x1F) == 0x1B)
			{
				inp->mod = !inp->mod;
				inp->has_map = 0;
				inp->has_mmap = 0;
				Info(inp->mod ? "8-button mode" : "5-button mode");
			}
		}
		if (ev->code == 0x131 && inp->mod) return 0;
	}
	else if (ev->code == 7)
	{
		if (inp->spinner_prev < 0) inp->spinner_prev = ev->value;
		int acc = inp->spinner_prev;

		int diff =
			(acc > 700 && ev->value < 300) ? (ev->value + 1024 - acc) :
			(acc < 300 && ev->value > 700) ? (ev->value - 1024 - acc) : (ev->value - acc);

		if (inp->spinner_accept)
		{
			inp->spinner_accept = (inp->spinner_dir && diff >= 0) || (!inp->spinner_dir && diff <= 0);
		}
		else if (diff <= -6 || diff >= 6)
		{
			inp->spinner_accept = 1;
			inp->spinner_dir = (diff > 0) ? 1 : 0;
			diff = inp->spinner_dir ? 1 : -1;
		}

		if (inp->spinner_accept && diff)
		{
			inp->spinner_prev = ev->value;

			if ((inp->misc_flags & 0x1F) == 0xB && ((inp->misc_flags & 0x20) ? (diff < -30) : (diff > 30)))
			{
				inp->misc_flags ^= 0x20;
				Info((inp->misc_flags & 0x20) ? "Spinner: Enabled" : "Spinner: Disabled");
			}

			if (inp->misc_flags & 0x20)
			{
				inp->paddle_val += diff;
				if (inp->paddle_val < 0) inp->paddle_val = 0;
				if (inp->paddle_val > 511) inp->paddle_val = 511;

				if (is_menu()) printf("vcs: diff = %d, paddle=%d, ev.value = %d\n", diff, inp->paddle_val, ev->value);

				input_absinfo absinfo;
				absinfo.minimum = 0;
				absinfo.maximum = 511;
				ev->type = EV_ABS;
				ev->code = 8;
				ev->value = inp->paddle_val;
				input_cb(ev, &absinfo, dev);

				inp->spinner_acc += diff;
				ev->type = EV_REL;
				ev->code = 7;
				ev->value = inp->spinner_acc / 2;
				inp->spinner_acc -= ev->value * 2;
				input_cb(ev, 0, dev);
			}
		}
		return 0;
	}

	return 1;
}

/* ========================================================================
 *  OpenFIRE lightgun MiSTer-compatible mode signal
 * ======================================================================== */

void openfire_signal()
{
	for (int i = 0; i < NUMDEV; i++)
	{
		if (input[i].vid == 0xf143 && strstr(input[i].name, "OpenFIRE ") &&
			strstr(input[i].devname, "mouse") == NULL)
		{
			// OF generates 3 devices, so just focus on the one actual gamepad slot.
			char *nameInit = input[i].name;
			if (memcmp(nameInit+strlen(input[i].name)-5, "Mouse", 5) != 0 && memcmp(nameInit+strlen(input[i].name)-8, "Keyboard", 8) != 0)
			{
				char mname[strlen(input[i].name)];
				strcpy(mname, input[i].name);

				// Cleanup mname to replace offending characters not used in device filepath.
				char *p;
				while ((p = strchr(mname, '/'))) *p = '_';
				while ((p = strchr(mname, ' '))) *p = '_';
				while ((p = strchr(mname, '*'))) *p = '_';
				while ((p = strchr(mname, ':'))) *p = '_';

				char devicePath[29+strlen(mname)+strlen(strrchr(input[i].id, '/')+1)];
				sprintf(devicePath, "/dev/serial/by-id/usb-%s_%s-if00", mname, strrchr(input[i].id, '/')+1);

				FILE *deviceFile = fopen(devicePath, "r+");
				if(deviceFile == NULL) {
					printf("Failed to send command to %s: device path doesn't exist?\n", input[i].name);
				} else {
					fprintf(deviceFile, "M0x9");
					printf("%s (device no. %i) set to MiSTer-compatible mode.\n", input[i].name, i);
					fclose(deviceFile);
				}
			}
		}
	}
}

/* ========================================================================
 *  Nintendo JoyCon pairing & combined-mode processing
 * ======================================================================== */

/* Pair left/right JoyCon controllers that share the same :combo LED
 * identifier, binding them together so they act as a single gamepad. */
void check_joycon()
{
	while (1)
	{
		int l = -1, r = -1;
		int id_combo = 0;

		for (int i = 0; i < NUMDEV; i++)
		{
			if (input[i].quirk == QUIRK_JOYCON && !JOYCON_COMBO(i))
			{
				if (JOYCON_LEFT(i))
				{
					int id = 0;
					char *led_path = get_led_path(i);
					if (led_path) id = get_led(led_path, ":combo");
					if (id && (!id_combo || id_combo == id))
					{
						id_combo = id;
						l = i;
					}
				}
				else if (JOYCON_RIGHT(i))
				{
					int id = 0;
					char *led_path = get_led_path(i);
					if (led_path) id = get_led(led_path, ":combo");
					if (id && (!id_combo || id_combo == id))
					{
						id_combo = id;
						r = i;
					}
				}
			}
		}

		if (l >= 0 && r >= 0)
		{
			printf("** joycon_l = %d, joycon_r = %d, id = %d\n", l, r, id_combo);

			input[l].bind = r;
			input[r].bind = l;
			input[l].misc_flags |= 1 << 31;
			input[r].misc_flags |= 1 << 31;
			strcpy(input[l].idstr, "057e_2009");
			strcpy(input[r].idstr, "057e_2009");
		}
		else break;
	}
}

/* Re-route events from a paired JoyCon half to its partner's input
 * stream, synthesising DPAD from the left stick and remapping buttons
 * so the combined pair behaves like a standard controller. */
int process_joycon(int dev, input_event *ev, input_absinfo *absinfo)
{
	if (ev->type == EV_ABS)
	{
		if (JOYCON_COMBO(dev)) return 0;
		if (ev->code == 4 && JOYCON_RIGHT(dev)) ev->value = -ev->value;
		if (ev->code == 0 && JOYCON_LEFT(dev)) ev->value = -ev->value;
		return 0;
	}

	int mask = 0;

	// simulate DPAD on left joycon
	if (JOYCON_COMBO(dev) && (ev->code & ~3) == 0x220)
	{
		mask = 0x100 << (ev->code & 3);
		input[dev].misc_flags = ev->value ? (input[dev].misc_flags | mask) : (input[dev].misc_flags & ~mask);
		if (ev->value)
		{
			ev->value = (ev->code & 1) ? 1 : -1;
		}
		else
		{
			mask = (ev->code & 2) ? 0x400 : 0x100;
			ev->value = (input[dev].misc_flags & mask) ? -1 : (input[dev].misc_flags & (mask << 1)) ? 1 : 0;
		}

		ev->code = (ev->code & 2) ? 16 : 17;
		ev->type = EV_ABS;
		absinfo->minimum = -1;
		absinfo->maximum = 1;
		return 0;
	}

	//check for request to combine/split joycons
	switch (ev->code)
	{
		case 0x136: case 0x137: mask = 1; break;
		case 0x138: case 0x139: mask = 2; break;
		case 0x13D: case 0x13E: mask = 4; break;
		default: return 0;
	}

	input[dev].misc_flags = ev->value ? (input[dev].misc_flags | mask) : (input[dev].misc_flags & ~mask);

	if (JOYCON_REQ(dev))
	{
		int uncombo = 0;
		int l = -1, r = -1;
		for (int n = 0; n < NUMDEV; n++)
		{
			if (input[n].quirk == QUIRK_JOYCON)
			{
				if (JOYCON_COMBO(n))
				{
					if (JOYCON_REQ(n) && JOYCON_REQ(input[n].bind))
					{
						r = n;
						l = input[n].bind;
						uncombo = 1;
						break;
					}
				}
				else if (JOYCON_RIGHT(n) && JOYCON_REQ(n)) r = n;
				else if (JOYCON_LEFT(n) && JOYCON_REQ(n)) l = n;
			}
		}

		if (l >= 0 && r >= 0)
		{
			uint8_t id = 0;
			char *led_path;

			printf(uncombo ? "Joycons request split\n" : "Joycons request combo\n");

			if (!uncombo)
			{
				FileLoad("/tmp/combo_id", &id, sizeof(id));
				if (!(++id)) ++id;
				FileSave("/tmp/combo_id", &id, sizeof(id));
			}

			led_path = get_led_path(l); if (led_path) set_led(led_path, ":combo", id);
			led_path = get_led_path(r); if (led_path) set_led(led_path, ":combo", id);

			printf("Close all devices.\n");
			for (int i = 0; i < NUMDEV; i++) if (pool[i].fd >= 0)
			{
				ioctl(pool[i].fd, EVIOCGRAB, 0);
				close(pool[i].fd);
			}
			update_num_hw(l, 7);
			update_num_hw(r, 7);
			usleep(500000);
			update_num_hw(l, 0);
			update_num_hw(r, 0);
			usleep(500000);
			return 1;
		}
	}

	return 0;
}

/* ========================================================================
 *  Force-feedback (rumble) support
 * ======================================================================== */

/* Find the evdev index that owns the rumble motor for a given player. */
int get_rumble_device(int player)
{
	for (int i = 0; i < NUMDEV; i++)
	{
		int dev = i;
		if (input[i].bind >= 0) dev = input[i].bind;

		if (input[dev].num == player && input[i].has_rumble)
		{
			return i;
		}
	}

	return -1;
}

/* Upload and play (or stop) a force-feedback rumble effect on a device.
 * Passing strong_mag=0 and weak_mag=0 removes the current effect. */
int rumble_input_device(int devnum, uint16_t strong_mag, uint16_t weak_mag, uint16_t duration, uint16_t delay)
{
	int ioret = 0;
	if (!input[devnum].has_rumble) return 0;
	int fd = pool[devnum].fd;
	if (!(fd >= 0)) return 0;

	if (!strong_mag && !weak_mag) //Stop rumble
	{
		if (input[devnum].rumble_effect.id == -1) return 1; //No uploaded effect

		ioret = ioctl(fd, EVIOCRMFF, input[devnum].rumble_effect.id);
		input[devnum].rumble_effect.id = -1; //always set to -1 even if we fail to remove it?
		return ioret != -1;
	}
	else {
		//Upload effect and then immediately play it
		//If the effect id in the input struct is -1, it will be filled with the newly uploaded effect
		//If it is filled with an already uploaded effect, the effect is modified in place
		struct ff_effect *fef;
		fef = &input[devnum].rumble_effect;
		fef->type = FF_RUMBLE;

		fef->direction = input[devnum].quirk == QUIRK_WHEEL ? 0x4000 : 0x0000;
		fef->u.rumble.strong_magnitude = strong_mag;
		fef->u.rumble.weak_magnitude = weak_mag;
		fef->replay.length = duration;
		fef->replay.delay = delay;
		ioret = ioctl(fd, EVIOCSFF, fef);

		if (ioret == -1)
		{
			printf("RUMBLE UPLOAD FAILED %s\n", strerror(errno));
			return 0;
		}

		//Play effect
		struct input_event play_ev;
		play_ev.type = EV_FF;
		play_ev.code = input[devnum].rumble_effect.id;
		play_ev.value = 1;
		ioret = write(fd, (const void *)&play_ev, sizeof(play_ev));
		return ioret != -1;
	}
	return 0;
}

/* Convenience wrapper: split a 16-bit value into strong/weak magnitudes
 * and issue a rumble effect, de-duplicating identical consecutive calls. */
void set_rumble(int dev, uint16_t rumble_val)
{
	if (input[dev].last_rumble != rumble_val)
	{
		uint16_t strong_m, weak_m;

		strong_m = (rumble_val & 0xFF00) + (rumble_val >> 8);
		weak_m = (rumble_val << 8) + (rumble_val & 0x00FF);

		rumble_input_device(dev, strong_m, weak_m, 0x7FFF);
		input[dev].last_rumble = rumble_val;
	}
}

/* ========================================================================
 *  Steering wheel setup & range control
 * ======================================================================== */

/* Write a rotation range (in degrees) to the sysfs "range" attribute
 * of a steering wheel, e.g. 270 or 900. */
void set_wheel_range(int dev, int range)
{
	static char path[1024];
	if (range && input[dev].sysfs[0])
	{
		sprintf(path, "/sys%s/device/range", input[dev].sysfs);

		FILE* f = fopen(path, "w");
		if (f)
		{
			fprintf(f, "%d", range);
			fclose(f);
		}
	}
}

/* Scan all connected devices for known steering-wheel VID/PID combos
 * and configure their pedal axes, force-feedback, and rotation ranges. */
void setup_wheels()
{
	if (cfg.wheel_force > 100) cfg.wheel_force = 100;

	for (int i = 0; i < NUMDEV; i++)
	{
		if (pool[i].fd != -1)
		{
			// steering wheel axis
			input[i].wh_steer = 0;
			// accelerator pedal axis
			input[i].wh_accel = -1;
			// brake pedal axis
			input[i].wh_brake = -1;
			// clutch pedal axis
			input[i].wh_clutch = -1;
			// shared accel and brake pedal axis
			input[i].wh_combo = -1;
			// invert pedal values range (if >0)
			input[i].wh_pedal_invert = -1;

			// Logitech Wheels
			if (input[i].vid == 0x046d)
			{
				switch (input[i].pid)
				{
				case 0xc299: // LOGITECH_G25_WHEEL
				case 0xc29b: // LOGITECH_G27_WHEEL
				case 0xc24f: // LOGITECH_G29_WHEEL
					input[i].wh_accel = 2;
					input[i].wh_brake = 5;
					input[i].wh_clutch = 1;
					input[i].quirk = QUIRK_WHEEL;
					break;

				case 0xc294: // LOGITECH_WHEEL
					input[i].wh_combo = 1;
					input[i].quirk = QUIRK_WHEEL;
					break;

				case 0xc298: // LOGITECH_DFP_WHEEL
					input[i].wh_accel = 1;
					input[i].wh_brake = 5;
					input[i].quirk = QUIRK_WHEEL;
					break;

				case 0xc29a: // LOGITECH_DFGT_WHEEL
					input[i].wh_accel = 1;
					input[i].wh_brake = 2;
					input[i].quirk = QUIRK_WHEEL;
					break;

				//case 0xc262: // LOGITECH_G920_WHEEL
				//case 0xc295: // LOGITECH_MOMO_WHEEL
				}

				if (input[i].quirk == QUIRK_WHEEL)
				{
					struct input_event ie = {};
					ie.type = EV_FF;
					ie.code = FF_AUTOCENTER;
					ie.value = 0xFFFFUL * cfg.wheel_force / 100;
					write(pool[i].fd, &ie, sizeof(ie));

					set_wheel_range(i, cfg.wheel_range);
				}
			}

			// Fanatec Wheels
			else if (input[i].vid == 0x0eb7)
			{
				switch (input[i].pid)
				{
				case 0x0004:   // CLUBSPORT_V25_WHEELBASE_DEVICE_ID
				case 0x0006:   // PODIUM_WHEELBASE_DD1_DEVICE_ID
				case 0x0007:   // PODIUM_WHEELBASE_DD2_DEVICE_ID
					input[i].wh_accel = 2;
					input[i].wh_brake = 5;
					input[i].wh_clutch = 1;
					input[i].quirk = QUIRK_WHEEL;
					break;

				//case 0x0001: // CLUBSPORT_V2_WHEELBASE_DEVICE_ID
				//case 0x0005: // CSL_ELITE_PS4_WHEELBASE_DEVICE_ID
				//case 0x0011: // CSR_ELITE_WHEELBASE_DEVICE_ID
				//case 0x0020: // CSL_DD_WHEELBASE_DEVICE_ID
				//case 0x0E03: // CSL_ELITE_WHEELBASE_DEVICE_ID
				}

				if (input[i].quirk == QUIRK_WHEEL)
				{
					struct ff_effect fef;
					fef.type = FF_SPRING;
					fef.id = -1;
					fef.u.condition[0].right_saturation = 0xFFFFUL * cfg.wheel_force / 100;
					fef.u.condition[0].left_saturation = 0xFFFFUL * cfg.wheel_force / 100;
					fef.u.condition[0].right_coeff = 0x7FFF;
					fef.u.condition[0].left_coeff = 0x7FFF;
					fef.u.condition[0].deadband = 0x0;
					fef.u.condition[0].center = 0x0;
					fef.u.condition[1] = fef.u.condition[0];
					fef.replay.delay = 0;

					if (ioctl(pool[i].fd, EVIOCSFF, &fef) >= 0)
					{
						struct input_event play_ev;
						play_ev.type = EV_FF;
						play_ev.code = fef.id;
						play_ev.value = 1;
						write(pool[i].fd, (const void *)&play_ev, sizeof(play_ev));
					}

					set_wheel_range(i, cfg.wheel_range);
				}
			}

			// Thrustmaster Guillemot Wheels
			else if (input[i].vid == 0x06f8)
			{
				switch (input[i].pid)
				{
				case 0x0004: // Force Feedback Racing Wheel
					input[i].wh_steer = 8;
					input[i].wh_accel = 9;
					input[i].wh_brake = 10;
					input[i].wh_pedal_invert = 1;
					input[i].quirk = QUIRK_WHEEL;
					break;
				}

				if (input[i].quirk == QUIRK_WHEEL)
				{
					struct input_event ie = {};
					ie.type = EV_FF;
					ie.code = FF_AUTOCENTER;
					ie.value = 0xFFFFUL * cfg.wheel_force / 100;
					write(pool[i].fd, &ie, sizeof(ie));

					set_wheel_range(i, cfg.wheel_range);
				}
			}

			// Thrustmaster Wheels
			else if (input[i].vid == 0x044f)
			{
				switch (input[i].pid)
				{
				case 0xb655: // FGT Rumble 3-in-1 (PC)
				case 0xb65b: // F430 Cockpit Wireless (PC)
					input[i].wh_steer = 0;
					input[i].wh_accel = 5;
					input[i].wh_brake = 1;
					input[i].quirk = QUIRK_WHEEL;
					break;
				case 0xb66e: // T300RS Racing Wheel (PC/PS3)
					input[i].wh_steer = 0;
					input[i].wh_accel = 5;
					input[i].wh_brake = 1;
					input[i].wh_clutch = 6;
					input[i].quirk = QUIRK_WHEEL;
					break;
				}
			}

			//Namco NeGcon via Arduino, RetroZord or Reflex Adapt
			else if (((input[i].vid == 0x2341 || (input[i].vid == 0x1209 && input[i].pid == 0x595A)) && strstr(input[i].name, "RZordPsWheel")) ||
					 (input[i].vid == 0x16D0 && input[i].pid == 0x127E && strstr(input[i].name, "ReflexPSWheel")))
			{
				input[i].wh_accel = 6;
				input[i].wh_brake = 10;
				input[i].wh_clutch = 2;
				input[i].quirk = QUIRK_WHEEL;
			}
		}
	}
}

/* ========================================================================
 *  JAMMA / J-PAC / I-PAC encoder mapping tables
 * ======================================================================== */

// Jammasd/J-PAC/I-PAC have shifted keys: when 1P start is kept pressed, it acts as a shift key,
// outputting other key signals. Example: 1P start + 2P start = KEY_ESC
// Shifted keys are passed as normal keyboard keys.
struct jamma_map_entry jamma2joy[] =
{
	{KEY_5,         1, 0x120}, // 1P coin
	{KEY_1,         1, 0x121}, // 1P start (shift key)
	{KEY_UP,        1, 0x122}, // 1P up
	{KEY_DOWN,      1, 0x123}, // 1P down
	{KEY_LEFT,      1, 0x124}, // 1P left
	{KEY_RIGHT,     1, 0x125}, // 1P right
	{KEY_LEFTCTRL,  1, 0x126}, // 1P 1
	{KEY_LEFTALT,   1, 0x127}, // 1P 2
	{KEY_SPACE,     1, 0x128}, // 1P 3
	{KEY_LEFTSHIFT, 1, 0x129}, // 1P 4
	{KEY_Z,         1, 0x12A}, // 1P 5
	{KEY_X,         1, 0x12B}, // 1P 6
	{KEY_C,         1, 0x12C}, // 1P 7
	{KEY_V,         1, 0x12D}, // 1P 8

	{KEY_9,         1, 0x12E}, // Test
	{KEY_TAB,       1, 0x12F}, // Tab (shift + 1P right)
	{KEY_ENTER,     1, 0x130}, // Enter (shift + 1P left)
	// ~ Tidle supportted?
	{KEY_P,         1, 0x131}, // P (pause) (shift + 1P down)
	{KEY_F1,        1, 0x132}, // Service
	{KEY_F2,        1, 0x133}, // Test
	{KEY_F3,        1, 0x134}, // Tilt

	{KEY_6,         2, 0x120}, // 2P coin
	{KEY_2,         2, 0x121}, // 2P start
	{KEY_R,         2, 0x122}, // 2P up
	{KEY_F,         2, 0x123}, // 2P down
	{KEY_D,         2, 0x124}, // 2P left
	{KEY_G,         2, 0x125}, // 2P right
	{KEY_A,         2, 0x126}, // 2P 1
	{KEY_S,         2, 0x127}, // 2P 2
	{KEY_Q,         2, 0x128}, // 2P 3
	{KEY_W,         2, 0x129}, // 2P 4
	{KEY_I,         2, 0x12A}, // 2P 5
	{KEY_K,         2, 0x12B}, // 2P 6
	{KEY_J,         2, 0x12C}, // 2P 7
	{KEY_L,         2, 0x12D}, // 2P 8

/*
	some key codes overlap with 1P/2P buttons.

	{KEY_7,         3, 0x120}, // 3P coin
	{KEY_3,         3, 0x121}, // 3P start
	{KEY_I,         3, 0x122}, // 3P up
	{KEY_K,         3, 0x123}, // 3P down
	{KEY_J,         3, 0x124}, // 3P left
	{KEY_L,         3, 0x125}, // 3P right
	{KEY_RIGHTCTRL, 3, 0x126}, // 3P 1
	{KEY_RIGHTSHIFT,3, 0x127}, // 3P 2
	{KEY_ENTER,     3, 0x128}, // 3P 3
	{KEY_O,         3, 0x129}, // 3P 4

	{KEY_8,         4, 0x120}, // 4P coin
	{KEY_4,         4, 0x121}, // 4P start
	{KEY_Y,         4, 0x122}, // 4P up
	{KEY_N,         4, 0x123}, // 4P down
	{KEY_V,         4, 0x124}, // 4P left
	{KEY_U,         4, 0x125}, // 4P right
	{KEY_B,         4, 0x126}, // 4P 1
	{KEY_E,         4, 0x127}, // 4P 2
	{KEY_H,         4, 0x128}, // 4P 3
	{KEY_M,         4, 0x129}, // 4P 4
*/
};

const int jamma2joy_count = sizeof(jamma2joy) / sizeof(jamma2joy[0]);

// Second Jammasd/J-PAC/I-PAC quirk. It's equivalent to jamma2joy but assigned to players 3 and 4
// to give support to JAMMA-VERSUS with two JAMMA USB control interfaces.
// i.e. JammaSD for Players1-2 (on a first cabinet), and J-PAC for Payers 3-4 (on a second cabinet)
struct jamma_map_entry jamma22joy[] =
{
	{KEY_5,         3, 0x120}, // 3P coin
	{KEY_1,         3, 0x121}, // 3P start
	{KEY_UP,        3, 0x122}, // 3P up
	{KEY_DOWN,      3, 0x123}, // 3P down
	{KEY_LEFT,      3, 0x124}, // 3P left
	{KEY_RIGHT,     3, 0x125}, // 3P right
	{KEY_LEFTCTRL,  3, 0x126}, // 3P 1
	{KEY_LEFTALT,   3, 0x127}, // 3P 2
	{KEY_SPACE,     3, 0x128}, // 3P 3
	{KEY_LEFTSHIFT, 3, 0x129}, // 3P 4
	{KEY_Z,         3, 0x12A}, // 3P 5
	{KEY_X,         3, 0x12B}, // 3P 6
	{KEY_C,         3, 0x12C}, // 3P 7
	{KEY_V,         3, 0x12D}, // 3P 8

	{KEY_6,         4, 0x120}, // 4P coin
	{KEY_2,         4, 0x121}, // 4P start
	{KEY_R,         4, 0x122}, // 4P up
	{KEY_F,         4, 0x123}, // 4P down
	{KEY_D,         4, 0x124}, // 4P left
	{KEY_G,         4, 0x125}, // 4P right
	{KEY_A,         4, 0x126}, // 4P 1
	{KEY_S,         4, 0x127}, // 4P 2
	{KEY_Q,         4, 0x128}, // 4P 3
	{KEY_W,         4, 0x129}, // 4P 4
	{KEY_I,         4, 0x12A}, // 4P 5
	{KEY_K,         4, 0x12B}, // 4P 6
	{KEY_J,         4, 0x12C}, // 4P 7
	{KEY_L,         4, 0x12D}, // 4P 8
};

const int jamma22joy_count = sizeof(jamma22joy) / sizeof(jamma22joy[0]);
