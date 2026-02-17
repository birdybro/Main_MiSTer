/*
 * input_internal.h - Internal shared definitions for the input subsystem.
 *
 * This header is intended only for use by the input subsystem implementation
 * files (input.cpp, input_scancode.cpp, input_dev.cpp, input_joy.cpp,
 * input_quirks.cpp). External consumers should use input.h instead.
 */

#ifndef INPUT_INTERNAL_H
#define INPUT_INTERNAL_H

#include <stdint.h>
#include <sys/poll.h>
#include <linux/input.h>

#include "input.h"

/* ========================================================================
 *  Constants
 * ======================================================================== */

#define NUMDEV         30
#define UINPUT_NAME    "MiSTer virtual input"

#define MAX_KEY_STATES 128
#define BTN_TGL        100
#define BTN_OSD        101

/* ========================================================================
 *  Device quirk identifiers
 * ======================================================================== */

enum QUIRK
{
	QUIRK_NONE = 0,
	QUIRK_WIIMOTE,
	QUIRK_DS3,
	QUIRK_DS4,
	QUIRK_DS4TOUCH,
	QUIRK_MADCATZ360,
	QUIRK_PDSP,
	QUIRK_PDSP_ARCADE,
	QUIRK_JAMMA,
	QUIRK_JAMMA2,
	QUIRK_MSSP,
	QUIRK_TOUCHGUN,
	QUIRK_VCS,
	QUIRK_JOYCON,
	QUIRK_LIGHTGUN_CRT,
	QUIRK_LIGHTGUN,
	QUIRK_LIGHTGUN_MOUSE,
	QUIRK_WHEEL,
};

/* ========================================================================
 *  Per-device state
 * ======================================================================== */

typedef struct
{
	uint16_t bustype, vid, pid, version;
	char     idstr[256];
	char     mod;

	uint8_t  led;
	uint8_t  mouse;
	uint8_t  axis_edge[256];
	int8_t   axis_pos[256];

	uint8_t  num;
	uint8_t  has_map;
	uint32_t map[NUMBUTTONS];
	int      map_shown;

	uint8_t  osd_combo;

	uint8_t  has_mmap;
	uint32_t mmap[NUMBUTTONS];
	uint8_t  has_jkmap;
	uint16_t jkmap[1024];
	int      stick_l[2];
	int      stick_r[2];

	uint8_t  has_kbdmap;
	uint8_t  kbdmap[256];

	int32_t  guncal[4];

	int      accx, accy;
	int      startx, starty;
	int      lastx, lasty;
	int      quirk;

	int      misc_flags;
	int      paddle_val;
	int      spinner_prev;
	int      spinner_acc;
	int      spinner_prediv;
	int      spinner_dir;
	int      spinner_accept;
	int      old_btn;
	int      ds_mouse_emu;

	int      lightgun_req;
	int      lightgun;

	int      has_rumble;
	int      rumble_en;
	uint16_t last_rumble;
	ff_effect rumble_effect;

	int8_t   wh_steer;
	int8_t   wh_accel;
	int8_t   wh_brake;
	int8_t   wh_clutch;
	int8_t   wh_combo;
	int8_t   wh_pedal_invert;

	int      timeout;
	char     mac[64];

	int      bind;
	uint32_t unique_hash;
	char     devname[32];
	char     id[80];
	char     name[128];
	char     sysfs[512];

	int      ss_range[2];
	int      max_cardinal[2];
	float    max_range[2];

	uint32_t deadzone;
} devInput;

/* ========================================================================
 *  Per-player key state tracking
 * ======================================================================== */

struct KeyStates {
	uint32_t key[MAX_KEY_STATES];
	uint32_t mask[MAX_KEY_STATES];
	uint32_t frames_held[MAX_KEY_STATES];
	int count;
};

/* ========================================================================
 *  Shared state  (defined in input.cpp)
 * ======================================================================== */

extern struct pollfd pool[];
extern devInput input[];
extern int grabbed;

/* Mouse */
extern unsigned char mouse_btn;
extern unsigned char mice_btn;
extern int mouse_emu;
extern int mouse_sniper;
extern int mouse_emu_x;
extern int mouse_emu_y;
extern uint32_t mouse_timer;

