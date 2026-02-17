#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <sys/sysinfo.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <time.h>
#include <stdarg.h>
#include <math.h>

#include "input.h"
#include "input/input_internal.h"
#include "autofire.h"
#include "file_io.h"
#include "user_io.h"
#include "menu.h"
#include "hardware.h"
#include "cfg.h"
#include "fpga_io.h"
#include "osd.h"
#include "video.h"
#include "audio.h"
#include "joymapping.h"
#include "support.h"
#include "profiling.h"
#include "gamecontroller_db.h"
#include "str_util.h"
#include "frame_timer.h"
#include "spi.h"

char joy_bnames[NUMBUTTONS][32] = {};
int  joy_bcount = 0;
struct pollfd pool[NUMDEV + 3];

/* ========================================================================
 *  Keyboard modifier tracking
 * ======================================================================== */

static uint32_t modifier = 0;

/* Shared state (non-static, declared extern in input_internal.h) */
devInput input[NUMDEV] = {};
int grabbed = 0;

/* Return the currently-held keyboard modifiers (Ctrl/Shift/Alt/GUI). */
uint32_t get_key_mod()
{
	return modifier & MODMASK;
}

/* ========================================================================
 *  Keyboard LED state (Num/Caps/Scroll Lock)
 * ======================================================================== */


static char leds_state = 0;
void set_kbdled(int mask, int state)
{
	leds_state = state ? leds_state | (mask&HID_LED_MASK) : leds_state & ~(mask&HID_LED_MASK);
}

int get_kbdled(int mask)
{
	return (leds_state & (mask&HID_LED_MASK)) ? 1 : 0;
}

int toggle_kbdled(int mask)
{
	int state = !get_kbdled(mask);
	set_kbdled(mask, state);
	return state;
}

static int sysled_is_enabled = 1;
void sysled_enable(int en)
{
	sysled_is_enabled = en;
}

/* ========================================================================
 *  Mapping-mode state machine
 *
 *  When the user enters button-mapping mode from the OSD, these variables
 *  track which device, which button index, and whether we are configuring
 *  the default (menu) map, a per-core map, a keyboard map, or a joy-to-key
 *  map.  See start_map_setting() / finish_map_setting().
 * ======================================================================== */

int mapping = 0;
int mapping_button;
int mapping_dev = -1;
int mapping_type;
static int mapping_count;
static int mapping_clear;
static int mapping_finish;
int mapping_set;

static int mapping_current_key = 0;
static int mapping_current_dev = -1;

static uint32_t tmp_axis[4];
static int tmp_axis_n = 0;


void start_map_setting(int cnt, int set)
{
	mapping_current_key = 0;
	mapping_current_dev = -1;

	mapping_button = 0;
	mapping = 1;
	mapping_set = set;
	if (!mapping_set)
	{
		mapping_dev = -1;
		mapping_type = (cnt < 0) ? 3 : cnt ? 1 : 2;
	}
	mapping_count = cnt;
	mapping_clear = 0;
	mapping_finish = 0;
	tmp_axis_n = 0;

	if (mapping_type <= 1 && is_menu()) mapping_button = -6;
	memset(tmp_axis, 0, sizeof(tmp_axis));

	//un-stick the enter key
	user_io_kbd(KEY_ENTER, 0);
}

int get_map_set()
{
	return mapping_set;
}

int get_map_button()
{
	return mapping_button;
}

int get_map_type()
{
	return mapping_type;
}

int get_map_clear()
{
	return mapping_clear;
}

int get_map_finish()
{
	return mapping_finish;
}

uint32_t osd_timer = 0;
int get_map_cancel()
{
	return (mapping && !is_menu() && osd_timer && CheckTimer(osd_timer));
}


void finish_map_setting(int dismiss)
{
	mapping = 0;
	if (mapping_dev<0) return;

	if (mapping_type == 2)
	{
		input[mapping_dev].has_kbdmap = 0;
		if (dismiss) FileDeleteConfig(get_kbdmap_name(mapping_dev));
		else FileSaveConfig(get_kbdmap_name(mapping_dev), &input[mapping_dev].kbdmap, sizeof(input[mapping_dev].kbdmap));
	}
	else if (mapping_type == 3)
	{
		if (dismiss) memset(input[mapping_dev].jkmap, 0, sizeof(input[mapping_dev].jkmap));
		save_map(get_jkmap_name(mapping_dev), &input[mapping_dev].jkmap, sizeof(input[mapping_dev].jkmap));
	}
	else
	{
		for (int i = 0; i < NUMDEV; i++)
		{
			input[i].has_map = 0;
			input[i].has_mmap = 0;
		}

		if (!dismiss) save_map(get_map_name(mapping_dev, 0), &input[mapping_dev].map, sizeof(input[mapping_dev].map));
		if (dismiss == 2) delete_map(get_map_name(mapping_dev, 0));
	}
}


uint16_t get_map_vid()
{
	return (mapping && mapping_dev >= 0) ? input[mapping_dev].vid : 0;
}

uint16_t get_map_pid()
{
	return (mapping && mapping_dev >= 0) ? input[mapping_dev].pid : 0;
}

int has_default_map()
{
	return (mapping_dev >= 0) ? (input[mapping_dev].has_mmap == 1) : 0;
}

void input_cb(struct input_event *ev, struct input_absinfo *absinfo, int dev);

/* ========================================================================
 *  Mouse / emulated-mouse state
 *
 *  MiSTer can aggregate real mice *and* gamepad-emulated mouse movement
 *  into a single stream sent to the FPGA core each frame.
 * ======================================================================== */

int kbd_toggle = 0;
static uint32_t crtgun_timeout[NUMDEV] = {};

unsigned char mouse_btn = 0; //emulated mouse
unsigned char mice_btn = 0;
static int mouse_req = 0;
static int mouse_x = 0;
static int mouse_y = 0;
static int mouse_w = 0;
int mouse_emu = 0;
static int kbd_mouse_emu = 0;
int mouse_sniper = 0;
int mouse_emu_x = 0;
int mouse_emu_y = 0;

uint32_t mouse_timer = 0;

/* ========================================================================
 *  uinput virtual keyboard  (relays key events to Linux when ungrabbed)
 * ======================================================================== */

static int uinp_fd = -1;
static int input_uinp_setup()
{
	if (uinp_fd <= 0)
	{
		struct uinput_user_dev uinp;

		if (!(uinp_fd = open("/dev/uinput", O_WRONLY | O_NDELAY | O_CLOEXEC)))
		{
			printf("Unable to open /dev/uinput\n");
			uinp_fd = -1;
			return 0;
		}

		memset(&uinp, 0, sizeof(uinp));
		strncpy(uinp.name, UINPUT_NAME, UINPUT_MAX_NAME_SIZE);
		uinp.id.version = 4;
		uinp.id.bustype = BUS_USB;

		ioctl(uinp_fd, UI_SET_EVBIT, EV_KEY);
		for (int i = 0; i < 256; i++) ioctl(uinp_fd, UI_SET_KEYBIT, i);

		write(uinp_fd, &uinp, sizeof(uinp));
		if (ioctl(uinp_fd, UI_DEV_CREATE))
		{
			printf("Unable to create UINPUT device.");
			close(uinp_fd);
			uinp_fd = -1;
			return 0;
		}
	}
	return 1;
}

void input_uinp_destroy()
{
	if (uinp_fd > 0)
	{
		ioctl(uinp_fd, UI_DEV_DESTROY);
		close(uinp_fd);
		uinp_fd = -1;
	}
}

static unsigned long uinp_repeat = 0;
static struct input_event uinp_ev;
void uinp_send_key(uint16_t key, int press)
{
	if (uinp_fd > 0)
	{
		if (!uinp_ev.value && press)
		{
			uinp_repeat = GetTimer(REPEATDELAY);
		}

		memset(&uinp_ev, 0, sizeof(uinp_ev));
		gettimeofday(&uinp_ev.time, NULL);
		uinp_ev.type = EV_KEY;
		uinp_ev.code = key;
		uinp_ev.value = press;
		write(uinp_fd, &uinp_ev, sizeof(uinp_ev));

		static struct input_event ev;
		ev.time = uinp_ev.time;
		ev.type = EV_SYN;
		ev.code = SYN_REPORT;
		ev.value = 0;
		write(uinp_fd, &ev, sizeof(ev));
	}
}

static void uinp_check_key()
{
	if (uinp_fd > 0)
	{
		if (!grabbed)
		{
			if (uinp_ev.value && CheckTimer(uinp_repeat))
			{
				uinp_repeat = GetTimer(REPEATRATE);
				uinp_send_key(uinp_ev.code, 2);
			}
		}
		else
		{
			if (uinp_ev.value)
			{
				uinp_send_key(uinp_ev.code, 0);
			}
		}
	}
}

void mouse_cb(int16_t x, int16_t y, int16_t w)
{
	if (grabbed)
	{
		mouse_x += x;
		mouse_y += y;
		mouse_w += w;
		mouse_req |= 1;
	}
}

void mouse_btn_req()
{
	if (grabbed) mouse_req |= 2;
}

/* ========================================================================
 *  input_cb  -  core input event callback
 *
 *  Called for every EV_KEY / EV_ABS / EV_REL event from every open device.
 *  Responsibilities:
 *    - keyboard scancode translation & forwarding to the FPGA core
 *    - gamepad button/axis -> player mapping (via joy_digital / joy_analog)
 *    - mapping-mode intercept (records button assignments)
 *    - OSD navigation via gamepad
 *    - mouse-emulation via d-pad/sticks
 * ======================================================================== */

