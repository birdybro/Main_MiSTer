/*
 * input_dev.cpp - Device management for the input subsystem.
 *
 * Handles device detection (inotify), identification/merging, player
 * assignment, LED indication, button-map file I/O, deadzone configuration,
 * and light-gun calibration persistence.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/inotify.h>
#include <linux/input.h>
#include <stdint.h>

#include "input.h"
#include "input/input_internal.h"
#include "cfg.h"
#include "file_io.h"
#include "support.h"
#include "user_io.h"
#include "joymapping.h"
#include "autofire.h"
#include "gamecontroller_db.h"
#include "str_util.h"

/* ========================================================================
 *  Device hotplug via inotify
 * ======================================================================== */

static int mfd = -1;
static int mwd = -1;

/* Set up inotify to watch /dev/input for device hotplug events.
 * Returns the inotify fd on success, -1 on failure. */
int set_watch()
{
	mwd = -1;
	mfd = inotify_init1(IN_CLOEXEC);
	if (mfd < 0)
	{
		printf("ERR: inotify_init");
		return -1;
	}

	mwd = inotify_add_watch(mfd, "/dev/input", IN_MODIFY | IN_CREATE | IN_DELETE);

	if (mwd < 0)
	{
		printf("ERR: inotify_add_watch");
		return -1;
	}

	return mfd;
}

#define EVENT_SIZE  ( sizeof (struct inotify_event) )
#define BUF_LEN     ( 1024 * ( EVENT_SIZE + 16 ) )

/* Read pending inotify events; returns 1 if devices were added/removed. */
int check_devs()
{
	int result = 0;
	int length, i = 0;
	char buffer[BUF_LEN];
	length = read(mfd, buffer, BUF_LEN);

	if (length < 0)
	{
		printf("ERR: read\n");
		return 0;
	}

	while (i<length)
	{
		struct inotify_event *event = (struct inotify_event *) &buffer[i];
		if (event->len)
		{
			if (event->mask & IN_CREATE)
			{
				result = 1;
				if (event->mask & IN_ISDIR)
				{
					printf("The directory %s was created.\n", event->name);
				}
				else
				{
					printf("The file %s was created.\n", event->name);
				}
			}
			else if (event->mask & IN_DELETE)
			{
				result = 1;
				if (event->mask & IN_ISDIR)
				{
					printf("The directory %s was deleted.\n", event->name);
				}
				else
				{
					printf("The file %s was deleted.\n", event->name);
				}
			}
		}
		i += EVENT_SIZE + event->len;
	}

	return result;
}

void INThandler(int code)
{
	(void)code;

	printf("\nExiting...\n");

	if (mwd >= 0) inotify_rm_watch(mfd, mwd);
	if (mfd >= 0) close(mfd);

	exit(0);
}

/* ========================================================================
 *  LED detection & control
 * ======================================================================== */

char has_led(int fd)
{
	unsigned char evtype_b[(EV_MAX + 7) / 8];
	if (fd<0) return 0;

	memset(&evtype_b, 0, sizeof(evtype_b));
	if (ioctl(fd, EVIOCGBIT(0, sizeof(evtype_b)), evtype_b) < 0)
	{
		printf("ERR: evdev ioctl.\n");
		return 0;
	}

	return test_bit(EV_LED, evtype_b) ? 1 : 0;
}

char *get_led_path(int dev, int add_id)
{
	static char path[1024];
	if (!input[dev].sysfs[0]) return NULL;

	sprintf(path, "/sys%s", input[dev].sysfs);
	char *p = strstr(path, "/input/");
	if (p)
	{
		*p = 0;
		char *id = strrchr(path, '/');
		strcpy(p, "/leds");
		if (add_id && id) strncat(p, id, p - id);
		return path;
	}

	return NULL;
}

int set_led(char *base, const char *led, int brightness)
{
	static char path[1024];
	snprintf(path, sizeof(path), "%s%s/brightness", base, led);
	FILE* f = fopen(path, "w");
	if (f)
	{
		fprintf(f, "%d", brightness);
		fclose(f);
		return 1;
	}

	return 0;
}

int get_led(char *base, const char *led)
{
	static char path[1024];
	snprintf(path, sizeof(path), "%s%s/brightness", base, led);
	FILE* f = fopen(path, "r");
	if (f)
	{
		int res = 0;
		fscanf(f, "%d", &res);
		fclose(f);
		return res;
	}

	return 0;
}

