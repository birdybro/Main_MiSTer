# Modem Emulation Analysis — Main_MiSTer

## Executive Summary

The Main_MiSTer codebase implements **modem mode as UART mode 4** within a broader UART/serial subsystem. However, Main_MiSTer itself does **not** contain any modem protocol emulation (no AT command parsing, no Hayes command set, no RING/CONNECT/CARRIER responses). It provides the **configuration UI and FPGA-side UART bridging**, while the actual modem emulation logic is delegated to an external utility (`/sbin/mlinkutil`) that is not part of this repository.

---

## UART Mode System Overview

The codebase defines seven UART modes, of which modem is mode 4:

| Mode | Name    | Description                              |
|------|---------|------------------------------------------|
| 0    | None    | UART disabled                            |
| 1    | PPP     | Point-to-Point Protocol                  |
| 2    | Console | Serial console                           |
| 3    | MIDI    | Musical instrument digital interface     |
| **4**| **Modem** | **Modem emulation (TCP/UDP/USB Serial)** |
| 5    | UDP     | UDP mode (not selectable via menu)       |
| 6    | SNI     | SNES-specific (requires snid binary)     |

**Source:** `menu.cpp:251`
```c
const char *config_uart_msg[] = { "      None", "       PPP", "   Console", "      MIDI", "     Modem", "UDP", "SNI"};
```

---

## Modem-Related Code Locations

### 1. UART Mode Detection — `user_io.cpp:1129-1139`

Mode state is tracked via sentinel files in `/tmp/`. When modem mode is active, `/tmp/uartmode4` exists.

```c
int GetUARTMode()
{
    struct stat filestat;
    if (!stat("/tmp/uartmode1", &filestat)) return 1;
    if (!stat("/tmp/uartmode2", &filestat)) return 2;
    if (!stat("/tmp/uartmode3", &filestat)) return 3;
    if (!stat("/tmp/uartmode4", &filestat)) return 4;  // Modem
    if (!stat("/tmp/uartmode5", &filestat)) return 5;
    if (!stat("/tmp/uartmode6", &filestat)) return 6;
    return 0;
}
```

### 2. UART Mode Activation — `user_io.cpp:1141-1162`

When modem mode is set, the function sends an SPI command (`UIO_SET_UART`, defined as `0x3B` in `user_io.h:69`) to the FPGA core with the mode and baud rate, then invokes the external `uartmode` script.

```c
void SetUARTMode(int mode)
{
    mode &= 0xF;
    uint32_t baud = GetUARTbaud(mode);

    spi_uio_cmd_cont(UIO_SET_UART);
    spi_w((mode == 4 || mode == 5) ? 1 : mode);  // Modem & UDP both send SPI mode 1
    spi_w(baud);
    spi_w(baud >> 16);
    DisableIO();

    MakeFile("/tmp/CORENAME", user_io_get_core_name());
    MakeFile("/tmp/RBFNAME", user_io_get_core_name(1));

    char data[20];
    sprintf(data, "%d", baud);
    MakeFile("/tmp/UART_SPEED", data);

    char cmd[32];
    sprintf(cmd, "uartmode %d", mode);
    system(cmd);  // Launches external uartmode script
}
```

**Key detail:** Modes 4 (Modem) and 5 (UDP) both transmit SPI value `1` to the FPGA, meaning they share the same FPGA-side UART implementation. The differentiation happens entirely on the ARM/Linux side through the transport layer (MidiLink mode).

### 3. Transport Layer Selection (MidiLink Modes) — `user_io.cpp:1236-1274`

When UART mode is Modem (4), the "MidiLink mode" controls the network transport backend:

