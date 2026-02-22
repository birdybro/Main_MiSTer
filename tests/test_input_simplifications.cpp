// test_input_simplifications.cpp
// Validates that each simplification made in input.cpp preserves exact semantics.
// Compile: g++ -std=c++14 -Wall -Wextra -o tests/test_input_simplifications tests/test_input_simplifications.cpp
// Run:     ./tests/test_input_simplifications

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        tests_failed++; \
        fprintf(stderr, "FAIL [line %d]: %s\n", __LINE__, msg); \
    } \
} while(0)

// ─── L1090: array selection in get_ps2_code ──────────────────────────────────
// Before: (ps2_kbd_scan_set == 1) ? ev2ps2_set1[key] : ev2ps2[key]
// After:  (ps2_kbd_scan_set == 1 ? ev2ps2_set1 : ev2ps2)[key]

static void test_array_selection()
{
    static int arr1[256], arr2[256];
    for (int i = 0; i < 256; i++) {
        arr1[i] = i * 2;
        arr2[i] = i * 3 + 1;
    }

    for (int scan_set = 0; scan_set <= 2; scan_set++) {
        for (int key = 0; key < 256; key++) {
            int old_result = (scan_set == 1) ? arr1[key] : arr2[key];
            int new_result = (scan_set == 1 ? arr1 : arr2)[key];
            ASSERT(old_result == new_result,
                   "L1090: array selection produces different result");
        }
    }
}

// ─── L1340, L1351: expr ? 1 : 0  →  !!expr ──────────────────────────────────
// has_led:    return test_bit(EV_LED, evtype_b) ? 1 : 0;
// get_kbdled: return (leds_state & (mask&HID_LED_MASK)) ? 1 : 0;

static void test_boolean_conversion()
{
    // Signed values (test_bit returns a char)
    for (int v = -128; v <= 127; v++) {
        int old_result = v ? 1 : 0;
        int new_result = !!v;
        ASSERT(old_result == new_result,
               "L1340/L1351: signed boolean conversion differs");
    }

    // Unsigned byte values (leds_state & mask)
    for (unsigned int v = 0; v <= 255; v++) {
        int old_result = (int)v ? 1 : 0;
        int new_result = !!(int)v;
        ASSERT(old_result == new_result,
               "L1351: unsigned byte boolean conversion differs");
    }
}

// ─── L1874: redundant outer parens ───────────────────────────────────────────
// Before: if ((lastcode[num] && lastmask[num] && (lastmask[num] & 0xF) == 0))
// After:  if (lastcode[num] && lastmask[num] && (lastmask[num] & 0xF) == 0)

static void test_redundant_parens()
{
    const uint32_t codes[] = { 0, 1, 0xDEAD, 0xBEEF };
    const uint32_t masks[] = { 0, 1, 0x0F, 0x10, 0x1F, 0xFF, 0x100 };

    for (uint32_t lastcode : codes) {
        for (uint32_t lastmask : masks) {
            bool old_result = (lastcode && lastmask && (lastmask & 0xF) == 0);
            bool new_result =  lastcode && lastmask && (lastmask & 0xF) == 0;
            ASSERT(old_result == new_result,
                   "L1874: outer-paren removal changed evaluation");
        }
    }
}

// ─── L4265: strstr(...) == NULL  →  !strstr(...) ─────────────────────────────

static void test_null_pointer_check()
{
    struct { const char *hay; const char *needle; } cases[] = {
        { "OpenFIRE mouse device", "mouse" },
        { "OpenFIRE gamepad",      "mouse" },
        { "",                       "mouse" },
        { "mousepad",              "mouse" },
    };

    for (auto &c : cases) {
        const char *r = strstr(c.hay, c.needle);
        bool old_result = (r == NULL);
        bool new_result = !r;
        ASSERT(old_result == new_result,
               "L4265: == NULL vs ! differs");
    }
}

// ─── L4268-4269: nameEnd replaces nameInit + repeated strlen ─────────────────
// Before: char *nameInit = name;
//         memcmp(nameInit+strlen(name)-5, "Mouse",    5)
//         memcmp(nameInit+strlen(name)-8, "Keyboard", 8)
// After:  char *nameEnd = name + strlen(name);
//         memcmp(nameEnd-5, "Mouse",    5)
//         memcmp(nameEnd-8, "Keyboard", 8)