/* ========================================================================
 *  Player number indication via controller LEDs
 * ======================================================================== */

void update_num_hw(int dev, int num)
{
	char *led_path;
	if (num > 7) num = 7;

	if (input[dev].quirk == QUIRK_DS4 || input[dev].quirk == QUIRK_DS4TOUCH)
	{
		led_path = get_led_path(dev);
		if (led_path)
		{
			if (set_led(led_path, ":player_id", (num > 5) ? 0 : num))
			{
				//duslsense
				set_led(led_path, ":blue", (num == 0) ? 128 : 64);
				set_led(led_path, ":green", (num == 0) ? 128 : 64);
				set_led(led_path, ":red", (num == 0) ? 128 : 0);
			}
			else
			{
				//dualshock4
				static const uint8_t color_code[8][3] =
				{
					{ 0x30, 0x30, 0x30 }, // White
					{ 0x00, 0x00, 0x40 }, // Blue
					{ 0x40, 0x00, 0x00 }, // Red
					{ 0x00, 0x40, 0x00 }, // Green
					{ 0x20, 0x00, 0x20 }, // Pink
					{ 0x40, 0x10, 0x00 }, // Orange
					{ 0x00, 0x20, 0x20 }, // Teal
					{ 0x00, 0x00, 0x00 }  // none
				};

				set_led(led_path, ":blue", color_code[num][2]);
				set_led(led_path, ":green", color_code[num][1]);
				set_led(led_path, ":red", color_code[num][0]);
			}
		}
	}
	else if (input[dev].quirk == QUIRK_DS3)
	{
		led_path = get_led_path(dev);
		if (led_path)
		{
			set_led(led_path, "::sony1", (num == 0 || num == 1 || num == 5));
			set_led(led_path, "::sony2", (num == 0 || num == 2 || num == 6));
			set_led(led_path, "::sony3", (num == 0 || num == 3));
			set_led(led_path, "::sony4", (num == 0 || num == 4 || num == 5 || num == 6));
		}
	}
	else if (input[dev].vid == 0x057e && (input[dev].pid == 0x0306 || input[dev].pid == 0x0330))
	{
		led_path = get_led_path(dev);
		if (led_path)
		{
			set_led(led_path, ":blue:p0", (num == 0 || num == 1 || num == 5));
			set_led(led_path, ":blue:p1", (num == 0 || num == 2 || num == 6));
			set_led(led_path, ":blue:p2", (num == 0 || num == 3));
			set_led(led_path, ":blue:p3", (num == 0 || num == 4 || num == 5 || num == 6));
		}
	}
	else if (input[dev].vid == 0x057e && ((input[dev].pid & 0xFF00) == 0x2000))
	{
		// nintendo switch controllers
		int repeat = 1;
		while (1)
		{
			led_path = get_led_path(dev);
			if (led_path)
			{
				set_led(led_path, ":home", num ? 1 : 15);
				set_led(led_path, ":player1", (num == 0 || num == 1 || num == 5));
				set_led(led_path, ":player2", (num == 0 || num == 2 || num == 6));
				set_led(led_path, ":player3", (num == 0 || num == 3));
				set_led(led_path, ":player4", (num == 0 || num == 4 || num == 5 || num == 6));
			}

			if (repeat && JOYCON_COMBINED(dev)) dev = input[dev].bind; else break;
			repeat = 0;
		}
	}
}

/* ========================================================================
 *  Button mapping file I/O
 * ======================================================================== */

#define JOYMAP_DIR  "inputs/"

/* Load a controller map from the "inputs/" config directory.
 * Falls back to the root config directory for legacy compatibility. */
int load_map(const char *name, void *pBuffer, int size)
{
	char path[256] = { JOYMAP_DIR };
	strcat(path, name);
	int ret = FileLoadConfig(path, pBuffer, size);
	if (!ret) return FileLoadConfig(name, pBuffer, size);
	return ret;
}

void delete_map(const char *name)
{
	char path[256] = { JOYMAP_DIR };
	strcat(path, name);
	FileDeleteConfig(name);
	FileDeleteConfig(path);
}

int save_map(const char *name, void *pBuffer, int size)
{
	char path[256] = { JOYMAP_DIR };
	strcat(path, name);
	FileDeleteConfig(name);
	return FileSaveConfig(path, pBuffer, size);
}