| MidiLink Mode | Flag File          | Transport    | Available for Modem? |
|---------------|--------------------|--------------|----------------------|
| 0             | `/tmp/ML_FSYNTH`   | FluidSynth   | No (MIDI only)       |
| 1             | `/tmp/ML_MUNT`     | MUNT         | No (MIDI only)       |
| 2             | `/tmp/ML_USBMIDI`  | USB MIDI     | No (MIDI only)       |
| 3             | `/tmp/ML_UDP`      | UDP          | No (MIDI only)       |
| **4**         | **`/tmp/ML_TCP`**  | **TCP**      | **Yes (default)**    |
| **5**         | **`/tmp/ML_UDP_ALT`** | **UDP**   | **Yes**              |
| **6**         | **`/tmp/ML_USBSER`** | **USB Serial** | **Yes (if /dev/ttyUSB0 exists)** |

```c
int GetMidiLinkMode()
{
    struct stat filestat;
    if (!stat("/tmp/ML_FSYNTH", &filestat))  return 0;
    if (!stat("/tmp/ML_MUNT", &filestat))    return 1;
    if (!stat("/tmp/ML_USBMIDI", &filestat)) return 2;
    if (!stat("/tmp/ML_UDP", &filestat))     return 3;
    if (!stat("/tmp/ML_TCP", &filestat))     return 4;
    if (!stat("/tmp/ML_UDP_ALT", &filestat)) return 5;
    if (!stat("/tmp/ML_USBSER", &filestat))  return 6;
    return 0;
}
```

When setting USB Serial mode, the code falls back to TCP if `/dev/ttyUSB0` is not present (`user_io.cpp:1262`):
```c
if (mode == 6 && stat("/dev/ttyUSB0", &filestat)) mode = 4;
```

### 4. Modem Mode Validation on Core Load — `user_io.cpp:1708-1714`

When a core is loaded, the saved UART configuration is restored. The code ensures modem mode (4) is only paired with valid MidiLink modes (4-6):

```c
int midilink = (mode >> 8) & 0xFF;
int uartmode = mode & 0xFF;
if (uartmode == 4 && (midilink < 4 || midilink > 6)) midilink = 4;  // Force TCP for modem
if (uartmode == 3 && midilink > 3) midilink = 0;
if (uartmode < 3 || uartmode > 4) midilink = 0;
SetMidiLinkMode(midilink);
SetUARTMode(uartmode);
```

### 5. OSD Menu UI for Modem — `menu.cpp:3488-3544`

The UART settings menu renders modem-specific options when mode 4 is active:

```c
if (mode == 4)
{
    sprintf(s, " Link:            %s",
        (midilink == 6) ? "USB Serial" :
        (midilink == 5) ? "       UDP" :
                          "       TCP");
    OsdWrite(m++, s, menusub == 1);
    menumask |= 2;
}
```

The user can cycle through TCP, UDP, and USB Serial transport options via the OSD.

### 6. Modem Link Cycling — `menu.cpp:3564-3583`

When the user navigates the Link sub-menu for modem mode, the MidiLink mode cycles through 4 (TCP) -> 5 (UDP) -> 6 (USB Serial) and back:

```c
int midilink = GetMidiLinkMode();
SetUARTMode(0);
if (minus)
{
    if (midilink <= 4) midilink = 6;
    else midilink--;
}
else
{
    if (midilink >= 6) midilink = 4;
    else midilink++;
}
SetMidiLinkMode(midilink);
SetUARTMode(mode);
```

### 7. UART Mode Selection Menu — `menu.cpp:3678-3748`

The UART mode selection screen conditionally shows Modem as an option. The mode is always available (unlike UDP which is hidden unless already active, or SNI which requires SNES core + snid binary).

When switching to modem mode, the code automatically adjusts the MidiLink transport:
```c
if (menusub == 4) midilink = (midilink == 2) ? 6 : 4;  // USB MIDI -> USB Serial, else TCP
```

### 8. Baud Rate Configuration — `user_io.cpp:658-659`, `menu.cpp:3762-3833`

Modem mode uses the `mlink_speeds` baud rate table (shared with MIDI modes >= 3):