/* Keyboard */
extern int kbd_toggle;

/* Key state tracking */
extern KeyStates key_states[];

/* Mapping */
extern int mapping;
extern int mapping_dev;
extern int mapping_type;
extern int mapping_button;
extern int mapping_set;
extern uint32_t osd_timer;
extern uint32_t osdbtn;

/* ========================================================================
 *  Convenience macros
 * ======================================================================== */

#define JOYCON_COMBO(dev)    (input[(dev)].misc_flags & (1 << 31))
#define JOYCON_LEFT(dev)     (input[(dev)].misc_flags & (1 << 30))
#define JOYCON_RIGHT(dev)    (input[(dev)].misc_flags & (1 << 29))
#define JOYCON_REQ(dev)      ((input[(dev)].misc_flags & 7) == 7)
#define JOYCON_COMBINED(dev) (input[(dev)].quirk == QUIRK_JOYCON && JOYCON_COMBO((dev)))

#define BTN_NUM (sizeof(devInput::map) / sizeof(devInput::map[0]))

#define test_bit(bit, array)  (array [bit / 8] & (1 << (bit % 8)))

/* ========================================================================
 *  Cross-module function declarations
 * ======================================================================== */

/* --- input.cpp (main orchestrator) ------------------------------------ */
void input_cb(struct input_event *ev, struct input_absinfo *absinfo, int dev);
void mouse_cb(int16_t x = 0, int16_t y = 0, int16_t w = 0);
void mouse_btn_req();
void uinp_send_key(uint16_t key, int press);

/* --- input_dev.cpp (device management) -------------------------------- */
int  set_watch();
int  check_devs();
void INThandler(int code);
char has_led(int fd);
char *get_led_path(int dev, int add_id = 1);
int  set_led(char *base, const char *led, int brightness);
int  get_led(char *base, const char *led);
void update_num_hw(int dev, int num);
void store_player(int num, int dev);
void restore_player(int dev);
void assign_player(int dev, int num, int force = 0);
void setup_deadzone(struct input_event *ev, int dev);
void unflag_players();
int  load_map(const char *name, void *pBuffer, int size);
void delete_map(const char *name);
int  save_map(const char *name, void *pBuffer, int size);
char *get_unique_mapping(int dev, int force_unique = 0);
char *get_map_name(int dev, int def);
char *get_jkmap_name(int dev);
char *get_kbdmap_name(int dev);
void mergedevs();
void make_unique(uint16_t vid, uint16_t pid, int type);
void input_lightgun_load(int idx);

extern uint16_t def_mmap[];

/* --- input_joy.cpp (joystick / autofire processing) ------------------- */
void     joy_digital(int jnum, uint32_t mask, uint32_t code, char press, int bnum, int dont_save = 0);
void     joy_analog(int dev, int axis, int offset, int stick = 0);
void     key_update_frames_held();
uint32_t build_joy_mask(int player);
uint32_t build_autofire_mask(int player);

/* --- input_quirks.cpp (device-specific handlers) ---------------------- */
int  keyrah_trans(int key, int press);
void touchscreen_proc(int dev, input_event *ev);
int  vcs_proc(int dev, input_event *ev);
int  process_joycon(int dev, input_event *ev, input_absinfo *absinfo);
void check_joycon();
void openfire_signal();
void setup_wheels();
void set_rumble(int dev, uint16_t rumble_val);
int  get_rumble_device(int player);
int  rumble_input_device(int devnum, uint16_t strong_mag, uint16_t weak_mag, uint16_t duration = 500, uint16_t delay = 0);
void send_mouse_with_throttle(int dev, int xval, int yval, int8_t wval);
void set_wheel_range(int dev, int range);
void check_touch_release();

extern uint32_t touch_rel;

/* JAMMA mapping table entry */
struct jamma_map_entry {
	uint16_t key;
	uint16_t player;
	uint16_t btn;
};

extern struct jamma_map_entry jamma2joy[];
extern const int jamma2joy_count;
extern struct jamma_map_entry jamma22joy[];
extern const int jamma22joy_count;

#endif /* INPUT_INTERNAL_H */