/* ========================================================================
 *  Mapping name generation
 * ======================================================================== */

/* Return a unique mapping key string for the device.
 * If force_unique is set (or configured per-VID:PID), appends a hash
 * of the device's MAC/serial so identical controllers get separate maps. */
char *get_unique_mapping(int dev, int force_unique)
{
	uint32_t vidpid = (input[dev].vid << 16) | input[dev].pid;
	static char str[128];

	for (uint i = 0; i < cfg.controller_unique_mapping[0]; i++)
	{
		if (!cfg.controller_unique_mapping[i + 1]) break;
		if (force_unique || cfg.controller_unique_mapping[i + 1] == 1 || cfg.controller_unique_mapping[i + 1] == vidpid)
		{
			sprintfz(str, "%s_%08x", input[dev].idstr, input[dev].unique_hash);
			return str;
		}
	}

	sprintfz(str, "%s", input[dev].idstr);
	return str;
}

char *get_map_name(int dev, int def)
{
	static char name[1024];
	char *id = get_unique_mapping(dev);

	if (def || is_menu()) sprintfz(name, "input_%s%s_v3.map", id, input[dev].mod ? "_m" : "");
	else sprintfz(name, "%s_input_%s%s_v3.map", user_io_get_core_name(), id, input[dev].mod ? "_m" : "");
	return name;
}

char *get_jkmap_name(int dev)
{
	static char name[1024];
	char *id = get_unique_mapping(dev);
	sprintfz(name, "%s_input_%s_jk.map", user_io_get_core_name(), id);
	return name;
}

char *get_kbdmap_name(int dev)
{
	static char name[128];
	char *id = get_unique_mapping(dev);

	sprintfz(name, "kbd_%s.map", id);
	return name;
}

/* ========================================================================
 *  Default controller mapping
 * ======================================================================== */

uint16_t def_mmap[] = {
	0x0321, 0x0000, 0x0320, 0x0000, 0x0323, 0x0000, 0x0322, 0x0000,
	0x0131, 0x0000, 0x0130, 0x0000, 0x0133, 0x0000, 0x0134, 0x0000,
	0x0136, 0x0000, 0x0137, 0x0000, 0x013A, 0x0000, 0x013B, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x013C, 0x0000, 0x013C, 0x0000, 0x0131, 0x0130,
	0x0000, 0x0002, 0x0001, 0x0002, 0x0003, 0x0002, 0x0004, 0x0002,
	0x0000, 0x0002, 0x0001, 0x0002, 0x0000, 0x0000, 0x0000, 0x0000
};

/* ========================================================================
 *  Light-gun calibration
 * ======================================================================== */

void input_lightgun_save(int idx, int32_t *cal)
{
	static char name[128];
	sprintf(name, "%s_gun_cal_%04x_%04x_v2.cfg", user_io_get_core_name(), input[idx].vid, input[idx].pid);
	FileSaveConfig(name, cal, 4 * sizeof(int32_t));
	memcpy(input[idx].guncal, cal, sizeof(input[idx].guncal));
}

void input_lightgun_load(int idx)
{
	static char name[128];
	sprintf(name, "%s_gun_cal_%04x_%04x_v2.cfg", user_io_get_core_name(), input[idx].vid, input[idx].pid);
	FileLoadConfig(name, input[idx].guncal, 4 * sizeof(int32_t));
}

int input_has_lightgun()
{
	for (int i = 0; i < NUMDEV; i++)
	{
		if (input[i].quirk == QUIRK_WIIMOTE)  return 1;
		if (input[i].quirk == QUIRK_TOUCHGUN) return 1;
		if (input[i].quirk == QUIRK_LIGHTGUN) return 1;
		if (input[i].quirk == QUIRK_LIGHTGUN_CRT) return 1;
		if (input[i].quirk == QUIRK_LIGHTGUN_MOUSE) return 1;
	}
	return 0;
}

/* ========================================================================
 *  Player assignment & persistence
 * ======================================================================== */

static devInput player_pad[NUMPLAYERS] = {};
static devInput player_pdsp[NUMPLAYERS] = {};

void reset_players()
{
	for (int i = 0; i < NUMDEV; i++)
	{
		input[i].num = 0;
		input[i].map_shown = 0;
		update_num_hw(i, 0);
	}

	memset(key_states, 0, sizeof(KeyStates) * NUMPLAYERS);
	for (int i = 0; i < NUMPLAYERS; i++) {
		clear_autofire(i);
	}
	memset(player_pad, 0, sizeof(player_pad));
	memset(player_pdsp, 0, sizeof(player_pdsp));
}