```c
static const uint32_t mlink_speeds[13] = {
    110, 300, 600, 1200, 2400, 4800, 9600,
    14400, 19200, 31250, 38400, 57600, 115200
};
```

Baud rate changes for modem mode are sent to the external `mlinkutil` utility (`menu.cpp:3818`):
```c
sprintf(s, "/sbin/mlinkutil BAUD %d", GetUARTbaud(GetUARTMode()));
system(s);
```

### 9. Configuration Persistence — `menu.cpp:3654-3665`

UART mode and MidiLink mode are saved per-core as a combined 32-bit value (MidiLink in upper byte, UART mode in lower byte):

```c
int mode = GetUARTMode() | (GetMidiLinkMode() << 8);
sprintf(s, "uartmode.%s", user_io_get_core_name());
FileSaveConfig(s, &mode, 4);
```

Baud rates are saved separately per-core:
```c
uint32_t speeds[3];
speeds[0] = GetUARTbaud(1);   // PPP/Console baud
speeds[1] = GetUARTbaud(3);   // MIDI baud
speeds[2] = GetUARTbaud(4);   // Modem/MLink baud
sprintf(s, "uartspeed.%s", user_io_get_core_name());
FileSaveConfig(s, speeds, sizeof(speeds));
```

### 10. UART Reset — `user_io.cpp:1276-1287`

Restarting the modem connection cycles the UART mode off and back on:

```c
void ResetUART()
{
    if (uart_mode)
    {
        int mode = GetUARTMode();
        if (mode != 0)
        {
            SetUARTMode(0);
            SetUARTMode(mode);
        }
    }
}
```

---

## Communication Flow

```
┌─────────────────────┐     SPI (UIO_SET_UART 0x3B)     ┌──────────────────┐
│   Main_MiSTer ARM   │ ──────────────────────────────── │   FPGA Core      │
│   (this codebase)   │     UART data over SPI           │   (UART module)  │
└─────────┬───────────┘                                  └──────────────────┘
          │
          │  system("uartmode 4")
          │  system("/sbin/mlinkutil BAUD ...")
          ▼
┌─────────────────────┐     TCP/UDP/USB Serial           ┌──────────────────┐
│   /sbin/uartmode    │ ──────────────────────────────── │   Remote Host    │
│   /sbin/mlinkutil   │     (network transport)          │   (BBS, server)  │
│   (NOT in this repo)│                                  └──────────────────┘
└─────────────────────┘
```

---

## What Is NOT in This Repository

The following modem emulation functionality is **absent** from Main_MiSTer and is expected to reside in the external `/sbin/mlinkutil` and `/sbin/uartmode` utilities:

- **AT command parsing** (ATD, ATH, ATA, ATS, ATZ, etc.)
- **Hayes modem command set** implementation
- **Modem response generation** (OK, CONNECT, RING, NO CARRIER, BUSY, etc.)
- **Dial-up negotiation** and handshake simulation
- **TCP socket-to-serial bridging** (the actual "virtual modem" logic)
- **Telnet protocol handling** (IAC negotiation for BBS connectivity)
- **PPP protocol implementation** (mode 1 also delegates externally)

---

## Summary of Files Involved

| File | Lines | Role |
|------|-------|------|
| `user_io.cpp` | 1129-1287 | UART/modem mode get/set, MidiLink mode management, baud rate handling, FPGA SPI commands |
| `user_io.h` | 69, 250-259 | `UIO_SET_UART` constant (0x3B), function declarations |
| `menu.cpp` | 251-252 | Mode label strings and MidiLink mode labels |
| `menu.cpp` | 3488-3544 | OSD menu rendering for modem configuration |
| `menu.cpp` | 3547-3674 | Menu interaction handlers (link cycling, baud rate, save, reset) |
| `menu.cpp` | 3678-3748 | UART mode selection submenu |
| `menu.cpp` | 3762-3833 | Baud rate selection submenu |