static void test_strlen_caching()
{
    const char *names[] = {
        "OpenFIRE Light Gun Mouse",
        "OpenFIRE Light Gun Keyboard",
        "OpenFIRE Light Gun",
        "SomeOtherDevice",
        "12345Mouse",
        "12345Keyboard",
        "ABCDEFGH",          // exactly 8 chars
    };

    for (const char *name : names) {
        if (strlen(name) < 8) continue;

        // Old: nameInit alias + strlen called twice
        const char *nameInit = name;
        int old_mouse = memcmp(nameInit + strlen(name) - 5, "Mouse",    5);
        int old_kbd   = memcmp(nameInit + strlen(name) - 8, "Keyboard", 8);

        // New: nameEnd computed once
        const char *nameEnd = name + strlen(name);
        int new_mouse = memcmp(nameEnd - 5, "Mouse",    5);
        int new_kbd   = memcmp(nameEnd - 8, "Keyboard", 8);

        ASSERT(old_mouse == new_mouse,
               "L4269: nameEnd mouse suffix check differs");
        ASSERT(old_kbd == new_kbd,
               "L4269: nameEnd keyboard suffix check differs");
    }
}

// ─── L4426: if (!(++id)) ++id;  →  if (++id == 0) ++id; ─────────────────────
// uint8_t wraps 255→0; the guard skips 0 by incrementing again to 1.

static void test_skip_zero_increment()
{
    for (int initial = 0; initial <= 255; initial++) {
        uint8_t id_old = (uint8_t)initial;
        uint8_t id_new = (uint8_t)initial;

        // Old
        if (!(++id_old)) ++id_old;

        // New
        if (++id_new == 0) ++id_new;

        ASSERT(id_old == id_new,
               "L4426: skip-zero increment produces different id");
    }
}

// ─── L4473: !(fd >= 0)  →  fd < 0 ───────────────────────────────────────────

static void test_fd_negative_check()
{
    const int fds[] = { INT_MIN, -100, -2, -1, 0, 1, 2, 100, INT_MAX };

    for (int fd : fds) {
        bool old_result = !(fd >= 0);
        bool new_result = fd < 0;
        ASSERT(old_result == new_result,
               "L4473: fd negative check differs");
    }
}

// ─── L1463: return (expr)  →  return expr ────────────────────────────────────
// Outer parens on a return value are purely syntactic noise.

static void test_return_parens()
{
    // Any value; parens never affect the returned expression.
    for (int mapping = 0; mapping <= 1; mapping++) {
        for (int osd_timer = 0; osd_timer <= 1; osd_timer++) {
            // Simulate: return (mapping && osd_timer);  vs  return mapping && osd_timer;
            int old_result = (mapping && osd_timer);
            int new_result =  mapping && osd_timer;
            ASSERT(old_result == new_result,
                   "L1463: return-paren removal changed result");
        }
    }
}

// ─── L1581: (has_mmap == 1)  →  has_mmap == 1 ───────────────────────────────
// Inner parens around a comparison inside a ternary branch are redundant.

static void test_inner_parens_ternary()
{
    for (int mapping_dev = -1; mapping_dev <= 1; mapping_dev++) {
        int has_mmap = (mapping_dev >= 0) ? 1 : 0;  // plausible value
        int old_result = (mapping_dev >= 0) ? (has_mmap == 1) : 0;
        int new_result = (mapping_dev >= 0) ?  has_mmap == 1  : 0;
        ASSERT(old_result == new_result,
               "L1581: inner-paren removal changed ternary result");
    }
}

// ─── L1631: fn = press ? 1 : 0  →  fn = !!press ─────────────────────────────
// press is a key event value (0=release, 1=press, 2=repeat).

static void test_fn_press_conversion()
{
    // Key event values: 0, 1, 2
    for (int press = 0; press <= 2; press++) {
        int old_fn = press ? 1 : 0;
        int new_fn = !!press;
        ASSERT(old_fn == new_fn,
               "L1631: fn press conversion differs");
    }
}