void store_player(int num, int dev)
{
	devInput *player = (input[dev].quirk == QUIRK_PDSP || input[dev].quirk == QUIRK_MSSP) ? player_pdsp : player_pad;

	// remove possible old assignment
	for (int i = 1; i < NUMPLAYERS; i++) if (!strcmp(player[i].id, input[dev].id)) player[i].id[0] = 0;

	if(num && num < NUMPLAYERS) memcpy(&player[num], &input[dev], sizeof(devInput));
	update_num_hw(dev, num);
}

void restore_player(int dev)
{
	// do not restore bound devices
	if (dev != input[dev].bind && !(JOYCON_COMBINED(dev) && JOYCON_LEFT(dev))) return;

	devInput *player = (input[dev].quirk == QUIRK_PDSP || input[dev].quirk == QUIRK_MSSP) ? player_pdsp : player_pad;
	for (int k = 1; k < NUMPLAYERS; k++)
	{
		if (strlen(player[k].id) && !strcmp(player[k].id, input[dev].id))
		{
			printf("restore player %d to %s (%s)\n", k, input[dev].devname, input[dev].id);

			input[dev].num = k;
			input[dev].map_shown = player[k].map_shown;
			if (JOYCON_COMBINED(dev))
			{
				input[input[dev].bind].num = k;
				input[input[dev].bind].map_shown = player[k].map_shown;
			}

			memcpy(input[dev].jkmap, player[k].jkmap, sizeof(input[dev].jkmap));
			input[dev].lightgun = player[k].lightgun;
			break;
		}
	}

	update_num_hw(dev, input[dev].num);
}

/* Assign a player number to a device and persist the assignment.
 * force=1 overrides any existing assignment. */
void assign_player(int dev, int num, int force)
{
	input[dev].num = num;
	if (JOYCON_COMBINED(dev)) input[input[dev].bind].num = num;
	store_player(num, dev);
	printf("Device %s %sassigned to player %d\n", input[dev].id, force ? "forcebly " : "", input[dev].num);
}

void unflag_players()
{
	for (int k = 1; k < NUMPLAYERS; k++)
	{
		int found = 0;
		for (int i = 0; i < NUMDEV; i++) if (strlen(player_pad[k].id) && !strcmp(player_pad[k].id, input[i].id)) found = 1;
		if (!found) player_pad[k].map_shown = 0;
	}

	for (int k = 1; k < NUMPLAYERS; k++)
	{
		int found = 0;
		for (int i = 0; i < NUMDEV; i++) if (strlen(player_pdsp[k].id) && !strcmp(player_pdsp[k].id, input[i].id)) found = 1;
		if (!found) player_pdsp[k].map_shown = 0;
	}
}

/* ========================================================================
 *  Analog joystick dead zone configuration
 * ======================================================================== */