void input_cb(struct input_event *ev, struct input_absinfo *absinfo, int dev)
{
	if (ev->type != EV_KEY && ev->type != EV_ABS && ev->type != EV_REL) return;
	if (ev->type == EV_KEY && (!ev->code || ev->code == KEY_UNKNOWN)) return;

	static uint16_t last_axis = 0;

	int sub_dev = dev;

	//check if device is a part of multifunctional device
	if (!JOYCON_COMBINED(dev) && input[dev].bind >= 0) dev = input[dev].bind;

	if (ev->type == EV_KEY)
	{
		if (input[dev].timeout > 0) input[dev].timeout = cfg.bt_auto_disconnect * 10;

		//mouse
		//if the lightgun mouse quirk is set then don't pass these button presses to the mouse system
		//let them get handled and mapped like regular buttons
		if (ev->code >= BTN_MOUSE && ev->code < BTN_JOYSTICK && input[dev].quirk != QUIRK_LIGHTGUN_MOUSE)
		{
			if (ev->value <= 1)
			{
				int mask = 1 << (ev->code - BTN_MOUSE);
				if (input[dev].ds_mouse_emu && mask == 1) mask = 2;
				mice_btn = (ev->value) ? (mice_btn | mask) : (mice_btn & ~mask);
				mouse_btn_req();
			}
			return;
		}
	}

	if (ev->type == EV_KEY && ev->code < 256 && !(mapping && mapping_type == 2))
	{
		if (!input[dev].has_kbdmap)
		{
			if (!FileLoadConfig(get_kbdmap_name(dev), &input[dev].kbdmap, sizeof(input[dev].kbdmap)))
			{
				memset(input[dev].kbdmap, 0, sizeof(input[dev].kbdmap));
			}
			input[dev].has_kbdmap = 1;
		}

		if (input[dev].kbdmap[ev->code]) ev->code = input[dev].kbdmap[ev->code];
	}

	static int key_mapped = 0;

	int map_skip = (ev->type == EV_KEY && mapping && ((ev->code == KEY_SPACE && mapping_type == 1) || ev->code == KEY_ALTERASE) && (mapping_dev >= 0 || mapping_button<0));
	int cancel   = (ev->type == EV_KEY && ev->code == KEY_ESC && !(mapping && mapping_type == 3 && mapping_button));
	int enter    = (ev->type == EV_KEY && ev->code == KEY_ENTER && !(mapping && mapping_type == 3 && mapping_button));
	int origcode = ev->code;

	if (!input[dev].has_mmap)
	{
		if (input[dev].quirk == QUIRK_TOUCHGUN)
		{
			memset(input[dev].mmap, 0, sizeof(input[dev].mmap));
			input[dev].mmap[SYS_AXIS_MX] = -1;
			input[dev].mmap[SYS_AXIS_MY] = -1;
			input[dev].mmap[SYS_AXIS_X] = -1;
			input[dev].mmap[SYS_AXIS_Y] = -1;
		}
		else if (input[dev].quirk != QUIRK_PDSP && input[dev].quirk != QUIRK_MSSP)
		{
			if (!load_map(get_map_name(dev, 1), &input[dev].mmap, sizeof(input[dev].mmap)))
			{
				if (!gcdb_map_for_controller(input[sub_dev].bustype, input[sub_dev].vid, input[sub_dev].pid, input[sub_dev].version, pool[sub_dev].fd, input[dev].mmap))
				{
					memset(input[dev].mmap, 0, sizeof(input[dev].mmap));
					memcpy(input[dev].mmap, def_mmap, sizeof(input[dev].mmap));
					//input[dev].has_mmap++;
				}
			} else {
				gcdb_show_string_for_ctrl_map(input[sub_dev].bustype, input[sub_dev].vid, input[sub_dev].pid, input[sub_dev].version, pool[sub_dev].fd, input[sub_dev].name, input[dev].mmap);
			}
			if (!input[dev].mmap[SYS_BTN_OSD_KTGL + 2]) input[dev].mmap[SYS_BTN_OSD_KTGL + 2] = input[dev].mmap[SYS_BTN_OSD_KTGL + 1];

			if (input[dev].quirk == QUIRK_WHEEL)
			{
				input[dev].mmap[SYS_AXIS_MX] = -1;
				input[dev].mmap[SYS_AXIS_MY] = -1;
				input[dev].mmap[SYS_AXIS_X] = -1;
				input[dev].mmap[SYS_AXIS_Y] = -1;
				input[dev].mmap[SYS_AXIS1_X] = -1;
				input[dev].mmap[SYS_AXIS1_Y] = -1;
				input[dev].mmap[SYS_AXIS2_X] = -1;
				input[dev].mmap[SYS_AXIS2_Y] = -1;
			}
			else
			{
				if (input[dev].mmap[SYS_AXIS_X] == input[dev].mmap[SYS_AXIS1_X])
				{
					input[dev].stick_l[0] = SYS_AXIS1_X;
					if ((input[dev].mmap[SYS_AXIS2_X] >> 16) == 2) input[dev].stick_r[0] = SYS_AXIS2_X;
				}
				if (input[dev].mmap[SYS_AXIS_Y] == input[dev].mmap[SYS_AXIS1_Y])
				{
					input[dev].stick_l[1] = SYS_AXIS1_Y;
					if ((input[dev].mmap[SYS_AXIS2_Y] >> 16) == 2) input[dev].stick_r[1] = SYS_AXIS2_Y;
				}
				if (input[dev].mmap[SYS_AXIS_X] == input[dev].mmap[SYS_AXIS2_X])
				{
					input[dev].stick_l[0] = SYS_AXIS2_X;
					if ((input[dev].mmap[SYS_AXIS1_X] >> 16) == 2) input[dev].stick_r[0] = SYS_AXIS1_X;
				}
				if (input[dev].mmap[SYS_AXIS_Y] == input[dev].mmap[SYS_AXIS2_Y])
				{
					input[dev].stick_l[1] = SYS_AXIS2_Y;
					if ((input[dev].mmap[SYS_AXIS1_Y] >> 16) == 2) input[dev].stick_r[1] = SYS_AXIS1_Y;
				}
			}
		}
		input[dev].has_mmap++;
	}

	if (!input[dev].has_map)
	{
		if (input[dev].quirk == QUIRK_PDSP || input[dev].quirk == QUIRK_MSSP)
		{
			memset(input[dev].map, 0, sizeof(input[dev].map));
			input[dev].map[map_paddle_btn()] = 0x120;
		}
		else if (!load_map(get_map_name(dev, 0), &input[dev].map, sizeof(input[dev].map)))
		{
			memset(input[dev].map, 0, sizeof(input[dev].map));
			if (!is_menu())
			{
				if (input[dev].has_mmap == 1)
				{
					// not defined try to guess the mapping
					map_joystick(input[dev].map, input[dev].mmap);
				}
				else
				{
					input[dev].has_map++;
				}
			}
			input[dev].has_map++;
		}
		input[dev].has_map++;
	}

	if (!input[dev].has_jkmap)
	{
		if (!load_map(get_jkmap_name(dev), &input[dev].jkmap, sizeof(input[dev].jkmap)))
		{
			memset(input[dev].jkmap, 0, sizeof(input[dev].jkmap));
		}
		input[dev].has_jkmap = 1;
	}

	if (!input[dev].num)
	{
		bool assign_btn = ((input[dev].quirk == QUIRK_PDSP || input[dev].quirk == QUIRK_MSSP) && (ev->type == EV_REL || ev->type == EV_KEY));
		assign_btn |= (input[dev].quirk == QUIRK_LIGHTGUN_MOUSE && ev->type == EV_KEY && ev->value == 1 && ev->code == BTN_MOUSE);

		if (!assign_btn && ev->type == EV_KEY && ev->value >= 1 && ev->code >= 256)
		{
			for (int i = SYS_BTN_RIGHT; i <= SYS_BTN_START; i++)
			{
				if (ev->code == input[dev].mmap[i]) assign_btn = 1;
			}
		}

		if (assign_btn)
		{
			for (uint8_t i = 0; i < (sizeof(cfg.player_controller) / sizeof(cfg.player_controller[0])); i++)
			{
				for (int n = 0; n < 8; n++)
				{
					if (!cfg.player_controller[i][n][0]) break;

					if (strcasestr(input[dev].id, cfg.player_controller[i][n]))
					{
						assign_player(dev, i + 1, 1);
						break;
					}

					if (strcasestr(input[dev].sysfs, cfg.player_controller[i][n]))
					{
						assign_player(dev, i + 1, 1);
						break;
					}

					if (strcasestr(get_unique_mapping(dev, 1), cfg.player_controller[i][n]))
					{
						assign_player(dev, i + 1, 1);
						break;
					}
				}
			}

			if (!input[dev].num)
			{
				for (uint8_t num = 1; num < NUMDEV + 1; num++)
				{
					int found = 0;
					for (int i = 0; i < NUMDEV; i++)
					{
						if (input[i].quirk != QUIRK_TOUCHGUN)
						{
							// paddles/spinners overlay on top of other gamepad
							if (!((input[dev].quirk == QUIRK_PDSP || input[dev].quirk == QUIRK_MSSP) ^ (input[i].quirk == QUIRK_PDSP || input[i].quirk == QUIRK_MSSP)))
							{
								found = (input[i].num == num);
								if (found) break;
							}
						}
					}

					if (!found)
					{
						assign_player(dev, num);
						break;
					}
				}
			}
		}
	}

	if (!input[dev].map_shown && input[dev].num)
	{
		input[dev].map_shown = 1;
		if (JOYCON_COMBINED(dev)) input[input[dev].bind].map_shown = 1;
		store_player(input[dev].num, dev);

		if (cfg.controller_info)
		{
			if (input[dev].quirk == QUIRK_PDSP || input[dev].quirk == QUIRK_MSSP)
			{
				char str[32];
				sprintf(str, "P%d paddle/spinner", input[dev].num);
				Info(str, cfg.controller_info * 1000);
			}
			else
			{
				map_joystick_show(input[dev].map, input[dev].mmap, input[dev].num);
			}
		}
	}

	int old_combo = input[dev].osd_combo;

	if (ev->type == EV_KEY)
	{
		if (ev->code == input[dev].mmap[SYS_BTN_OSD_KTGL + 2])
		{
			if (ev->value) input[dev].osd_combo |= 2;
			else input[dev].osd_combo &= ~2;
		}

		if (ev->code == input[dev].mmap[SYS_BTN_OSD_KTGL + 1])
		{
			if (ev->value) input[dev].osd_combo |= 1;
			else input[dev].osd_combo &= ~1;
		}
	}

	int osd_event = 0;
	if (old_combo != 3 && input[dev].osd_combo == 3)
	{
		osd_event = 1;
		if (mapping && !is_menu()) osd_timer = GetTimer(1000);
	}
	else if (old_combo == 3 && input[dev].osd_combo != 3)
	{
		osd_event = 2;
		if (mapping && !is_menu())
		{
			if (CheckTimer(osd_timer))
			{
				cancel = 1;
				ev->code = KEY_ESC;
				ev->value = 0;
			}
			else
			{
				map_skip = 1;
				ev->value = 1;
			}
		}
		osd_timer = 0;
	}

	if (mapping && mapping_type == 3)
	{
		if (map_skip)
		{
			mapping_finish = 1;
			ev->value = 0;
		}
		osd_event = 0;
	}

	//mapping
	if (mapping && (mapping_dev >= 0 || ev->value)
		&& !((mapping_type < 2 || !mapping_button) && (cancel || enter))
		&& input[dev].quirk != QUIRK_PDSP
		&& input[dev].quirk != QUIRK_MSSP)
	{
		int idx = 0;
		osdbtn = 0;

		if (is_menu())
		{
			spi_uio_cmd(UIO_KEYBOARD); //ping the Menu core to wakeup
			osd_event = 0;
		}

		// paddle axis - skip from mapping
		if ((ev->type == EV_ABS || ev->type == EV_REL) && (ev->code == 7 || ev->code == 8) && input[dev].quirk != QUIRK_WHEEL) return;

		// protection against joysticks generating 2 codes per button
		if (ev->type == EV_KEY && !(is_menu() && mapping < 2 && mapping_button == SYS_BTN_OSD_KTGL) && !map_skip)
		{
			if (!mapping_current_key)
			{
				if (ev->value == 1)
				{
					mapping_current_key = ev->code;
					mapping_current_dev = dev;
				}
				else return;
			}
			else
			{
				if (ev->value == 0 && mapping_current_key == ev->code && mapping_current_dev == dev)
				{
					mapping_current_key = 0;
				}
				else return;
			}
		}

		if (map_skip) mapping_current_key = 0;

		if (ev->type == EV_KEY && mapping_button>=0 && !osd_event)
		{
			if (mapping_type == 2) // keyboard remap
			{
				if (ev->code < 256)
				{
					if (!mapping_button)
					{
						if (ev->value == 1)
						{
							if (mapping_dev < 0)
							{
								mapping_dev = dev;
								mapping_button = 0;
							}

							if (!mapping_button) mapping_button = ev->code;
							mapping_current_dev = mapping_dev;
						}
					}
					else
					{
						if (ev->value == 0 && mapping_dev >= 0 && mapping_button != ev->code)
						{
							input[mapping_dev].kbdmap[mapping_button] = ev->code;
							mapping_button = 0;
						}
					}
				}
				return;
			}
			else if (mapping_type == 3) // button remap
			{
				if (input[dev].mmap[SYS_BTN_OSD_KTGL] == ev->code ||
					input[dev].mmap[SYS_BTN_OSD_KTGL + 1] == ev->code ||
					input[dev].mmap[SYS_BTN_OSD_KTGL + 2] == ev->code) return;

				if (ev->value == 1 && !mapping_button)
				{
					if (mapping_dev < 0) mapping_dev = dev;
					if (mapping_dev == dev && ev->code < 1024) mapping_button = ev->code;
					mapping_current_dev = mapping_dev;
				}

				if (mapping_dev >= 0 && (ev->code < 256 || mapping_dev == dev) && mapping_button && mapping_button != ev->code)
				{
					if (ev->value == 1)
					{
						// Technically it's hard to map the key to button as keyboards
						// are all the same while joysticks are personalized and numbered.
						input[mapping_dev].jkmap[mapping_button] = ev->code;
						mapping_current_dev = dev;
					}

					if (ev->value == 0) mapping_button = 0;
				}
				return;
			}
			else
			{
				int clear = (ev->code == KEY_F12 || ev->code == KEY_MENU || ev->code == KEY_HOMEPAGE) && !is_menu();
				if (ev->value == 1 && mapping_dev < 0 && !clear)
				{
					mapping_dev = dev;
					mapping_type = (ev->code >= 256) ? 1 : 0;
					key_mapped = 0;
					memset(input[mapping_dev].map, 0, sizeof(input[mapping_dev].map));
				}

				mapping_clear = 0;
				if (mapping_dev >= 0 && !map_skip && (mapping_dev == dev || clear) && mapping_button < (is_menu() ? (mapping_type ? SYS_BTN_CNT_ESC + 1 : SYS_BTN_OSD_KTGL + 1) : mapping_count))
				{
					if (ev->value == 1 && !key_mapped)
					{
						if (is_menu())
						{
							if (mapping_dev == dev && !(!mapping_button && last_axis && ((ev->code == last_axis) || (ev->code == last_axis + 1))))
							{
								if (!mapping_button) memset(input[dev].map, 0, sizeof(input[dev].map));
								input[dev].osd_combo = 0;

								int found = 0;
								if (mapping_button < SYS_BTN_CNT_OK)
								{
									for (int i = (mapping_button >= BUTTON_DPAD_COUNT) ? BUTTON_DPAD_COUNT : 0; i < mapping_button; i++) if (input[dev].map[i] == ev->code) found = 1;
								}

								if (!found || (mapping_button == SYS_BTN_OSD_KTGL && mapping_type))
								{
									if (mapping_button == SYS_BTN_CNT_OK) input[dev].map[SYS_BTN_MENU_FUNC] = ev->code & 0xFFFF;
									else if (mapping_button == SYS_BTN_CNT_ESC) input[dev].map[SYS_BTN_MENU_FUNC] = (ev->code << 16) | input[dev].map[SYS_BTN_MENU_FUNC];
									else if (mapping_button == SYS_BTN_OSD_KTGL)
									{
										input[dev].map[SYS_BTN_OSD_KTGL + mapping_type] = ev->code;
										input[dev].map[SYS_BTN_OSD_KTGL + 2] = input[dev].map[SYS_BTN_OSD_KTGL + 1];
										mapping_current_key = 0; // allow 2 buttons to be pressed
									}
									else input[dev].map[mapping_button] = ev->code;

									key_mapped = ev->code;

									//check if analog stick has been used for mouse
									if (mapping_button == BUTTON_DPAD_COUNT + 1 || mapping_button == BUTTON_DPAD_COUNT + 3)
									{
										if (input[dev].map[mapping_button] >= KEY_EMU &&
											input[dev].map[mapping_button - 1] >= KEY_EMU &&
											(input[dev].map[mapping_button - 1] - input[dev].map[mapping_button] == 1) && // same axis
											absinfo)
										{
											input[dev].map[SYS_AXIS_MX + (mapping_button - (BUTTON_DPAD_COUNT + 1)) / 2] = ((input[dev].map[mapping_button] - KEY_EMU) / 2) | 0x20000;
										}
									}
								}
							}
						}
						else
						{
							if (clear)
							{
								memset(input[mapping_dev].map, 0, sizeof(input[mapping_dev].map));
								mapping_button = 0;
								mapping_clear = 1;
							}
							else
							{
								if (!mapping_button)
								{
									for (uint i = 0; i < sizeof(input[0].map) / sizeof(input[0].map[0]); i++)
									{
										input[dev].map[i] &= mapping_set ? 0x0000FFFF : 0xFFFF0000;
									}
								}

								int found = 0;
								for (int i = 0; i < mapping_button; i++)
								{
									if (mapping_set && (input[dev].map[i] >> 16) == ev->code) found = 1;
									if (!mapping_set && (input[dev].map[i] & 0xFFFF) == ev->code) found = 1;
								}

								if (!found)
								{
									if (mapping_set) input[dev].map[mapping_button] = (input[dev].map[mapping_button] & 0xFFFF) | (ev->code << 16);
									else input[dev].map[mapping_button] = (input[dev].map[mapping_button] & 0xFFFF0000) | ev->code;
									key_mapped = ev->code;
								}
							}
						}
					}
					//combo for osd button
					if (ev->value == 1 && key_mapped && key_mapped != ev->code && is_menu() && mapping_button == SYS_BTN_OSD_KTGL && mapping_type)
					{
						input[dev].map[SYS_BTN_OSD_KTGL + 2] = ev->code;
						printf("Set combo: %x + %x\n", input[dev].map[SYS_BTN_OSD_KTGL + 1], input[dev].map[SYS_BTN_OSD_KTGL + 2]);
					}
					else if(mapping_dev == dev && ev->value == 0 && key_mapped == ev->code)
					{
						mapping_button++;
						key_mapped = 0;
					}

					if(!ev->value && mapping_dev == dev && ((ev->code == last_axis) || (ev->code == last_axis+1)))
					{
						last_axis = 0;
					}
				}
			}
		}
		else if (is_menu())
		{
			//Define min-0-max analogs
			switch (mapping_button)
			{
				case 23: idx = SYS_AXIS_X;  break;
				case 24: idx = SYS_AXIS_Y;  break;
				case -4: idx = SYS_AXIS1_X; break;
				case -3: idx = SYS_AXIS1_Y; break;
				case -2: idx = SYS_AXIS2_X; break;
				case -1: idx = SYS_AXIS2_Y; break;
			}

			if (mapping_dev == dev || (mapping_dev < 0 && mapping_button < 0))
			{
				int max = 0; // , min = 0;

				if (ev->type == EV_ABS)
				{
					int threshold = (absinfo->maximum - absinfo->minimum) / 5;

					max = (ev->value >= (absinfo->maximum - threshold));
					//min = (ev->value <= (absinfo->minimum + threshold));
					//printf("threshold=%d, min=%d, max=%d\n", threshold, min, max);
				}

				//check DPAD horz
				if (mapping_button == -6)
				{
					last_axis = 0;
					if (ev->type == EV_ABS && max)
					{
						if (mapping_dev < 0) mapping_dev = dev;
						mapping_type = 1;

						if (absinfo->maximum > 2)
						{
							tmp_axis[tmp_axis_n++] = ev->code | 0x20000;
							mapping_button++;
						}
						else
						{
							//Standard DPAD event
							mapping_button += 2;
						}
					}
					else if (ev->type == EV_KEY && ev->value == 1)
					{
						//DPAD uses simple button events
						if (!map_skip)
						{
							mapping_button += 2;
							if (mapping_dev < 0) mapping_dev = dev;
							if (ev->code < 256)
							{
								// keyboard, skip stick 1/2
								mapping_button += 4;
								mapping_type = 0;
							}
						}
					}
				}
				//check DPAD vert
				else if (mapping_button == -5)
				{
					if (ev->type == EV_ABS && max && absinfo->maximum > 1 && ev->code != (tmp_axis[0] & 0xFFFF))
					{
						tmp_axis[tmp_axis_n++] = ev->code | 0x20000;
						mapping_button++;
					}
				}
				//Sticks
				else if (ev->type == EV_ABS && idx)
				{
					if (mapping_dev < 0) mapping_dev = dev;

					if (idx && max && absinfo->maximum > 2)
					{
						if (mapping_button < 0)
						{
							int found = 0;
							for (int i = 0; i < tmp_axis_n; i++) if (ev->code == (tmp_axis[i] & 0xFFFF)) found = 1;
							if (!found)
							{
								mapping_type = 1;
								tmp_axis[tmp_axis_n++] = ev->code | 0x20000;
								//if (min) tmp_axis[idx - AXIS1_X] |= 0x10000;
								mapping_button++;
								if (tmp_axis_n >= 4) mapping_button = 0;
								last_axis = KEY_EMU + (ev->code << 1);
							}
						}
						else
						{
							if (idx == SYS_AXIS_X || ev->code != (input[dev].map[idx - 1] & 0xFFFF))
							{
								input[dev].map[idx] = ev->code | 0x20000;
								//if (min) input[dev].map[idx] |= 0x10000;
								mapping_button++;
							}
						}
					}
				}
			}
		}

		while (mapping_type <= 1 && mapping_button < mapping_count)
		{
			if (map_skip)
			{
				if (map_skip == 2 || ev->value == 1)
				{
					if (mapping_dev >= 0)
					{
						if (idx) input[mapping_dev].map[idx] = 0;
						else if (mapping_button > 0)
						{
							if (!is_menu()) input[mapping_dev].map[mapping_button] &= mapping_set ? 0x0000FFFF : 0xFFFF0000;
						}
					}
					last_axis = 0;
					mapping_button++;
					if (mapping_button < 0 && (mapping_button & 1)) mapping_button++;
				}
			}

			map_skip = 0;
			if (mapping_button >= 4 && !is_menu() && !strcmp(joy_bnames[mapping_button - 4], "-")) map_skip = 2;
			if (!map_skip) break;
		}

		if (is_menu() && mapping_type <= 1 && mapping_dev >= 0)
		{
			memcpy(&input[mapping_dev].mmap[SYS_AXIS1_X], tmp_axis, sizeof(tmp_axis));
			memcpy(&input[mapping_dev].map[SYS_AXIS1_X], tmp_axis, sizeof(tmp_axis));
		}
	}
	else
	{
		key_mapped = 0;
		switch (ev->type)
		{
		case EV_KEY:
			if (ev->code < 1024 && input[dev].jkmap[ev->code] && !user_io_osd_is_visible()) ev->code = input[dev].jkmap[ev->code];

			//joystick buttons, digital directions
			if (ev->code >= 256)
			{
				if (input[dev].lightgun_req && !user_io_osd_is_visible())
				{
					if (osd_event == 1)
					{
						input[dev].lightgun = !input[dev].lightgun;
						Info(input[dev].lightgun ? "Light Gun mode is ON" : "Light Gun mode is OFF");
					}
				}
				else
				{
					if (osd_event == 1) joy_digital(input[dev].num, 0, 0, 1, BTN_OSD);
					if (osd_event == 2) joy_digital(input[dev].num, 0, 0, 0, BTN_OSD);
				}

				if (user_io_osd_is_visible() || video_fb_state())
				{
					if (ev->value <= 1)
					{
						if ((input[dev].mmap[SYS_BTN_MENU_FUNC] & 0xFFFF) ?
							(ev->code == (input[dev].mmap[SYS_BTN_MENU_FUNC] & 0xFFFF)) :
							(ev->code == input[dev].mmap[SYS_BTN_A]))
						{
							joy_digital(0, JOY_BTN1, 0, ev->value, 0);
							return;
						}

						if ((input[dev].mmap[SYS_BTN_MENU_FUNC] >> 16) ?
							(ev->code == (input[dev].mmap[SYS_BTN_MENU_FUNC] >> 16)) :
							(ev->code == input[dev].mmap[SYS_BTN_B]))
						{
							joy_digital(0, JOY_BTN2, 0, ev->value, 0);
							return;
						}

						if (ev->code == input[dev].mmap[SYS_BTN_X])
						{
							joy_digital(0, JOY_BTN4, 0, ev->value, 0);
							return;
						}

						if (ev->code == input[dev].mmap[SYS_BTN_Y])
						{
							joy_digital(0, JOY_BTN3, 0, ev->value, 0);
							return;
						}

						if (ev->code == input[dev].mmap[SYS_BTN_L])
						{
							joy_digital(0, JOY_L, 0, ev->value, 0);
							return;
						}

						if (ev->code == input[dev].mmap[SYS_BTN_R])
						{
							joy_digital(0, JOY_R, 0, ev->value, 0);
							return;
						}

						if (ev->code == input[dev].mmap[SYS_BTN_START])
						{
							joy_digital(0, JOY_L2, 0, ev->value, 0);
							return;
						}

						if (ev->code == input[dev].mmap[SYS_BTN_SELECT])
						{
							joy_digital(0, JOY_R2, 0, ev->value, 0);
							return;
						}

						for (int i = 0; i < SYS_BTN_A; i++)
						{
							if (ev->code == input[dev].mmap[i])
							{
								joy_digital(0, 1 << i, 0, ev->value, i);
								return;
							}
						}

						for (int i = SYS_MS_RIGHT; i <= SYS_MS_UP; i++)
						{
							if (ev->code == input[dev].mmap[i])
							{
								int n = i - SYS_MS_RIGHT;
								joy_digital(0, 1 << n, 0, ev->value, n);
								return;
							}
						}

						if (input[dev].quirk != QUIRK_WHEEL)
						{
							if (input[dev].mmap[SYS_AXIS_X])
							{
								uint16_t key = KEY_EMU + ((uint16_t)input[dev].mmap[SYS_AXIS_X] * 2);
								if (ev->code == (key + 1)) joy_digital(0, 1 << 0, 0, ev->value, 0);
								if (ev->code == key) joy_digital(0, 1 << 1, 0, ev->value, 1);
							}

							if (input[dev].mmap[SYS_AXIS_Y])
							{
								uint16_t key = KEY_EMU + ((uint16_t)input[dev].mmap[SYS_AXIS_Y] * 2);
								if (ev->code == (key + 1)) joy_digital(0, 1 << 2, 0, ev->value, 2);
								if (ev->code == key) joy_digital(0, 1 << 3, 0, ev->value, 3);
							}
						}
					}
				}
				else
				{
					if (mouse_emu)
					{
						int use_analog = (input[dev].mmap[SYS_AXIS_MX] || input[dev].mmap[SYS_AXIS_MY]);

						for (int i = (use_analog ? SYS_MS_BTN_L : SYS_MS_RIGHT); i <= SYS_MS_BTN_M; i++)
						{
							if (ev->code == input[dev].mmap[i])
							{
								switch (i)
								{
								case SYS_MS_RIGHT:
									mouse_emu_x = ev->value ? 10 : 0;
									break;
								case SYS_MS_LEFT:
									mouse_emu_x = ev->value ? -10 : 0;
									break;
								case SYS_MS_DOWN:
									mouse_emu_y = ev->value ? 10 : 0;
									break;
								case SYS_MS_UP:
									mouse_emu_y = ev->value ? -10 : 0;
									break;

								default:
									mouse_btn = ev->value ? mouse_btn | 1 << (i - SYS_MS_BTN_L) : mouse_btn & ~(1 << (i - SYS_MS_BTN_L));
									mouse_btn_req();
									break;
								}
								return;
							}
						}
					}

					if (input[dev].has_map >= 2)
					{
						if (input[dev].has_map == 3) Info("This joystick is not defined");
						input[dev].has_map = 1;
					}
					
					for (uint i = 0; i < BTN_NUM; i++)
					{
						if (ev->code == (input[dev].map[i] & 0xFFFF) || ev->code == (input[dev].map[i] >> 16)) {
							if (ev->value <= 1) joy_digital(input[dev].num, 1 << i, origcode, ev->value, i, (ev->code == input[dev].mmap[SYS_BTN_OSD_KTGL + 1] || ev->code == input[dev].mmap[SYS_BTN_OSD_KTGL + 2]));
						}
					}

					if (ev->code == input[dev].mmap[SYS_MS_BTN_EMU] && (ev->value <= 1) && ((!(mouse_emu & 1)) ^ (!ev->value)))
					{
						mouse_emu = ev->value ? mouse_emu | 1 : mouse_emu & ~1;
						if (input[sub_dev].quirk == QUIRK_DS4) input[dev].ds_mouse_emu = mouse_emu & 1;
						if (mouse_emu & 2)
						{
							mouse_sniper = ev->value;
						}
						else
						{
							mouse_timer = 0;
							mouse_btn = 0;
							mouse_emu_x = 0;
							mouse_emu_y = 0;
							mouse_cb();
							mouse_btn_req();
						}
					}
				}
				return;
			}
			// keyboard
			else
			{
				//  replace MENU key by RGUI to allow using Right Amiga on reduced keyboards
				// (it also disables the use of Menu for OSD)
				if (cfg.key_menu_as_rgui && ev->code == KEY_COMPOSE) ev->code = KEY_RIGHTMETA;

				//Keyrah v2: USB\VID_18D8&PID_0002\A600/A1200_MULTIMEDIA_EXTENSION_VERSION
				int keyrah = (cfg.keyrah_mode && (((((uint32_t)input[dev].vid) << 16) | input[dev].pid) == cfg.keyrah_mode));
				if (keyrah) ev->code = keyrah_trans(ev->code, ev->value);

				uint32_t ps2code = get_ps2_code(ev->code);
				if (ev->value) modifier |= ps2code;
				else modifier &= ~ps2code;

				uint16_t reset_m = (modifier & MODMASK) >> 8;
				if (ev->code == 111) reset_m |= 0x100;
				user_io_check_reset(reset_m, (keyrah && !cfg.reset_combo) ? 1 : cfg.reset_combo);

				if(!user_io_osd_is_visible() && ((user_io_get_kbdemu() == EMU_JOY0) || (user_io_get_kbdemu() == EMU_JOY1)) && !video_fb_state())
				{
					if (!kbd_toggle)
					{
						for (uint i = 0; i < BTN_NUM; i++)
						{
							if (ev->code == (uint16_t)input[dev].map[i])
							{
								if (ev->value <= 1) joy_digital((user_io_get_kbdemu() == EMU_JOY0) ? 1 : 2, 1 << i, origcode, ev->value, i);
								return;
							}
						}
						// if we move the return above to here, it should change behavior so that joy_digital() is called once for every
						// button that is mapped to a given code. each call would have the same ev->code but a different mask and bnum.
						// no time to test now though so leaving as note to anybody else who wants to try it. current system can't even
						// generate an ev->code with multiple buttons
						// return;
					}

					if (ev->code == input[dev].mmap[SYS_BTN_OSD_KTGL])
					{
						if (ev->value <= 1) joy_digital((user_io_get_kbdemu() == EMU_JOY0) ? 1 : 2, 0, 0, ev->value, BTN_TGL);
						return;
					}
				}
				else
				{
					kbd_toggle = 0;
				}

				if (!user_io_osd_is_visible() && (user_io_get_kbdemu() == EMU_MOUSE))
				{
					if (kbd_mouse_emu)
					{
						for (int i = SYS_MS_RIGHT; i <= SYS_MS_BTN_M; i++)
						{
							if (ev->code == input[dev].mmap[i])
							{
								switch (i)
								{
								case SYS_MS_RIGHT:
									mouse_emu_x = ev->value ? 10 : 0;
									break;
								case SYS_MS_LEFT:
									mouse_emu_x = ev->value ? -10 : 0;
									break;
								case SYS_MS_DOWN:
									mouse_emu_y = ev->value ? 10 : 0;
									break;
								case SYS_MS_UP:
									mouse_emu_y = ev->value ? -10 : 0;
									break;

								default:
									mouse_btn = ev->value ? mouse_btn | 1 << (i - SYS_MS_BTN_L) : mouse_btn & ~(1 << (i - SYS_MS_BTN_L));
									mouse_btn_req();
									break;
								}
								return;
							}
						}

						if (ev->code == input[dev].mmap[SYS_MS_BTN_EMU])
						{
							if (ev->value <= 1) mouse_sniper = ev->value;
							return;
						}
					}

					if (ev->code == input[dev].mmap[SYS_BTN_OSD_KTGL])
					{
						if (ev->value == 1)
						{
							kbd_mouse_emu = !kbd_mouse_emu;
							printf("kbd_mouse_emu = %d\n", kbd_mouse_emu);

							mouse_timer = 0;
							mouse_btn = 0;
							mouse_emu_x = 0;
							mouse_emu_y = 0;
							mouse_cb();
							mouse_btn_req();
						}
						return;
					}
				}

				if (ev->code == KEY_HOMEPAGE) ev->code = KEY_MENU;
				user_io_kbd(ev->code, ev->value);
				return;
			}
			break;

		//analog joystick
		case EV_ABS:
			if (!user_io_osd_is_visible())
			{
				int value = ev->value;
				if (ev->value < absinfo->minimum) value = absinfo->minimum;
				else if (ev->value > absinfo->maximum) value = absinfo->maximum;

				if (ev->code == 8 && input[dev].quirk != QUIRK_WHEEL)
				{
					if (input[dev].num && input[dev].num <= NUMPLAYERS)
					{
						value -= absinfo->minimum;
						value = (value * 255) / (absinfo->maximum - absinfo->minimum);
						user_io_l_analog_joystick(((input[dev].num - 1) << 4) | 0xF, value, 0);
					}
					break;
				}

				int hrange = (absinfo->maximum - absinfo->minimum) / 2;

				// normalize to -range/2...+range/2
				value -= (absinfo->minimum + absinfo->maximum) / 2;

				int range = is_psx() ? 128 : 127;
				value = (value * range) / hrange;

				// final check to eliminate additive error
				if (value < -range) value = -range;
				else if (value > 127) value = 127;

				if (input[sub_dev].axis_pos[ev->code & 0xFF] == (int8_t)value) break;
				input[sub_dev].axis_pos[ev->code & 0xFF] = (int8_t)value;

				if (ev->code == (input[dev].mmap[SYS_AXIS_MX] & 0xFFFF) && mouse_emu)
				{
					mouse_emu_x = 0;
					if (value < -1 || value > 1) mouse_emu_x = value;
					mouse_emu_x /= 12;
					return;
				}
				else if (ev->code == (input[dev].mmap[SYS_AXIS_MY] & 0xFFFF) && mouse_emu)
				{
					mouse_emu_y = 0;
					if (value < -1 || value > 1) mouse_emu_y = value;
					mouse_emu_y /= 12;
					return;
				}
				else
				{
					// skip if joystick is undefined.
					if (!input[dev].num) break;

					if (input[dev].quirk == QUIRK_WHEEL)
					{
						int wh_value = ((127 * (ev->value - absinfo->minimum)) / (absinfo->maximum - absinfo->minimum)) - 127;
						if (input[dev].wh_pedal_invert > 0) {
							// invert pedal values range for wheel setups that require it
							wh_value = ~(wh_value + 127);
						}

						// steering wheel passes full range, pedals are standardised in +127 to 0 to -127 range
						if (ev->code == input[dev].wh_steer)
						{
							joy_analog(dev, 0, value, 0);
						}
						else if (ev->code == input[dev].wh_accel)
						{
							joy_analog(dev, 1, wh_value, 0);
						}
						else if (ev->code == input[dev].wh_brake)
						{
							joy_analog(dev, 1, wh_value, 1);
						}
						else if (ev->code == input[dev].wh_clutch)
						{
							joy_analog(dev, 0, wh_value, 1);
						}
						else if (ev->code == input[dev].wh_combo)
						{
							// if accel and brake pedal use a shared axis then map negative to accel and positive to brake
							if (value < -1) joy_analog(dev, 1, value, 0);
							else if (value > 1) joy_analog(dev, 1, -value, 1);
							else
							{
								joy_analog(dev, 1, 0, 0);
								joy_analog(dev, 1, 0, 0);
							}
						}
					}
					else if (ev->code == 0 && input[dev].lightgun)
					{
						joy_analog(dev, 0, value);
					}
					else if (ev->code == 1 && input[dev].lightgun)
					{
						joy_analog(dev, 1, value);
					}
					else
					{
						int offset = (value < -1 || value > 1) ? value : 0;
						if (input[dev].stick_l[0] && ev->code == (uint16_t)input[dev].mmap[input[dev].stick_l[0]])
						{
							joy_analog(dev, 0, offset, 0);
						}
						else if (input[dev].stick_l[1] && ev->code == (uint16_t)input[dev].mmap[input[dev].stick_l[1]])
						{
							joy_analog(dev, 1, offset, 0);
						}
						else if (input[dev].stick_r[0] && ev->code == (uint16_t)input[dev].mmap[input[dev].stick_r[0]])
						{
							joy_analog(dev, 0, offset, 1);
						}
						else if (input[dev].stick_r[1] && ev->code == (uint16_t)input[dev].mmap[input[dev].stick_r[1]])
						{
							joy_analog(dev, 1, offset, 1);
						}
					}
				}
			}
			break;

		// spinner
		case EV_REL:
			if (!user_io_osd_is_visible() && ev->code == 7)
			{
				if (input[dev].num && input[dev].num <= NUMPLAYERS)
				{
					int value = ev->value;
					if (ev->value < -128) value = -128;
					else if (ev->value > 127) value = 127;

					user_io_l_analog_joystick(((input[dev].num - 1) << 4) | 0x8F, value, 0);
				}
			}
			break;
		}
	}
}

