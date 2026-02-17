/*
 * input_internal.h - Internal shared definitions for the input subsystem.
 *
 * This header is intended only for use by the input subsystem implementation
 * files (input.cpp, input_scancode.cpp, input_dev.cpp, input_joy.cpp,
 * input_quirks.cpp).  External consumers should use input.h instead.
 *
 * Architecture overview
 * ---------------------
 *   input.h              Public API consumed by the rest of MiSTer.
 *   input_internal.h     (this file) Shared types, externs, and macros.
 *
 *   input.cpp            Main event loop (input_test / input_poll), the
 *                         heavyweight input_cb callback, uinput relay,
 *                         mapping-mode state machine, mouse aggregation.
 *   input_scancode.cpp   Scancode translation tables (Linux evdev -> Amiga,
 *                         PS/2 Set 1 & 2, Archimedes).
 *   input_dev.cpp        Device lifecycle: hotplug via inotify, merging by
 *                         /proc/bus/input/devices, player assignment, LED
 *                         control, button-map file I/O, lightgun calibration.
 *   input_joy.cpp        Digital/analog joystick routing to the FPGA core,
 *                         dead-zone processing, per-player key-state tracking,
 *                         autofire toggle.
 *   input_quirks.cpp     Per-device quirk handlers: Keyrah, touchscreen,
 *                         Atari VCS, JoyCon, OpenFIRE, rumble, steering
 *                         wheels, JAMMA encoders.
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
	QUIRK_WIIMOTE,          // Nintendo Wii Remote (light-gun via accelerometer)
	QUIRK_DS3,              // Sony DualShock 3  (axis remapping + larger deadzone)
	QUIRK_DS4,              // Sony DualShock 4  (axis remapping + larger deadzone)
	QUIRK_DS4TOUCH,         // DS4 touchpad exposed as a separate evdev node
	QUIRK_MADCATZ360,       // Mad Catz Xbox 360 fight-stick variant
	QUIRK_PDSP,             // Paddle / spinner overlay (dedicated event node)
	QUIRK_PDSP_ARCADE,      // Arcade-style spinner overlay
	QUIRK_JAMMA,            // JAMMA arcade encoder (primary mapping table)
	QUIRK_JAMMA2,           // JAMMA arcade encoder (secondary mapping table)
	QUIRK_MSSP,             // Mouse-as-spinner overlay
	QUIRK_TOUCHGUN,         // Touchscreen acting as a light-gun
	QUIRK_VCS,              // Atari VCS paddle / spinner controller
	QUIRK_JOYCON,           // Nintendo Switch Joy-Con (requires pairing/combining)
	QUIRK_LIGHTGUN_CRT,     // Serial CRT light-gun (e.g. GunCon, Stunner)
	QUIRK_LIGHTGUN,         // USB light-gun (absolute positioning)
	QUIRK_LIGHTGUN_MOUSE,   // Light-gun that also passes mouse events
	QUIRK_WHEEL,            // Steering wheel w/ force-feedback range setting
};

/* ========================================================================
 *  Per-device state
 * ======================================================================== */

typedef struct
{
	/* ---- identification (filled at open time from EVIOCGID / sysfs) ---- */
	uint16_t bustype, vid, pid, version;
	char     idstr[256];            // VID:PID string used as map-file key
	char     mod;                   // modifier variant flag (e.g. accent key)

	/* ---- capabilities -------------------------------------------------- */
	uint8_t  led;                   // device has LED indicators
	uint8_t  mouse;                 // true if this is a mouse/trackball node
	uint8_t  axis_edge[256];        // edge-trigger state per axis
	int8_t   axis_pos[256];         // last reported axis position

	/* ---- player number & per-core button mapping ----------------------- */
	uint8_t  num;                   // 1-based player number (0 = unassigned)
	uint8_t  has_map;               // per-core map loaded?
	uint32_t map[NUMBUTTONS];       // per-core button mapping
	int      map_shown;             // OSD mapping prompt already displayed?

	uint8_t  osd_combo;             // OSD combo key state

	/* ---- default (menu) button mapping --------------------------------- */
	uint8_t  has_mmap;              // default map loaded?
	uint32_t mmap[NUMBUTTONS];      // default (menu) mapping
	uint8_t  has_jkmap;             // per-core joystick-to-keyboard map loaded?
	uint16_t jkmap[1024];           // joy -> keyboard remapping table
	int      stick_l[2];            // left  stick axis indices [X, Y]
	int      stick_r[2];            // right stick axis indices [X, Y]

	/* ---- keyboard remapping -------------------------------------------- */
	uint8_t  has_kbdmap;
	uint8_t  kbdmap[256];           // scancode -> scancode remap

	/* ---- light-gun calibration ----------------------------------------- */
	int32_t  guncal[4];             // [x_off, y_off, x_scale, y_scale]

	/* ---- touchscreen / accelerometer state ----------------------------- */
	int      accx, accy;            // accumulated touch delta
	int      startx, starty;        // touch-down origin
	int      lastx, lasty;          // previous touch position
	int      quirk;                 // QUIRK_* enum identifying special handling

	/* ---- spinner / paddle state ---------------------------------------- */
	int      misc_flags;            // bitfield: JoyCon pairing flags, etc.
	int      paddle_val;            // last paddle ADC value
	int      spinner_prev;          // previous spinner position
	int      spinner_acc;           // accumulated spinner delta
	int      spinner_prediv;        // pre-divider for high-resolution spinners
	int      spinner_dir;           // last spin direction (+1 / -1)
	int      spinner_accept;        // axis ready for spinner input?
	int      old_btn;               // previous button state (VCS, etc.)
	int      ds_mouse_emu;          // DualShock trackpad -> mouse emulation

	/* ---- light-gun runtime -------------------------------------------- */
	int      lightgun_req;          // pending light-gun coordinate update?
	int      lightgun;              // light-gun mode enabled for this device

	/* ---- force-feedback / rumble --------------------------------------- */
	int      has_rumble;            // device supports FF_RUMBLE?
	int      rumble_en;             // rumble currently enabled?
	uint16_t last_rumble;           // last rumble magnitude sent
	ff_effect rumble_effect;        // uploaded FF effect

	/* ---- steering wheel axis mapping ----------------------------------- */
	int8_t   wh_steer;              // axis index: steering
	int8_t   wh_accel;              //              accelerator
	int8_t   wh_brake;              //              brake
	int8_t   wh_clutch;             //              clutch
	int8_t   wh_combo;              // combined pedal axis mode
	int8_t   wh_pedal_invert;       // invert pedal values

	/* ---- Bluetooth & enumeration -------------------------------------- */
	int      timeout;               // BT auto-disconnect countdown (0 = wired)
	char     mac[64];               // Bluetooth MAC (empty for wired)

	/* ---- device merging / multi-function binding ----------------------- */
	int      bind;                  // index of master device (self if unbound)
	uint32_t unique_hash;           // hash of id + mac for unique mapping
	char     devname[32];           // /dev/input/eventN path
	char     id[80];                // merged physical-path id
	char     name[128];             // human-readable name from EVIOCGNAME
	char     sysfs[512];            // sysfs path for this device

	/* ---- analog range tracking (for dead-zone & N64 emu) -------------- */
	int      ss_range[2];           // max sum-of-squares seen [stick0, stick1]
	int      max_cardinal[2];       // max cardinal distance   [stick0, stick1]
	float    max_range[2];          // sqrt(ss_range)          [stick0, stick1]

	uint32_t deadzone;              // dead-zone radius (0-64, cfgurable)
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