void setup_deadzone(struct input_event* ev, int dev)
{
	// Lightgun/wheel has no dead zone
	if (ev->type != EV_ABS || (ev->code <= 1 && (input[dev].lightgun || input[dev].quirk == QUIRK_WHEEL)))
	{
		input[dev].deadzone = 0U;
	}
	// Dual Shock 3/4
	else if (input[dev].quirk == QUIRK_DS3 || input[dev].quirk == QUIRK_DS4)
	{
		input[dev].deadzone = 10U;
	}
	// Default dead zone
	else
	{
		input[dev].deadzone = 2U;
	}

	char cfg_format[32];
	char cfg_uid[sizeof(*cfg.controller_deadzone)];

	snprintf(cfg_format, sizeof(cfg_format), "%%%u[^ \t,]%%*[ \t,]%%u%%n", (size_t)(sizeof(cfg_uid) - 1));

	const char* dev_uid = get_unique_mapping(dev, 1);

	for (size_t i = 0; i < sizeof(cfg.controller_deadzone) / sizeof(*cfg.controller_deadzone); i++)
	{
		const char* cfg_line = cfg.controller_deadzone[i];
		if (!cfg_line || !strlen(cfg_line)) break;

		uint32_t cfg_vidpid, cfg_deadzone;
		size_t scan_pos;
		char vp;

		if ((sscanf(cfg_line, cfg_format, cfg_uid, &cfg_deadzone, &scan_pos) < 2) ||
			(scan_pos != strlen(cfg_line))) continue;

		if ((
			sscanf(cfg_uid, "0%*[Xx]%08x%n", &cfg_vidpid, &scan_pos) ||
			sscanf(cfg_uid, "%08x%n", &cfg_vidpid, &scan_pos)) &&
			(scan_pos == strlen(cfg_uid)))
		{
			const uint32_t vidpid = (input[dev].vid << 16) | input[dev].pid;
			if (vidpid != cfg_vidpid) continue;
		}
		else if ((
			(sscanf(cfg_uid, "%[VvPp]%*[Ii]%*[Dd]:0%*[Xx]%04x%n", &vp, &cfg_vidpid, &scan_pos) == 2) ||
			(sscanf(cfg_uid, "%[VvPp]%*[Ii]%*[Dd]:%04x%n", &vp, &cfg_vidpid, &scan_pos) == 2)) &&
			(scan_pos == strlen(cfg_uid)))
		{
			if (vp == 'V' || vp == 'v')
			{
				if (input[dev].vid != cfg_vidpid) continue;
			}
			else
			{
				if (input[dev].pid != cfg_vidpid) continue;
			}
		}
		else if (
			!strcasestr(input[dev].id, cfg_uid) &&
			!strcasestr(input[dev].sysfs, cfg_uid) &&
			!strcasestr(dev_uid, cfg_uid))
		{
			continue;
		}

		if (cfg_deadzone > 64) cfg_deadzone = 64;

		printf("Analog device %s was given a dead zone of %u\n", input[dev].id, cfg_deadzone);
		input[dev].deadzone = cfg_deadzone;
		break;
	}
}

/* ========================================================================
 *  Device identity merging
 *
 *  Reads /proc/bus/input/devices to associate event and mouse device nodes
 *  that belong to the same physical controller. Also assigns controller
 *  quirks for spinner/paddle overlays and handles the "no merge" lists.
 * ======================================================================== */

void make_unique(uint16_t vid, uint16_t pid, int type)
{
	int cnt = 0;
	int lastmin = -1;
	int min;

	printf("make_unique(%04X,%04X,%d)\n", vid, pid, type);

	while(1)
	{
		int idx = -1;
		min = INT32_MAX;
		for (int i = 0; i < NUMDEV; i++)
		{
			if ((!type && (input[i].vid == vid)) ||
				(type > 0 && (input[i].vid == vid) && (input[i].pid == pid)) ||
				(type < 0 && (input[i].vid == vid) && (input[i].pid != pid)))
			{
				int num = -1;
				const char *n = strstr(input[i].devname, "/event");
				if (n) num = strtoul(n + 6, NULL, 10);
				if (num >= 0 && num < min && num > lastmin)
				{
					min = num;
					idx = i;
				}
			}
		}

		if (idx < 0) break;

		lastmin = min;
		sprintf(input[idx].id + strlen(input[idx].id), "/%d", cnt++);
	}
}

/* Merge multi-function device nodes (event + mouse) that share the same
 * physical path in /proc/bus/input/devices.  Also applies the no-merge
 * and spinner/paddle quirk lists. */