/* Send a synthetic key event into the mapping-mode state machine. */
void send_map_cmd(int key)
{
	if (mapping && mapping_dev >= 0)
	{
		input_event ev;
		ev.type = EV_KEY;
		ev.code = key;
		ev.value = 1;
		input_cb(&ev, 0, mapping_dev);
	}
}

#define CMD_FIFO "/dev/MiSTer_cmd"
#define LED_MONITOR "/sys/class/leds/hps_led0/brightness_hw_changed"

/* ========================================================================
 *  input_test  -  main device polling & event dispatch loop
 *
 *  Opens evdev nodes, detects hotplug via inotify, reads events from all
 *  devices via poll(), and dispatches them through input_cb().  Also
 *  handles device identification, quirk detection, Bluetooth timeouts,
 *  serial CRT light-gun data, and /dev/MiSTer_cmd FIFO commands.
 *
 *  Returns the next OSD character when getchar != 0, or 0 otherwise.
 * ======================================================================== */

int input_test(int getchar)
{
	static char cur_leds = 0;
	static int state = 0;
	struct input_absinfo absinfo;
	struct input_event ev;
	static uint32_t timeout = 0;

	if (touch_rel && CheckTimer(touch_rel))
	{
		touch_rel = 0;
		mice_btn = 0;
		mouse_btn_req();
	}

	if (state == 0)
	{
		input_uinp_setup();
		memset(pool, -1, sizeof(pool));

		signal(SIGINT, INThandler);
		pool[NUMDEV].fd = set_watch();
		pool[NUMDEV].events = POLLIN;

		unlink(CMD_FIFO);
		mkfifo(CMD_FIFO, 0666);

		pool[NUMDEV+1].fd = open(CMD_FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		pool[NUMDEV+1].events = POLLIN;

		pool[NUMDEV + 2].fd = open(LED_MONITOR, O_RDONLY | O_CLOEXEC);
		pool[NUMDEV + 2].events = POLLPRI;

		state++;
	}

	if (state == 1)
	{
		timeout = 0;
		printf("Open up to %d input devices.\n", NUMDEV);
		for (int i = 0; i < NUMDEV; i++)
		{
			pool[i].fd = -1;
			pool[i].events = 0;
		}

		// clear button reference counts and key states
		memset(key_states, 0, sizeof(KeyStates) * NUMPLAYERS);
		for (int i = 0; i < NUMPLAYERS; i++) {
			clear_autofire(i);
		}

		memset(input, 0, sizeof(input));

		int n = 0;
		DIR *d = opendir("/dev/input");
		if (d)
		{
			struct dirent *de;
			while ((de = readdir(d)))
			{
				if (!strncmp(de->d_name, "event", 5) || !strncmp(de->d_name, "mouse", 5))
				{
					memset(&input[n], 0, sizeof(input[n]));
					sprintf(input[n].devname, "/dev/input/%s", de->d_name);
					int fd = open(input[n].devname, O_RDWR | O_CLOEXEC);
					//printf("open(%s): %d\n", input[n].devname, fd);

					if (fd > 0)
					{
						pool[n].fd = fd;
						pool[n].events = POLLIN;
						input[n].mouse = !strncmp(de->d_name, "mouse", 5);

						char uniq[32] = {};
						if (!input[n].mouse)
						{
							struct input_id id;
							memset(&id, 0, sizeof(id));
							ioctl(pool[n].fd, EVIOCGID, &id);
							input[n].vid = id.vendor;
							input[n].pid = id.product;
							input[n].version = id.version;
							input[n].bustype = id.bustype;

							ioctl(pool[n].fd, EVIOCGUNIQ(sizeof(uniq)), uniq);
							ioctl(pool[n].fd, EVIOCGNAME(sizeof(input[n].name)), input[n].name);
							input[n].led = has_led(pool[n].fd);
						}

						//skip our virtual device
						if (!strcmp(input[n].name, UINPUT_NAME))
						{
							close(pool[n].fd);

							pool[n].fd = -1;
							continue;
						}

						input[n].bind = -1;

						int effects;
						input[n].has_rumble = false;
						if (cfg.rumble)
						{
							if (ioctl(fd, EVIOCGEFFECTS, &effects) >= 0)
							{
								unsigned char ff_features[(FF_MAX + 7) / 8] = {};

								if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff_features)), ff_features) != -1)
								{
									if (test_bit(FF_RUMBLE, ff_features)) {
										input[n].rumble_effect.id = -1;
										input[n].has_rumble = true;
									}
								}
							}
						}

						// enable scroll wheel reading
						if (input[n].mouse)
						{
							unsigned char buffer[4];
							static const unsigned char mousedev_imps_seq[] = { 0xf3, 200, 0xf3, 100, 0xf3, 80 };
							if (write(pool[n].fd, mousedev_imps_seq, sizeof(mousedev_imps_seq)) != sizeof(mousedev_imps_seq))
							{
								printf("Cannot switch %s to ImPS/2 protocol(1)\n", input[n].devname);
							}
							else if (read(pool[n].fd, buffer, sizeof buffer) != 1 || buffer[0] != 0xFA)
							{
								printf("Failed to switch %s to ImPS/2 protocol(2)\n", input[n].devname);
							}
						}

						// RasPad3 touchscreen
						if (input[n].vid == 0x222a && input[n].pid == 1)
						{
							input[n].quirk = QUIRK_TOUCHGUN;
							input[n].num = 1;
							input[n].map_shown = 1;

							input[n].lightgun = 0;
							input[n].guncal[0] = 0;
							input[n].guncal[1] = 16383;
							input[n].guncal[2] = 2047;
							input[n].guncal[3] = 14337;
							input_lightgun_load(n);
						}

						if (input[n].vid == 0x054c)
						{
							if (strcasestr(input[n].name, "Motion"))
							{
								// don't use Accelerometer
								close(pool[n].fd);
								pool[n].fd = -1;
								continue;
							}

							if (input[n].pid == 0x0268)  input[n].quirk = QUIRK_DS3;
							else if (input[n].pid == 0x05c4 || input[n].pid == 0x09cc || input[n].pid == 0x0ba0 || input[n].pid == 0x0ce6)
							{
								input[n].quirk = QUIRK_DS4;
								if (strcasestr(input[n].name, "Touchpad"))
								{
									input[n].quirk = QUIRK_DS4TOUCH;
								}
							}
						}

						if (input[n].vid == 0x0079 && input[n].pid == 0x1802)
						{
							input[n].lightgun = 1;
							input[n].num = 2; // force mayflash mode 1/2 as second joystick.
						}

						if (input[n].vid == 0x057e && (input[n].pid == 0x0306 || input[n].pid == 0x0330))
						{
							if (strcasestr(input[n].name, "Accelerometer"))
							{
								// don't use Accelerometer
								close(pool[n].fd);
								pool[n].fd = -1;
								continue;
							}
							else if (strcasestr(input[n].name, "Motion Plus"))
							{
								// don't use Accelerometer
								close(pool[n].fd);
								pool[n].fd = -1;
								continue;
							}
							else if (!strcasestr(input[n].name, "Pro Controller"))
							{
								input[n].quirk = QUIRK_WIIMOTE;
								input[n].guncal[0] = 0;
								input[n].guncal[1] = 767;
								input[n].guncal[2] = 1;
								input[n].guncal[3] = 1023;
								input_lightgun_load(n);
							}
						}

						if (input[n].vid == 0x057e)
						{
							if (strstr(input[n].name, " IMU"))
							{
								// don't use Accelerometer
								close(pool[n].fd);
								pool[n].fd = -1;
								continue;
							}
						}

						if (input[n].vid == 0x057e && input[n].pid == 0x2006)
						{
							input[n].misc_flags = 1 << 30;
							input[n].quirk = QUIRK_JOYCON;
						}
						if (input[n].vid == 0x057e && input[n].pid == 0x2007)
						{
							input[n].misc_flags = 1 << 29;
							input[n].quirk = QUIRK_JOYCON;
						}

						//Ultimarc lightgun
						if (input[n].vid == 0xd209 && input[n].pid == 0x1601)
						{
							input[n].lightgun = 1;
						}

						//Namco Guncon via Arduino, RetroZord or Reflex Adapt
						if (((input[n].vid == 0x2341 || (input[n].vid == 0x1209 && input[n].pid == 0x595A)) && (strstr(uniq, "RZordPsGun") || strstr(input[n].name, "RZordPsGun"))) ||
							(input[n].vid == 0x16D0 && input[n].pid == 0x127E && (strstr(uniq, "ReflexPSGun") || strstr(input[n].name, "ReflexPSGun"))))
						{
							input[n].quirk = QUIRK_LIGHTGUN;
							input[n].lightgun = 1;
							input[n].guncal[0] = 0;
							input[n].guncal[1] = 32767;
							input[n].guncal[2] = 0;
							input[n].guncal[3] = 32767;
							input_lightgun_load(n);
						}

						//Namco GunCon 2
						if (input[n].vid == 0x0b9a && input[n].pid == 0x016a)
						{
							input[n].quirk = QUIRK_LIGHTGUN_CRT;
							input[n].lightgun = 1;
							input[n].guncal[0] = 25;
							input[n].guncal[1] = 245;
							input[n].guncal[2] = 145;
							input[n].guncal[3] = 700;
							input_lightgun_load(n);
						}

						//Namco GunCon 3
						if (input[n].vid == 0x0b9a && input[n].pid == 0x0800)
						{
							input[n].quirk = QUIRK_LIGHTGUN;
							input[n].lightgun = 1;
							input[n].guncal[0] = -32768;
							input[n].guncal[1] = 32767;
							input[n].guncal[2] = -32768;
							input[n].guncal[3] = 32767;
							input_lightgun_load(n);
						}

						//GUN4IR Lightgun
						if (input[n].vid == 0x2341 && input[n].pid >= 0x8042 && input[n].pid <= 0x8049)
						{
							input[n].quirk = QUIRK_LIGHTGUN;
							input[n].lightgun = 1;
							input[n].guncal[0] = 0;
							input[n].guncal[1] = 32767;
							input[n].guncal[2] = 0;
							input[n].guncal[3] = 32767;
							input_lightgun_load(n);
						}

						//OpenFIRE Lightgun
						//!Note that OF has a user-configurable PID, but the VID is reserved and every device name has the prefix "OpenFIRE"
						if (input[n].vid == 0xf143 && strstr(input[n].name, "OpenFIRE "))
						{
							// OF generates 3 devices, so just focus on the one actual gamepad slot.
							char *nameInit = input[n].name;
							if(memcmp(nameInit+strlen(input[n].name)-5, "Mouse", 5) != 0 && memcmp(nameInit+strlen(input[n].name)-8, "Keyboard", 8) != 0)
							{
								input[n].quirk = QUIRK_LIGHTGUN;
								input[n].lightgun = 1;
								input[n].guncal[0] = -32767;
								input[n].guncal[1] = 32767;
								input[n].guncal[2] = -32767;
								input[n].guncal[3] = 32767;
								input_lightgun_load(n);
							}
						}

						//Blamcon Lightgun
						if (input[n].vid == 0x3673 && ((input[n].pid >= 0x0100 && input[n].pid <= 0x0103) || (input[n].pid >= 0x0200 && input[n].pid <= 0x0203)))
						{
							input[n].quirk = QUIRK_LIGHTGUN;
							input[n].lightgun = 1;
							input[n].guncal[0] = 0;
							input[n].guncal[1] = 32767;
							input[n].guncal[2] = 0;
							input[n].guncal[3] = 32767;
							input_lightgun_load(n);
						}

						//Retroshooter
						if (input[n].vid == 0x0483 && input[n].pid >= 0x5750 && input[n].pid <= 0x5753)
						{
							input[n].quirk = QUIRK_LIGHTGUN_MOUSE;
							input[n].lightgun = 1;
							input[n].guncal[0] = 0;
							input[n].guncal[1] = 767;
							input[n].guncal[2] = 0;
							input[n].guncal[3] = 1023;
							input_lightgun_load(n);
						}

						//Sinden Lightgun (two different PIDs, four different PIDs depending on gun color/config)                                                                                                   
						if ((input[n].vid == 0x16c0 || input[n].vid == 0x16d0) && (                             
            					input[n].pid == 0x0f01 ||                             
            					input[n].pid == 0x0f02 ||                             
            					input[n].pid == 0x0f38 ||                             
            					input[n].pid == 0x0f39))                             
						{                             
    							input[n].quirk = QUIRK_LIGHTGUN;                             
    							input[n].lightgun = 1;                             
    							input[n].guncal[0] = 0;                             
    							input[n].guncal[1] = 65535;                             
    							input[n].guncal[2] = 0;                             
    							input[n].guncal[3] = 65535;                             
    							input_lightgun_load(n);                             
						} 

						//Madcatz Arcade Stick 360
						if (input[n].vid == 0x0738 && input[n].pid == 0x4758) input[n].quirk = QUIRK_MADCATZ360;

						// mr.Spinner
						// 0x120  - Button
						// Axis 7 - EV_REL is spinner
						// Axis 8 - EV_ABS is Paddle
						// Overlays on other existing gamepads
						if (strstr(uniq, "MiSTer-S1")) input[n].quirk = QUIRK_PDSP;
						if (strstr(input[n].name, "MiSTer-S1")) input[n].quirk = QUIRK_PDSP;

						// Arcade with spinner and/or paddle:
						// Axis 7 - EV_REL is spinner
						// Axis 8 - EV_ABS is Paddle
						// Includes other buttons and axes, works as a full featured gamepad.
						if (strstr(uniq, "MiSTer-A1")) input[n].quirk = QUIRK_PDSP_ARCADE;
						if (strstr(input[n].name, "MiSTer-A1")) input[n].quirk = QUIRK_PDSP_ARCADE;

						//Jamma
						if (cfg.jamma_vid && cfg.jamma_pid && input[n].vid == cfg.jamma_vid && input[n].pid == cfg.jamma_pid)
						{
							input[n].quirk = QUIRK_JAMMA;
						}

						//Jamma2
						if (cfg.jamma2_vid && cfg.jamma2_pid && input[n].vid == cfg.jamma2_vid && input[n].pid == cfg.jamma2_pid)
						{
							input[n].quirk = QUIRK_JAMMA2;
						}

						//Atari VCS wireless joystick with spinner
						if (input[n].vid == 0x3250 && input[n].pid == 0x1001)
						{
							input[n].quirk = QUIRK_VCS;
							input[n].spinner_acc = -1;
							input[n].misc_flags = 0;
						}

						//Arduino and Teensy devices may share the same VID:PID, so additional field UNIQ is used to differentiate them
						//Reflex Adapt also uses the UNIQ field to differentiate between device modes
						//RetroZord Adapter also uses the UNIQ field to differentiate between device modes
						if ((input[n].vid == 0x2341 || (input[n].vid == 0x16C0 && (input[n].pid>>8) == 0x4) || (input[n].vid == 0x16D0 && input[n].pid == 0x127E) || (input[n].vid == 0x1209 && input[n].pid == 0x595A)) && strlen(uniq))
						{
							snprintf(input[n].idstr, sizeof(input[n].idstr), "%04x_%04x_%s", input[n].vid, input[n].pid, uniq);
							char *p;
							while ((p = strchr(input[n].idstr, '/'))) *p = '_';
							while ((p = strchr(input[n].idstr, ' '))) *p = '_';
							while ((p = strchr(input[n].idstr, '*'))) *p = '_';
							while ((p = strchr(input[n].idstr, ':'))) *p = '_';
							strcpy(input[n].name, uniq);
						}
						else if (input[n].vid == 0x1209 && (input[n].pid == 0xFACE || input[n].pid == 0xFACA))
						{
							int sum = 0;
							for (uint32_t i = 0; i < sizeof(input[n].name); i++)
							{
								if (!input[n].name[i]) break;
								sum += (uint8_t)input[n].name[i];
							}
							snprintf(input[n].idstr, sizeof(input[n].idstr), "%04x_%04x_%d", input[n].vid, input[n].pid, sum);
						}
						else
						{
							snprintf(input[n].idstr, sizeof(input[n].idstr), "%04x_%04x", input[n].vid, input[n].pid);
						}

						ioctl(pool[n].fd, EVIOCGRAB, (grabbed | user_io_osd_is_visible()) ? 1 : 0);

						n++;
						if (n >= NUMDEV) break;
					}
				}
			}
			closedir(d);

			mergedevs();
			check_joycon();
			openfire_signal();
			setup_wheels();
			for (int i = 0; i < n; i++)
			{
				printf("opened %d(%2d): %s (%04x:%04x:%08x) %d \"%s\" \"%s\"\n", i, input[i].bind, input[i].devname, input[i].vid, input[i].pid, input[i].unique_hash, input[i].quirk, input[i].id, input[i].name);
				restore_player(i);
				setup_deadzone(&ev, i);
			}
			unflag_players();
		}
		cur_leds |= 0x80;
		state++;
	}

	if (cfg.bt_auto_disconnect)
	{
		if (!timeout) timeout = GetTimer(6000);
		else if (CheckTimer(timeout))
		{
			timeout = GetTimer(6000);
			for (int i = 0; i < NUMDEV; i++)
			{
				if (pool[i].fd >= 0 && input[i].timeout > 0)
				{
					if (!(JOYCON_COMBINED(i) && JOYCON_LEFT(i)) && input[i].bind != i) continue;
					input[i].timeout--;
					if (!input[i].timeout)
					{
						static char cmd[128];
						sprintf(cmd, "btctl disconnect %s", input[i].mac);
						system(cmd);
						if (JOYCON_COMBINED(i))
						{
							sprintf(cmd, "btctl disconnect %s", input[input[i].bind].mac);
							system(cmd);
						}
					}
				}
			}
		}
	}

	if (state == 2)
	{
		int timeout = 0;
		if (is_menu() && video_fb_state()) timeout = 25;

		while (1)
		{
			if (cfg.rumble && !is_menu())
			{
				for (int i = 0; i < NUMDEV; i++)
				{
					if (!input[i].has_rumble) continue;

					int dev = i;
					if (input[i].bind >= 0) dev = input[i].bind;
					if (!input[dev].num) continue;

					set_rumble(i, spi_uio_cmd(UIO_GET_RUMBLE | ((input[dev].num - 1) << 8)));
				}
			}

			int return_value = poll(pool, NUMDEV + 3, timeout);
			if (!return_value) break;

			if (return_value < 0)
			{
				printf("ERR: poll\n");
				break;
			}

			if ((pool[NUMDEV].revents & POLLIN) && check_devs())
			{
				printf("Close all devices.\n");
				for (int i = 0; i < NUMDEV; i++) if (pool[i].fd >= 0)
				{
					ioctl(pool[i].fd, EVIOCGRAB, 0);
					close(pool[i].fd);
				}
				state = 1;
				return 0;
			}

			for (int pos = 0; pos < NUMDEV; pos++)
			{
				int i = pos;


				if ((pool[i].fd >= 0) && (pool[i].revents & POLLIN))
				{
					if (!input[i].mouse)
					{

						memset(&ev, 0, sizeof(ev));
						if (read(pool[i].fd, &ev, sizeof(ev)) == sizeof(ev))
						{
							if (getchar)
							{
								if (ev.type == EV_KEY && ev.value >= 1)
								{
									return ev.code;
								}
							}
							else if (ev.type)
							{
								int dev = i;
								if (!JOYCON_COMBINED(i) && input[dev].bind >= 0) dev = input[dev].bind;

								int noabs = 0;

								if (input[i].quirk == QUIRK_DS4TOUCH && ev.type == EV_KEY)
								{
									if (ev.code == BTN_TOOL_FINGER || ev.code == BTN_TOUCH || ev.code == BTN_TOOL_DOUBLETAP) continue;
								}

								if (input[i].quirk == QUIRK_MADCATZ360 && ev.type == EV_KEY)
								{
									if (ev.code == BTN_THUMBR) input[i].misc_flags = ev.value ? (input[i].misc_flags | 1) : (input[i].misc_flags & ~1);
									else if (ev.code == BTN_MODE && !user_io_osd_is_visible())
									{
										if (input[i].misc_flags & 1)
										{
											if (ev.value)
											{
												if ((input[i].misc_flags & 0x6) == 0) input[i].misc_flags = 0x3; // X
												else if ((input[i].misc_flags & 0x6) == 2) input[i].misc_flags = 0x5; // Y
												else input[i].misc_flags = 0x1; // None

												Info(((input[i].misc_flags & 0x6) == 2) ? "Paddle mode" :
													((input[i].misc_flags & 0x6) == 4) ? "Spinner mode" :
													"Normal mode");
											}
											continue;
										}
									}
								}

								if (input[i].quirk == QUIRK_TOUCHGUN)
								{
									touchscreen_proc(i, &ev);
									continue;
								}

								if (ev.type == EV_ABS)
								{
									if (input[i].quirk == QUIRK_WIIMOTE)
									{
										//nunchuck accel events
										if (ev.code >= 3 && ev.code <= 5) continue;
									}

									//Dualshock: drop accelerator and raw touchpad events
									if (input[i].quirk == QUIRK_DS4TOUCH && ev.code == 57)
									{
										input[dev].lightgun_req = (ev.value >= 0);
									}

									if ((input[i].quirk == QUIRK_DS4TOUCH || input[i].quirk == QUIRK_DS4 || input[i].quirk == QUIRK_DS3) && ev.code > 40)
									{
										continue;
									}

									if (ioctl(pool[i].fd, EVIOCGABS(ev.code), &absinfo) < 0) memset(&absinfo, 0, sizeof(absinfo));
									else
									{
										//DS4 specific: touchpad as lightgun
										if (input[i].quirk == QUIRK_DS4TOUCH && ev.code <= 1)
										{
											if (!input[dev].lightgun || user_io_osd_is_visible()) continue;

											if (ev.code == 1)
											{
												absinfo.minimum = 300;
												absinfo.maximum = 850;
											}
											else if (ev.code == 0)
											{
												absinfo.minimum = 200;
												absinfo.maximum = 1720;
											}
											else continue;
										}

										if (input[i].quirk == QUIRK_DS4 && ev.code <= 1)
										{
											if (input[dev].lightgun) noabs = 1;
										}

										if (input[i].quirk == QUIRK_WIIMOTE)
										{
											input[dev].lightgun = 0;
											if (absinfo.maximum == 1023 || absinfo.maximum == 767)
											{
												if (ev.code == 16)
												{
													ev.value = absinfo.maximum - ev.value;
													ev.code = 0;
													input[dev].lightgun = 1;
												}
												else if (ev.code == 17)
												{
													ev.code = 1;
													input[dev].lightgun = 1;
												}
												// other 3 IR tracking aren't used
												else continue;
											}
											else if (absinfo.maximum == 62)
											{
												//LT/RT analog
												continue;
											}
											else if (ev.code & 1)
											{
												//Y axes on wiimote and accessories are inverted
												ev.value = -ev.value;
											}
										}
									}

									if (input[i].quirk == QUIRK_MADCATZ360 && (input[i].misc_flags & 0x6) && (ev.code == 16) && !user_io_osd_is_visible())
									{
										if (ev.value)
										{
											if ((input[i].misc_flags & 0x6) == 2)
											{
												if (ev.value > 0) input[i].paddle_val += 4;
												if (ev.value < 0) input[i].paddle_val -= 4;

												if (input[i].paddle_val > 256) input[i].paddle_val = 256;
												if (input[i].paddle_val < 0)   input[i].paddle_val = 0;

												absinfo.maximum = 255;
												absinfo.minimum = 0;
												ev.code = 8;
												ev.value = input[i].paddle_val;
											}
											else
											{
												ev.type = EV_REL;
												ev.code = 7;
											}
										}
										else continue;
									}
								}

								if (input[dev].quirk == QUIRK_VCS && !vcs_proc(i, &ev)) continue;

								if (input[dev].quirk == QUIRK_JAMMA && ev.type == EV_KEY)
								{
									input[dev].num = 0;
									for (uint32_t i = 0; i < (uint32_t)jamma2joy_count; i++)
									{
										if (jamma2joy[i].key == ev.code)
										{
											ev.code = jamma2joy[i].btn;
											input[dev].num = jamma2joy[i].player;
											break;
										}
									}
                                }

								if (input[dev].quirk == QUIRK_JAMMA2 && ev.type == EV_KEY)
								{
									input[dev].num = 0;
									for (uint32_t i = 0; i < (uint32_t)jamma22joy_count; i++)
									{
										if (jamma22joy[i].key == ev.code)
										{
											ev.code = jamma22joy[i].btn;
											input[dev].num = jamma22joy[i].player;
											break;
										}
									}
								}

								if (input[i].quirk == QUIRK_JOYCON)
								{
									if (process_joycon(i, &ev, &absinfo))
									{
										state = 1;
										return 0;
									}
								}

								//Menu combo on 8BitDo receiver in PSC mode
								if (input[dev].vid == 0x054c && input[dev].pid == 0x0cda && ev.type == EV_KEY)
								{
									//in PSC mode these keys coming from separate virtual keyboard device
									//so it's impossible to use joystick codes as keyboards aren't personalized
									if (ev.code == 164 || ev.code == 1) ev.code = KEY_MENU;
								}

								// various controllers in X-Input mode generate keyboard key codes, remap them.
								if (input[dev].vid == 0x45E && ev.type == EV_KEY)
								{
									switch (ev.code)
									{
									case KEY_BACK:   ev.code = BTN_SELECT; break;
									case KEY_MENU:   ev.code = BTN_MODE;   break;
									case KEY_RECORD: ev.code = BTN_Z;      break;
									}
								}

								if (is_menu() && !video_fb_state())
								{
									/*
									if (mapping && mapping_type <= 1 && !(ev.type==EV_KEY && ev.value>1))
									{
										static char str[64], str2[64];
										OsdWrite(12, "\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81");
										sprintf(str, "     VID=%04X PID=%04X", input[i].vid, input[i].pid);
										OsdWrite(13, str);

										sprintf(str, "Type=%d Code=%d Value=%d", ev.type, ev.code, ev.value);
										str2[0] = 0;
										int len = (29 - (strlen(str))) / 2;
										while (len-- > 0) strcat(str2, " ");
										strcat(str2, str);
										OsdWrite(14, str2);

										str2[0] = 0;
										if (ev.type == EV_ABS)
										{
											sprintf(str, "Min=%d Max=%d", absinfo.minimum, absinfo.maximum);
											int len = (29 - (strlen(str))) / 2;
											while (len-- > 0) strcat(str2, " ");
											strcat(str2, str);
										}
										OsdWrite(15, str2);
									}
									*/

									switch (ev.type)
									{
										//keyboard, buttons
									case EV_KEY:
										printf("%04x:%04x:%02d P%d Input event: type=EV_KEY, code=%d(0x%x), value=%d\n", input[dev].vid, input[dev].pid, i, input[dev].num, ev.code, ev.code, ev.value);
										break;

									case EV_REL:
									{
										//limit the amount of EV_REL messages, so Menu core won't be laggy
										static unsigned long timeout = 0;
										if (!timeout || CheckTimer(timeout))
										{
											timeout = GetTimer(20);
											printf("%04x:%04x:%02d P%d Input event: type=EV_REL, Axis=%d, Offset=%d\n", input[dev].vid, input[dev].pid, i, input[dev].num, ev.code, ev.value);
										}
									}
									break;

									case EV_SYN:
									case EV_MSC:
										break;

										//analog joystick
									case EV_ABS:
									{
										//limit the amount of EV_ABS messages, so Menu core won't be laggy
										static unsigned long timeout = 0;
										if (!timeout || CheckTimer(timeout))
										{
											timeout = GetTimer(20);

											//reduce flood from DUALSHOCK 3/4
											if ((input[i].quirk == QUIRK_DS4 || input[i].quirk == QUIRK_DS3) && ev.code <= 5 && ev.value > 118 && ev.value < 138)
											{
												break;
											}

											//aliexpress USB encoder floods messages
											if (input[dev].vid == 0x0079 && input[dev].pid == 0x0006)
											{
												if (ev.code == 2) break;
											}

											printf("%04x:%04x:%02d P%d Input event: type=EV_ABS, Axis=%d [%d...%d], Offset=%d", input[dev].vid, input[dev].pid, i, input[dev].num, ev.code, absinfo.minimum, absinfo.maximum, ev.value);
											//if (absinfo.fuzz) printf(", fuzz = %d", absinfo.fuzz);
											if (absinfo.resolution) printf(", res = %d", absinfo.resolution);
											printf("\n");
										}
									}
									break;

									default:
										printf("%04x:%04x:%02d P%d Input event: type=%d, code=%d(0x%x), value=%d(0x%x)\n", input[dev].vid, input[dev].pid, i, input[dev].num, ev.type, ev.code, ev.code, ev.value, ev.value);
									}

									if (ev.type == EV_KEY && input[dev].num)
									{
										if (ev.code == (input[dev].mmap[SYS_BTN_L] & 0xFFFF)) input[dev].rumble_en = ev.value;

										int n = get_rumble_device(input[dev].num);
										if (n >= 0 && (input[dev].rumble_en || !ev.value))
										{
											uint16_t rumble_val = input[n].last_rumble;
											if (ev.code == (input[dev].mmap[SYS_BTN_X] & 0xFFFF)) set_rumble(n, (rumble_val & 0xFF00) | ((ev.value) ? 0xFF : 0x00));
											if (ev.code == (input[dev].mmap[SYS_BTN_Y] & 0xFFFF)) set_rumble(n, (rumble_val & 0xFF) | ((ev.value) ? 0xFF00 : 0x00));
										}
									}
								}

								if (ev.type == EV_ABS && input[i].quirk == QUIRK_WIIMOTE && input[dev].lightgun)
								{
									menu_lightgun_cb(i, ev.type, ev.code, ev.value);

									// don't pass IR tracking to OSD
									if (user_io_osd_is_visible()) continue;

									if (!ev.code)
									{
										absinfo.minimum = input[i].guncal[2];
										absinfo.maximum = input[i].guncal[3];
									}
									else
									{
										absinfo.minimum = input[i].guncal[0];
										absinfo.maximum = input[i].guncal[1];
									}
								}

								if (ev.type == EV_ABS && (input[i].quirk == QUIRK_LIGHTGUN || input[i].quirk == QUIRK_LIGHTGUN_MOUSE))
								{
									menu_lightgun_cb(i, ev.type, ev.code, ev.value);

									if (ev.code == ABS_X)
									{
										absinfo.minimum = input[i].guncal[2];
										absinfo.maximum = input[i].guncal[3];
									}
									else if (ev.code == ABS_Y)
									{
										absinfo.minimum = input[i].guncal[0];
										absinfo.maximum = input[i].guncal[1];
									}
								}

								if (ev.type == EV_ABS && input[i].quirk == QUIRK_LIGHTGUN_CRT)
								{
									menu_lightgun_cb(i, ev.type, ev.code, ev.value);

									if (ev.code == ABS_X)
									{
										absinfo.minimum = input[i].guncal[2];
										absinfo.maximum = input[i].guncal[3];

										// When the gun loses tracking, give it a short grace period
										// before passing through the off-screen coordinates.
										// The GunCon 1 and 2 both report out-of-screen x values
										// more reliably than Y values, so X is used here.
										if (ev.value < absinfo.minimum || ev.value > absinfo.maximum)
										{
											// Grace period of 50 ms. Longer times here make guns a bit
											// more reliable on dark screens, but introduce lag to any mechanics
											// where you want to shoot offscreen (e.g., to reload.)
											if (!crtgun_timeout[i]) crtgun_timeout[i] = GetTimer(50);
										}
										else
										{
											crtgun_timeout[i] = 0;
											input[i].lastx = ev.value;
										}
										// For the window between losing the gun signal and the timer
										// running out, report the last on-screen coordinate
										if (crtgun_timeout[i] && !CheckTimer(crtgun_timeout[i]))
										{
											ev.value = input[i].lastx;
										}
									}
									else if (ev.code == ABS_Y)
									{
										absinfo.minimum = input[i].guncal[0];
										absinfo.maximum = input[i].guncal[1];

										// Handle gun going off-screen
										if (crtgun_timeout[i])
										{
											// For the window between losing the gun signal and the timer
											// running out, report the last on-screen coordinate
											if (!CheckTimer(crtgun_timeout[i]))
											{
												ev.value = input[i].lasty;
											}
										}
										else
										{
											input[i].lasty = ev.value;
										}
									}
								}

								if (ev.type == EV_KEY && user_io_osd_is_visible())
								{
									if (input[i].quirk == QUIRK_WIIMOTE || input[i].quirk == QUIRK_LIGHTGUN_CRT || input[i].quirk == QUIRK_LIGHTGUN || input[i].quirk == QUIRK_LIGHTGUN_MOUSE)
									{
										if (menu_lightgun_cb(i, ev.type, ev.code, ev.value)) continue;
									}
								}

								// redirect further actions to left joycon in combined mode
								if (JOYCON_COMBINED(i))
								{
									if (JOYCON_RIGHT(i)) i = input[i].bind;
									dev = i;
								}

								if (!noabs) input_cb(&ev, &absinfo, i);

								// simulate digital directions from analog
								if (ev.type == EV_ABS && !(mapping && mapping_type <= 1 && mapping_button < -4) && !(ev.code <= 1 && input[dev].lightgun) && input[dev].quirk != QUIRK_PDSP && input[dev].quirk != QUIRK_MSSP)
								{
									input_absinfo *pai = 0;
									uint8_t axis_edge = 0;
									if ((absinfo.maximum == 1 && absinfo.minimum == -1) || (absinfo.maximum == 2 && absinfo.minimum == 0))
									{
										if (ev.value == absinfo.minimum) axis_edge = 1;
										if (ev.value == absinfo.maximum) axis_edge = 2;
									}
									else
									{
										pai = &absinfo;
										int range = absinfo.maximum - absinfo.minimum + 1;
										int center = absinfo.minimum + (range / 2);
										int treshold = range / 4;

										int only_max = 1;
										for (int n = 0; n < 4; n++) if (input[dev].mmap[SYS_AXIS1_X + n] && ((input[dev].mmap[SYS_AXIS1_X + n] & 0xFFFF) == ev.code)) only_max = 0;

										if (ev.value < center - treshold && !only_max) axis_edge = 1;
										if (ev.value > center + treshold) axis_edge = 2;
									}

									uint8_t last_state = input[dev].axis_edge[ev.code & 255];
									input[dev].axis_edge[ev.code & 255] = axis_edge;

									//printf("last_state=%d, axis_edge=%d\n", last_state, axis_edge);
									if (last_state != axis_edge)
									{
										uint16_t ecode = KEY_EMU + (ev.code << 1) - 1;
										ev.type = EV_KEY;
										if (last_state)
										{
											ev.value = 0;
											ev.code = ecode + last_state;
											input_cb(&ev, pai, i);
										}

										if (axis_edge)
										{
											ev.value = 1;
											ev.code = ecode + axis_edge;
											input_cb(&ev, pai, i);
										}
									}

									// Menu button on 8BitDo Receiver in D-Input mode
									if (ev.code == 9 && input[dev].vid == 0x2dc8 && (input[dev].pid == 0x3100 || input[dev].pid == 0x3104))
									{
										ev.type = EV_KEY;
										ev.code = KEY_EMU + (ev.code << 1);
										input_cb(&ev, pai, i);
									}
								}
							}
						}
					}
					else
					{
						uint8_t data[4] = {};
						if (read(pool[i].fd, data, sizeof(data)))
						{
							int edev = i;
							int dev = i;
							if (input[i].bind >= 0) edev = input[i].bind; // mouse to event
							if (input[edev].bind >= 0) dev = input[edev].bind; // event to base device

							if ((input[i].quirk == QUIRK_DS4TOUCH || input[i].quirk == QUIRK_DS4))
							{
								//disable DS4 mouse in lightgun mode
								if (input[dev].lightgun) continue;
							}

							if (input[i].quirk == QUIRK_TOUCHGUN)
							{
								//don't use original raspad3 emulated mouse
								continue;
							}

							int xval, yval, zval;
							xval = ((data[0] & 0x10) ? -256 : 0) | data[1];
							yval = ((data[0] & 0x20) ? -256 : 0) | data[2];
							zval = ((data[3] & 0x80) ? -256 : 0) | data[3];

							input_absinfo absinfo = {};
							absinfo.maximum = 255;
							absinfo.minimum = 0;

							if (input[dev].quirk == QUIRK_MSSP)
							{
								int val;
								if(cfg.spinner_axis == 0)
									val = xval;
								else if(cfg.spinner_axis == 1)
									val = yval;
								else
									val = zval;

								int btn = (data[0] & 7) ? 1 : 0;
								if (input[i].misc_flags != btn)
								{
									input[i].misc_flags = btn;
									ev.value = btn;
									ev.type = EV_KEY;
									ev.code = 0x120;
									input_cb(&ev, &absinfo, i);
								}

								int throttle = (cfg.spinner_throttle ? abs(cfg.spinner_throttle) : 100) * input[i].spinner_prediv;
								int inv = cfg.spinner_throttle < 0;

								input[i].spinner_acc += (val * 100);
								int spinner = (input[i].spinner_acc <= -throttle || input[i].spinner_acc >= throttle) ? (input[i].spinner_acc / throttle) : 0;
								input[i].spinner_acc -= spinner * throttle;

								if (spinner)
								{
									ev.value = inv ? -spinner : spinner;
									ev.type = EV_REL;
									ev.code = 7;
									input_cb(&ev, &absinfo, i);

									input[i].paddle_val += ev.value;
									if (input[i].paddle_val < 0) input[i].paddle_val = 0;
									if (input[i].paddle_val > 255) input[i].paddle_val = 255;

									ev.value = input[i].paddle_val;
									ev.type = EV_ABS;
									ev.code = 8;
									input_cb(&ev, &absinfo, i);
								}

								if (is_menu() && !video_fb_state()) printf("%s: xval=%d, btn=%d, spinner=%d, paddle=%d\n", input[i].devname, val, btn, spinner, input[i].paddle_val);
							}
							else
							{
								send_mouse_with_throttle(i, xval, yval, data[3]);
							}
						}
					}
				}
			}

			if ((pool[NUMDEV + 1].fd >= 0) && (pool[NUMDEV + 1].revents & POLLIN))
			{
				static char cmd[1024];
				int len = read(pool[NUMDEV + 1].fd, cmd, sizeof(cmd) - 1);
				if (len)
				{
					if (cmd[len - 1] == '\n') cmd[len - 1] = 0;
					cmd[len] = 0;
					printf("MiSTer_cmd: %s\n", cmd);
					if (!strncmp(cmd, "fb_cmd", 6)) video_cmd(cmd);
					else if (!strncmp(cmd, "load_core ", 10))
					{
						if(isXmlName(cmd)) xml_load(cmd + 10);
						else fpga_load_rbf(cmd + 10);
					}
					else if (!strncmp(cmd, "screenshot", 10))
					{
						user_io_screenshot_cmd(cmd);
					}
					else if (!strncmp(cmd, "volume ", 7))
					{
						if (!strcmp(cmd + 7, "mute")) set_volume(0x81);
						else if (!strcmp(cmd + 7, "unmute")) set_volume(0x80);
						else if (cmd[7] >= '0' && cmd[7] <= '7') set_volume(0x40 - 0x30 + cmd[7]);
					}
				}
			}

			if ((pool[NUMDEV + 2].fd >= 0) && (pool[NUMDEV + 2].revents & POLLPRI))
			{
				static char status[16];
				if (read(pool[NUMDEV + 2].fd, status, sizeof(status) - 1) && status[0] != '0')
				{
					if (sysled_is_enabled || video_fb_state()) DISKLED_ON;
				}
				lseek(pool[NUMDEV + 2].fd, 0, SEEK_SET);
			}
		}

		if (cur_leds != leds_state)
		{
			cur_leds = leds_state;
			for (int i = 0; i < NUMDEV; i++)
			{
				if (input[i].led)
				{
					ev.type = EV_LED;

					ev.code = LED_SCROLLL;
					ev.value = (cur_leds&HID_LED_SCROLL_LOCK) ? 1 : 0;
					write(pool[i].fd, &ev, sizeof(struct input_event));

					ev.code = LED_NUML;
					ev.value = (cur_leds&HID_LED_NUM_LOCK) ? 1 : 0;
					write(pool[i].fd, &ev, sizeof(struct input_event));

					ev.code = LED_CAPSL;
					ev.value = (cur_leds&HID_LED_CAPS_LOCK) ? 1 : 0;
					write(pool[i].fd, &ev, sizeof(struct input_event));
				}
			}
		}
	}

	return 0;
}