// ─── L2969: mapping_type = (ev->code >= 256) ? 1 : 0  →  ev->code >= 256 ────
// C/C++ comparison operators already produce exactly 0 or 1.

static void test_comparison_already_bool()
{
    // Simulate ev->code as uint16_t spanning interesting boundary
    for (int code = 250; code <= 260; code++) {
        int old_result = (code >= 256) ? 1 : 0;
        int new_result =  code >= 256;
        ASSERT(old_result == new_result,
               "L2969: mapping_type comparison-as-bool differs");
    }
    // Full uint16_t range at the extremes
    for (int code : {0, 255, 256, 1023, 65535}) {
        int old_result = (code >= 256) ? 1 : 0;
        int new_result =  code >= 256;
        ASSERT(old_result == new_result,
               "L2969: mapping_type comparison-as-bool differs (extreme)");
    }
}

// ─── L4216: spinner_dir = (diff > 0) ? 1 : 0  →  diff > 0 ──────────────────

static void test_spinner_dir_conversion()
{
    const int diffs[] = { -100, -6, -1, 0, 1, 6, 100 };
    for (int diff : diffs) {
        int old_dir = (diff > 0) ? 1 : 0;
        int new_dir =  diff > 0;
        ASSERT(old_dir == new_dir,
               "L4216: spinner_dir conversion differs");
    }
}

// ─── L4285: deviceFile == NULL  →  !deviceFile ───────────────────────────────

static void test_file_null_check()
{
    void *ptrs[] = { nullptr, (void*)1, (void*)0xDEAD };
    for (void *p : ptrs) {
        bool old_result = (p == nullptr);
        bool new_result = !p;
        ASSERT(old_result == new_result,
               "L4285: FILE* == NULL vs ! differs");
    }
}

// ─── L5007-5008: second nameEnd strlen caching site ──────────────────────────
// Identical transformation to L4268-4269; shared by test_strlen_caching().

// ─── L5126 / L6014: (expr) ? 1 : 0  →  !!expr  in ioctl EVIOCGRAB arg ───────
// grabbed is int, user_io_osd_is_visible() returns int; OR then bool-ify.

static void test_grab_bool_conversion()
{
    const int grabbed_vals[]  = { 0, 1, 2, 0xFF };
    const int visible_vals[]  = { 0, 1 };

    for (int grabbed : grabbed_vals) {
        for (int visible : visible_vals) {
            int combined = grabbed | visible;
            int old_result = combined ? 1 : 0;
            int new_result = !!combined;
            ASSERT(old_result == new_result,
                   "L5126/L6014: EVIOCGRAB bool conversion differs");
        }
    }
}

// ─── L5747: (data[0] & 7) ? 1 : 0  →  !!(data[0] & 7) ──────────────────────

static void test_byte_mask_conversion()
{
    for (unsigned int byte = 0; byte <= 255; byte++) {
        int old_result = (byte & 7) ? 1 : 0;
        int new_result = !!(byte & 7);
        ASSERT(old_result == new_result,
               "L5747: byte mask bool conversion differs");
    }
}

// ─── L5841/5845/5849: LED bitmask ? 1 : 0  →  !! ────────────────────────────
// HID_LED_SCROLL_LOCK=4, HID_LED_NUM_LOCK=1, HID_LED_CAPS_LOCK=2

static void test_led_bool_conversion()
{
    const int HID_LED_SCROLL_LOCK = 4;
    const int HID_LED_NUM_LOCK    = 1;
    const int HID_LED_CAPS_LOCK   = 2;

    for (int cur_leds = 0; cur_leds <= 7; cur_leds++) {
        // Scroll lock
        int old_sl = (cur_leds & HID_LED_SCROLL_LOCK) ? 1 : 0;
        int new_sl = !!(cur_leds & HID_LED_SCROLL_LOCK);
        ASSERT(old_sl == new_sl, "L5841: SCROLL_LOCK LED bool conversion differs");

        // Num lock
        int old_nl = (cur_leds & HID_LED_NUM_LOCK) ? 1 : 0;
        int new_nl = !!(cur_leds & HID_LED_NUM_LOCK);
        ASSERT(old_nl == new_nl, "L5845: NUM_LOCK LED bool conversion differs");

        // Caps lock
        int old_cl = (cur_leds & HID_LED_CAPS_LOCK) ? 1 : 0;
        int new_cl = !!(cur_leds & HID_LED_CAPS_LOCK);
        ASSERT(old_cl == new_cl, "L5849: CAPS_LOCK LED bool conversion differs");
    }
}