void mergedevs()
{
	for (int i = 0; i < NUMDEV; i++)
	{
		memset(input[i].id, 0, sizeof(input[i].id));
	}

	FILE *f = fopen("/proc/bus/input/devices", "r");
	if (!f)
	{
		printf("Failed to open /proc/bus/input/devices\n");
		return;
	}

	static char str[1024];
	char phys[64] = {};
	char uniq[64] = {};
	char id[64] = {};
	static char sysfs[512] = {};

	while (fgets(str, sizeof(str), f))
	{
		int len = strlen(str);
		while (len && str[len - 1] == '\n') str[--len] = 0;

		if (!len)
		{
			phys[0] = 0;
			uniq[0] = 0;
		}
		else
		{
			if (!strncmp("P: Phys", str, 7)) snprintf(phys, sizeof(phys), "%s", strchr(str, '=') + 1);
			if (!strncmp("U: Uniq", str, 7)) snprintf(uniq, sizeof(uniq), "%s", strchr(str, '=') + 1);
			if (!strncmp("S: Sysfs", str, 8)) snprintf(sysfs, sizeof(sysfs), "%s", strchr(str, '=') + 1);

			if (!strncmp("H: ", str, 3))
			{
				if (strlen(phys) && strlen(uniq)) snprintf(id, sizeof(id), "%s/%s", phys, uniq);
				else if (strlen(phys)) strcpy(id, phys);
				else strcpy(id, uniq);

				char *handlers = strchr(str, '=');
				if (handlers && id[0])
				{
					handlers++;
					for (int i = 0; i < NUMDEV; i++)
					{
						if (pool[i].fd >= 0)
						{
							char *dev = strrchr(input[i].devname, '/');
							if (dev)
							{
								char idsp[32];
								strcpy(idsp, dev + 1);
								strcat(idsp, " ");
								if (strstr(handlers, idsp))
								{
									strcpy(input[i].id, id);
									strcpy(input[i].sysfs, sysfs);
									strcpy(input[i].mac, uniq);

									input[i].unique_hash = str_hash(input[i].id);
									input[i].unique_hash = str_hash(input[i].mac, input[i].unique_hash);

									input[i].timeout = (strlen(uniq) && strstr(sysfs, "bluetooth")) ? (cfg.bt_auto_disconnect * 10) : 0;
								}
							}
						}
					}
				}
			}
		}
	}

	fclose(f);

	//Bypass merging of specified 2 port/player controllers
	make_unique(0x289B, 0x0057, -1); // Raphnet
	make_unique(0x0E8F, 0x3013, 1);  // Mayflash SNES controller 2 port adapter
	make_unique(0x16C0, 0x05E1, 1);  // XinMo XM-10 2 player USB Encoder
	make_unique(0x045E, 0x02A1, 1);  // Xbox 360 wireless receiver
	make_unique(0x8282, 0x3201, 1);  // Irken Labs JAMMA Expander / Mojo Retro Adapter
	make_unique(0x1209, 0xFACA, 1);  // ControllaBLE
	make_unique(0x16D0, 0x127E, 1);  // Reflex Adapt to USB
	make_unique(0x1209, 0x595A, 1);  // RetroZord adapter

	if (cfg.no_merge_vid)
	{
		make_unique(cfg.no_merge_vid, cfg.no_merge_pid, (cfg.no_merge_pid ? 1 : 0));
	}

	for (int i = 0; i < (int)cfg.no_merge_vidpid[0]; i++) make_unique(cfg.no_merge_vidpid[i + 1] >> 16, (uint16_t)(cfg.no_merge_vidpid[i + 1]), 1);

	// merge multifunctional devices by id
	for (int i = 0; i < NUMDEV; i++)
	{
		input[i].bind = i;
		if (input[i].id[0] && !input[i].mouse)
		{
			for (int j = 0; j < i; j++)
			{
				if (!strcmp(input[i].id, input[j].id))
				{
					input[i].bind = j;
					break;
				}
			}
		}
	}

	//copy missing fields to mouseX
	for (int i = 0; i < NUMDEV; i++) if (input[i].mouse)
	{
		for (int j = 0; j < NUMDEV; j++) if (!input[j].mouse)
		{
			if (!strcmp(input[i].id, input[j].id))
			{
				input[i].bind = j;
				input[i].vid = input[j].vid;
				input[i].pid = input[j].pid;
				input[i].version = input[j].version;
				input[i].bustype = input[j].bustype;
				input[i].quirk = input[j].quirk;
				memcpy(input[i].name, input[j].name, sizeof(input[i].name));
				memcpy(input[i].idstr, input[j].idstr, sizeof(input[i].idstr));

				if (!input[i].quirk)
				{
					//All mice as spinners
					if ((cfg.spinner_vid == 0xFFFF && cfg.spinner_pid == 0xFFFF)
						//Mouse as spinner
						|| (cfg.spinner_vid && cfg.spinner_pid && input[i].vid == cfg.spinner_vid && input[i].pid == cfg.spinner_pid))
					{
						input[i].quirk = QUIRK_MSSP;
						input[i].bind = i;
						input[i].spinner_prediv = 1;
					}

					//Arcade Spinner TS-BSP01 (X axis) and Atari (Y axis)
					if (input[i].vid == 0x32be && input[i].pid == 0x1420)
					{
						input[i].quirk = QUIRK_MSSP;
						input[i].bind = i;
						input[i].spinner_prediv = 3;
					}

					if (input[i].quirk == QUIRK_MSSP) strcat(input[i].id, "_sp");
				}
				break;
			}
		}
	}
}