/* ========================================================================
 *  input_poll  -  per-frame entry point called from the main loop
 *
 *  Drives autofire frame counters, calls input_test() for device I/O,
 *  then aggregates digital joystick masks and mouse deltas for the core.
 * ======================================================================== */

int input_poll(int getchar)
{
	PROFILE_FUNCTION();
	static bool autofire_cfg_parsed = false;
 	if (!autofire_cfg_parsed) autofire_cfg_parsed = parse_autofire_cfg();
	static uint32_t joy_mask_prev[NUMPLAYERS] = {};
	
	// FRAME_TICK compares against frame_timer's counter (updated elsewhere) and fires once per frame.
	static uint32_t last_frame_count = 0;
	if (FRAME_TICK(last_frame_count)) {
		key_update_frames_held();
		//autofire_tick(); 	// advance all autofire patterns by 1
	}

	int ret = input_test(getchar);
	if (getchar) return ret;

	uinp_check_key();

	static int prev_dx = 0;
	static int prev_dy = 0;

	if (mouse_emu || ((user_io_get_kbdemu() == EMU_MOUSE) && kbd_mouse_emu))
	{
		if((prev_dx || mouse_emu_x || prev_dy || mouse_emu_y) && (!mouse_timer || CheckTimer(mouse_timer)))
		{
			mouse_timer = GetTimer(20);

			int dx = mouse_emu_x;
			int dy = mouse_emu_y;
			if (mouse_sniper ^ cfg.sniper_mode)
			{
				if (dx > 2) dx = 2;
				if (dx < -2) dx = -2;
				if (dy > 2) dy = 2;
				if (dy < -2) dy = -2;
			}

			mouse_cb(dx, dy);
			prev_dx = mouse_emu_x;
			prev_dy = mouse_emu_y;
		}
	}

	if (!mouse_emu_x && !mouse_emu_y) mouse_timer = 0;

	uint32_t joy_mask[NUMPLAYERS];
	uint32_t autofire_mask[NUMPLAYERS];

	for (int i = 0; i < NUMPLAYERS; i++) {
		joy_mask[i] = build_joy_mask(i);
		autofire_mask[i] = build_autofire_mask(i);
	}

	if (grabbed)
	{
		for (int i = 0; i < NUMPLAYERS; i++) {
			joy_mask[i] = joy_mask[i] | autofire_mask[i];
			int newdir = (joy_mask[i] & 0xF) | (joy_mask_prev[i] & 0xF);
			if (joy_mask[i] != joy_mask_prev[i])
			{
				joy_mask_prev[i] = joy_mask[i];
				user_io_digital_joystick(i, joy_mask[i], newdir);
			}
		}
	}

	if (!grabbed || user_io_osd_is_visible())
	{
		for (int i = 0; i < NUMPLAYERS; i++)
		{
			if(joy_mask[i]) user_io_digital_joystick(i, 0, 1);
		}
		memset(key_states, 0, sizeof(KeyStates) * NUMPLAYERS);
	}

	if (mouse_req)
	{
		static uint32_t old_time = 0;
		uint32_t time = GetTimer(0);
		if ((time - old_time > 15) || (mouse_req & 2))
		{
			old_time = time;
			user_io_mouse(mouse_btn | mice_btn, mouse_x, mouse_y, mouse_w);
			mouse_req = 0;
			mouse_x = 0;
			mouse_y = 0;
			mouse_w = 0;
		}
	}

	return 0;
}