// ─── L1874: (lastmask & 0xF) == 0  →  !(lastmask & 0xF) ─────────────────────
// Checking "no direction bits set" expressed as bitwise-zero test.

static void test_bitmask_zero_check_direction()
{
    // lastmask values covering all direction-bit patterns
    const uint32_t masks[] = { 0, 1, 2, 4, 8, 0xF, 0x10, 0xFF, 0x100, 0xDEAD };
    for (uint32_t lastmask : masks) {
        bool old_result = (lastmask & 0xF) == 0;
        bool new_result = !(lastmask & 0xF);
        ASSERT(old_result == new_result,
               "L1874: direction-bits zero check differs");
    }
}

// ─── L5262: (misc_flags & 0x6) == 0  →  !(misc_flags & 0x6) ─────────────────
// Same pattern: checking two mode bits are both clear.

static void test_bitmask_zero_check_misc_flags()
{
    for (int flags = 0; flags <= 0xF; flags++) {
        bool old_result = (flags & 0x6) == 0;
        bool new_result = !(flags & 0x6);
        ASSERT(old_result == new_result,
               "L5262: misc_flags zero check differs");
    }
}

// ─── L5863: mask[k] != 0  →  mask[k] ────────────────────────────────────────
// Removing explicit != 0 on an integer used as a truth value.

static void test_mask_nonzero_check()
{
    const uint32_t masks[] = { 0, 1, 2, 0xFF, 0x100, UINT32_MAX };
    for (uint32_t mask : masks) {
        bool old_result = mask != 0;
        bool new_result = !!mask;  // direct truthiness: (bool)mask
        ASSERT(old_result == new_result,
               "L5863: mask != 0 vs direct truthiness differs");
    }
}

// ─── L5929: x = x | y  →  x |= y ────────────────────────────────────────────
// Compound assignment is semantically identical to the expanded form.

static void test_compound_or_assignment()
{
    const uint32_t base_vals[]    = { 0, 1, 0xF0, 0xFF, 0xDEAD };
    const uint32_t overlay_vals[] = { 0, 1, 0x0F, 0xFF, 0xBEEF };

    for (uint32_t base : base_vals) {
        for (uint32_t overlay : overlay_vals) {
            uint32_t x_old = base;
            uint32_t x_new = base;

            // Old form
            x_old = x_old | overlay;

            // New form
            x_new |= overlay;

            ASSERT(x_old == x_new,
                   "L5929: x = x | y vs x |= y differs");
        }
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main()
{
    printf("Running input.cpp simplification tests...\n\n");

    // Round 1
    test_array_selection();      // L1090
    test_boolean_conversion();   // L1340, L1351
    test_redundant_parens();     // L1874
    test_null_pointer_check();   // L4265
    test_strlen_caching();       // L4268-4269 (and L5007-5008, same logic)
    test_skip_zero_increment();  // L4426
    test_fd_negative_check();    // L4473

    // Round 2
    test_return_parens();            // L1463
    test_inner_parens_ternary();     // L1581
    test_fn_press_conversion();      // L1631
    test_comparison_already_bool();  // L2969
    test_spinner_dir_conversion();   // L4216
    test_file_null_check();          // L4285
    test_grab_bool_conversion();     // L5126, L6014
    test_byte_mask_conversion();     // L5747
    test_led_bool_conversion();      // L5841, L5845, L5849

    // Round 3
    test_bitmask_zero_check_direction();   // L1874
    test_bitmask_zero_check_misc_flags();  // L5262
    test_mask_nonzero_check();             // L5863
    test_compound_or_assignment();         // L5929

    printf("Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED\n", tests_failed);
        return 1;
    }
    printf(", all passed\n");
    return 0;
}