/* Query the kernel to see if a specific key is physically held down. */
int is_key_pressed(int key)
{
	unsigned char bits[(KEY_CNT + 7) / 8];
	for (int i = 0; i < NUMDEV; i++)
	{
		if (pool[i].fd > 0)
		{
			unsigned long evbit = 0;
			if (ioctl(pool[i].fd, EVIOCGBIT(0, sizeof(evbit)), &evbit) >= 0)
			{
				if (evbit & (1 << EV_KEY))
				{
					memset(bits, 0, sizeof(bits));
					if (ioctl(pool[i].fd, EVIOCGKEY(sizeof(bits)), &bits) >= 0)
					{
						if (bits[key / 8] & (1 << (key % 8)))
						{
							return 1;
						}
					}
				}
			}
		}
	}

	return 0;
}

/* Reset mouse-emulation state on core/mode switch. */
void input_notify_mode()
{
	//reset mouse parameters on any mode switch
	kbd_mouse_emu = 1;
	mouse_sniper = 0;
	mouse_timer = 0;
	mouse_btn = 0;
	mouse_emu_x = 0;
	mouse_emu_y = 0;
	mouse_cb();
	mouse_btn_req();
}

/* Grab or release all evdev file descriptors (prevents events leaking to Linux). */
void input_switch(int grab)
{
	if (grab >= 0) grabbed = grab;
	//printf("input_switch(%d), grabbed = %d\n", grab, grabbed);

	for (int i = 0; i < NUMDEV; i++)
	{
		if (pool[i].fd >= 0) ioctl(pool[i].fd, EVIOCGRAB, (grabbed | user_io_osd_is_visible()) ? 1 : 0);
	}
}

int input_state()
{
	return grabbed;
}

/* ========================================================================
 *  Core button-string overrides (J,/jn,/jp, in MRA/core config)
 * ======================================================================== */

static char ovr_buttons[1024] = {};
static char ovr_nmap[1024] = {};
static char ovr_pmap[1024] = {};

static char *get_btn(int type)
{
	int i = 2;
	while (1)
	{
		char *p = user_io_get_confstr(i);
		if (!p) break;

		if ((p[0] == 'J' && !type) || (p[0] == 'j' && ((p[1] == 'n' && type == 1) || (p[1] == 'p' && type == 2))))
		{
			p = strchr(p, ',');
			if (!p) break;

			p++;
			if (!strlen(p)) break;
			return p;
		}

		i++;
	}
	return NULL;
}

char *get_buttons(int type)
{
	if (type == 0 && ovr_buttons[0]) return ovr_buttons;
	if (type == 1 && ovr_nmap[0]) return ovr_nmap;
	if (type == 2 && ovr_pmap[0]) return ovr_pmap;

	return get_btn(type);
}

void set_ovr_buttons(char *s, int type)
{
	switch (type)
	{
	case 0:
		snprintf(ovr_buttons, sizeof(ovr_buttons), "%s", s);
		break;

	case 1:
		snprintf(ovr_nmap, sizeof(ovr_nmap), "%s", s);
		break;

	case 2:
		snprintf(ovr_pmap, sizeof(ovr_pmap), "%s", s);
		break;
	}
}

void parse_buttons()
{
	joy_bcount = 0;

	char *str = get_buttons();
	if (!str) return;

	for (int n = 0; n < 28; n++)
	{
		substrcpy(joy_bnames[n], str, n);
		if (!joy_bnames[n][0]) break;
		joy_bcount++;
	}
}
