# Hayes Modem Command Set — Implementation Plan for Main_MiSTer

## Background

Currently, modem emulation (UART mode 4) in Main_MiSTer is a thin configuration layer. The ARM software configures the FPGA's UART via SPI (`UIO_SET_UART`, command `0x3B`), writes flag files to `/tmp/`, and delegates all actual data handling to external utilities (`/sbin/uartmode` and `/sbin/mlinkutil`). The SPI command `UIO_SIO_IN` (`0x1b`) is defined in `user_io.h:37` but completely unused — no serial data flows through the Main_MiSTer process itself.

This plan describes how to add a built-in Hayes AT command set implementation directly in Main_MiSTer, intercepting UART data from the FPGA before it reaches the network.

---

## Architecture Decision: In-Process vs. External

### Option A: Built into Main_MiSTer (Recommended)

Add a modem state machine that reads/writes UART data via the existing SPI interface, parses AT commands, and manages TCP connections from within the Main_MiSTer process.

**Pros:**
- No dependency on external utilities for modem mode
- Lower latency — no IPC overhead
- Full control over modem state and the OSD (can display connection status, etc.)
- Consistent with how other subsystems work (SD card emulation, CD-ROM, IDE all live in-process)

**Cons:**
- Adds networking code (sockets) to the main binary
- Must be non-blocking to avoid stalling the main poll loop

### Option B: Standalone daemon

Create a separate binary that reads/writes the FPGA UART and handles AT commands independently.

**Pros:** Isolation from main loop, can crash without taking down MiSTer.
**Cons:** Requires IPC for OSD status, duplicates SPI access patterns, adds deployment complexity.

**Recommendation:** Option A. The main binary already manages complex I/O (IDE, CD-ROM, SD emulation) in its poll loop. A non-blocking modem handler fits naturally.

---

## Implementation Plan

### Phase 1: UART Data Path — SPI Read/Write for Serial Bytes

**Goal:** Establish bidirectional byte transfer between the FPGA UART and a new in-process buffer.

**Files to modify:**
- `user_io.cpp` — add `modem_poll()` call in the poll loop
- `user_io.h` — expose modem control functions

**New file:**
- `modem.cpp` / `modem.h` — all modem logic lives here

**Details:**

The FPGA UART exposes serial data via the `UIO_SIO_IN` (`0x1b`) SPI command. This is already defined but unused. The data transfer pattern would follow the same conventions used by other SPI subsystems in the codebase:

```c
// Reading bytes from FPGA UART
void modem_poll_rx()
{
    uint16_t status = spi_uio_cmd_cont(UIO_SIO_IN);
    // status word indicates bytes available (core-dependent)
    while (bytes_available) {
        uint8_t byte = spi_in();
        rx_buffer_push(byte);
    }
    DisableIO();
}

// Writing bytes to FPGA UART (needs a UIO_SIO_OUT command)
void modem_poll_tx()
{
    spi_uio_cmd_cont(UIO_SIO_OUT);  // May need a new command definition
    while (tx_buffer_has_data()) {
        spi_w(tx_buffer_pop());
    }
    DisableIO();
}
```

**Open question:** The exact SPI protocol for reading/writing UART data depends on the FPGA-side UART module implementation. The `UIO_SIO_IN` command exists, but the framing (how many bytes, status bits, flow control) must be verified against the MiSTer framework FPGA code (`sys/` modules in core repos). A `UIO_SIO_OUT` command may need to be added (currently undefined).

**Integration point in main loop** — `user_io.cpp`, inside `user_io_poll()` (around line 3131, in the `CORE_TYPE_8BIT` branch):

```c
if (GetUARTMode() == 4) modem_poll();
```

---

### Phase 2: AT Command Parser

**Goal:** Parse the Hayes AT command set from the FPGA UART rx buffer.

**File:** `modem.cpp`

#### Modem State Machine

```
                  ┌─────────────┐
         ┌───────│  COMMAND     │◄──── ATH, remote disconnect,
         │       │  (idle)      │      +++  escape sequence
         │       └──────┬───────┘
         │              │ ATD<host>:<port>
         │              ▼
         │       ┌─────────────┐
         │       │  DIALING    │──── connection failed ──► "NO CARRIER\r\n"
         │       │  (connecting)│                           back to COMMAND
         │       └──────┬───────┘
         │              │ TCP connected
         │              ▼
         │       ┌─────────────┐
  ATH ───┘       │  ONLINE     │──── data passthrough
  +++            │  (connected) │     FPGA UART ↔ TCP socket
                 └──────┬───────┘
                        │ remote close
                        ▼
                 "NO CARRIER\r\n"
                 back to COMMAND
```

#### States

| State | Description |
|-------|-------------|
| `MODEM_COMMAND` | Accepting AT commands from UART, echoing characters |
| `MODEM_DIALING` | TCP `connect()` in progress (non-blocking) |
| `MODEM_ONLINE` | Transparent data passthrough between UART and TCP socket |
| `MODEM_ESCAPE` | Detected potential `+++` escape sequence, waiting for guard time |

#### AT Commands to Implement

**Tier 1 — Essential (minimum viable modem):**

| Command | Function | Implementation |
|---------|----------|----------------|
| `AT` | Attention / no-op | Return `OK\r\n` |
| `ATD<host>:<port>` | Dial (connect) | Non-blocking TCP `connect()` to host:port. Return `CONNECT <baud>\r\n` on success, `NO CARRIER\r\n` on failure |
| `ATH` | Hang up | Close TCP socket, return to command mode, `OK\r\n` |
| `ATA` | Answer | Accept incoming connection (if listening mode enabled) |
| `ATO` | Return online | Switch from command mode back to data mode on existing connection |
| `ATZ` | Reset | Close connection, reset all registers to defaults, `OK\r\n` |
| `ATE0` / `ATE1` | Echo off/on | Toggle local echo of typed characters |
| `ATV0` / `ATV1` | Numeric/verbal responses | `0` vs `OK`, `3` vs `NO CARRIER`, etc. |
| `ATQ0` / `ATQ1` | Quiet mode off/on | Suppress/enable result codes |
| `+++` | Escape to command mode | Guard time (default 1 second) before and after, switch to COMMAND state without disconnecting |

**Tier 2 — Extended compatibility:**

| Command | Function | Implementation |
|---------|----------|----------------|
| `ATS0=n` | Auto-answer ring count | Enable listening socket, answer after n rings |
| `ATS2=n` | Escape character | Change escape char (default `+`, ASCII 43) |
| `ATS3=n` | Carriage return character | Default 13 |
| `ATS4=n` | Line feed character | Default 10 |
| `ATS5=n` | Backspace character | Default 8 |
| `ATS12=n` | Escape guard time | In 1/50ths of a second (default 50 = 1 sec) |
| `AT&F` | Factory defaults | Reset all S-registers and settings |
| `ATI` / `ATI1-4` | Identification | Return modem info string ("MiSTer Hayes Modem v1.0") |
| `AT&D0` | DTR handling | Ignore DTR (useful since virtual modem has no real DTR) |

**Tier 3 — Telnet negotiation (for BBS compatibility):**

| Feature | Description |
|---------|-------------|
| IAC interpretation | Strip or respond to telnet IAC sequences (0xFF) |
| WILL/WONT/DO/DONT | Negotiate terminal type, echo, suppress go-ahead |
| Binary mode | Pass 8-bit clean data after negotiation |

Many BBSes accessed via telnet send IAC sequences. Without handling these, binary transfers and some menus break.

#### Command Parser Design

```c
// modem.h

#define MODEM_CMD_BUF_SIZE 256
#define MODEM_S_REGISTERS   32

typedef enum {
    MODEM_STATE_COMMAND,
    MODEM_STATE_DIALING,
    MODEM_STATE_ONLINE,
    MODEM_STATE_ESCAPE
} modem_state_t;

typedef struct {
    modem_state_t state;
    int           socket_fd;        // TCP socket (-1 if not connected)
    int           listen_fd;        // Listening socket for ATA/S0 (-1 if not listening)

    // Command buffer
    char          cmd_buf[MODEM_CMD_BUF_SIZE];
    int           cmd_pos;

    // S-registers
    uint8_t       s_reg[MODEM_S_REGISTERS];

    // Settings
    bool          echo;             // ATE
    bool          verbose;          // ATV
    bool          quiet;            // ATQ
    bool          telnet_mode;      // Telnet IAC handling enabled

    // Escape sequence detection
    uint32_t      last_data_time;   // Timestamp of last data byte sent
    int           plus_count;       // Number of consecutive '+' seen
    uint32_t      first_plus_time;  // Timestamp of first '+'

    // Telnet IAC state
    uint8_t       iac_state;
} modem_t;
```

The parser processes one byte at a time from the UART rx buffer. In `MODEM_STATE_COMMAND`, bytes accumulate in `cmd_buf` until CR is received, then `modem_exec_command()` dispatches:

```c
void modem_rx_byte(modem_t *m, uint8_t byte)
{
    switch (m->state)
    {
    case MODEM_STATE_COMMAND:
        if (m->echo) modem_tx_byte(m, byte);  // Local echo
        if (byte == m->s_reg[3]) {             // CR (S3 register)
            modem_exec_command(m);
        } else if (byte == m->s_reg[5]) {      // BS (S5 register)
            if (m->cmd_pos > 0) m->cmd_pos--;
        } else {
            if (m->cmd_pos < MODEM_CMD_BUF_SIZE - 1)
                m->cmd_buf[m->cmd_pos++] = byte;
        }
        break;

    case MODEM_STATE_ONLINE:
        // Forward to TCP socket + detect +++ escape
        modem_online_rx(m, byte);
        break;

    // ...
    }
}
```

---

### Phase 3: TCP Networking (Non-Blocking)

**Goal:** Manage outbound (ATD) and inbound (ATA) TCP connections without blocking the main loop.

**File:** `modem.cpp`

All socket operations must be non-blocking. The main MiSTer loop runs at high frequency to service FPGA requests — blocking on `connect()`, `recv()`, or `send()` would stall input, video, and SD card emulation.

```c
// Outbound connection (ATD)
void modem_dial(modem_t *m, const char *host, int port)
{
    m->socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, resolved_ip, &addr.sin_addr);

    int ret = connect(m->socket_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno == EINPROGRESS) {
        m->state = MODEM_STATE_DIALING;  // Will check in poll loop
    } else if (ret == 0) {
        modem_connected(m);
    } else {
        modem_send_response(m, "NO CARRIER");
    }
}

// In poll loop — check dialing progress and transfer data
void modem_poll(modem_t *m)
{
    if (m->state == MODEM_STATE_DIALING) {
        // Use poll()/select() with zero timeout to check connect completion
        struct pollfd pfd = { m->socket_fd, POLLOUT, 0 };
        if (poll(&pfd, 1, 0) > 0) {
            int err;
            socklen_t len = sizeof(err);
            getsockopt(m->socket_fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) modem_connected(m);
            else modem_send_response(m, "NO CARRIER");
        }
    }

    if (m->state == MODEM_STATE_ONLINE) {
        // TCP → UART: read from socket, write to FPGA
        uint8_t buf[256];
        int n = recv(m->socket_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (m->telnet_mode) modem_telnet_filter(m, buf[i]);
                else modem_tx_byte(m, buf[i]);
            }
        } else if (n == 0) {
            // Remote closed
            modem_hangup(m);
            modem_send_response(m, "NO CARRIER");
        }

        // UART → TCP: read from FPGA rx buffer, write to socket
        // (handled in modem_rx_byte -> modem_online_rx)
    }

    // Escape sequence timeout detection
    if (m->state == MODEM_STATE_ESCAPE) {
        // If guard time elapsed after 3 '+' chars, enter command mode
    }

    // Listening socket (ATA / S0 auto-answer)
    if (m->listen_fd >= 0) {
        // Accept with zero timeout
    }
}
```

**DNS resolution** is blocking by default. Options:
1. Use `getaddrinfo_a()` (GNU async DNS) — Linux-specific, available on the MiSTer's ARM Linux
2. Maintain a simple cache and resolve in a background thread
3. Accept IP addresses only for v1, add DNS later

Recommendation: Use `getaddrinfo_a()` for non-blocking DNS, falling back to synchronous resolution with a short timeout.

---

### Phase 4: Escape Sequence Detection (`+++`)

**Goal:** Correctly detect the Hayes escape sequence without false positives during binary transfers.

The standard requires:
1. At least 1 second of silence (no data from DTE) — the **guard time** (S12 register)
2. Three escape characters (S2 register, default `+`) sent within the escape timing window
3. Another 1 second of silence after the three characters

```c
void modem_online_rx(modem_t *m, uint8_t byte)
{
    uint32_t now = GetTimer(0);  // MiSTer's timer function

    if (byte == m->s_reg[2]) {  // Escape char (default '+')
        uint32_t guard = m->s_reg[12] * 20;  // S12 in 1/50ths sec → ms

        if (m->plus_count == 0) {
            // First '+': check guard time since last data
            if (CheckTimer(m->last_data_time, guard)) {
                m->plus_count = 1;
                m->first_plus_time = now;
            } else {
                // Within guard time of data — not an escape, send as data
                send(m->socket_fd, &byte, 1, MSG_DONTWAIT);
                m->last_data_time = now;
            }
        } else if (m->plus_count < 3) {
            m->plus_count++;
            if (m->plus_count == 3) {
                m->state = MODEM_STATE_ESCAPE;
                m->first_plus_time = now;  // Start post-guard timer
            }
        }
    } else {
        // Non-escape character: flush any pending '+' as data
        for (int i = 0; i < m->plus_count; i++) {
            uint8_t esc = m->s_reg[2];
            send(m->socket_fd, &esc, 1, MSG_DONTWAIT);
        }
        m->plus_count = 0;
        send(m->socket_fd, &byte, 1, MSG_DONTWAIT);
        m->last_data_time = now;
    }
}
```

In the poll loop, check if `MODEM_STATE_ESCAPE` has waited long enough:
```c
if (m->state == MODEM_STATE_ESCAPE) {
    uint32_t guard = m->s_reg[12] * 20;
    if (CheckTimer(m->first_plus_time, guard)) {
        m->state = MODEM_STATE_COMMAND;
        modem_send_response(m, "OK");
    }
}
```

---

### Phase 5: Telnet IAC Handling

**Goal:** Transparently handle telnet protocol when connecting to BBSes.

Most retro BBSes listen on telnet (port 23 or custom ports). They send IAC (Interpret As Command, `0xFF`) sequences that must be handled or the raw bytes corrupt the data stream.

```c
// Telnet protocol constants
#define TEL_IAC   0xFF
#define TEL_WILL  0xFB
#define TEL_WONT  0xFC
#define TEL_DO    0xFD
#define TEL_DONT  0xFE
#define TEL_SB    0xFA
#define TEL_SE    0xF0

void modem_telnet_filter(modem_t *m, uint8_t byte)
{
    switch (m->iac_state) {
    case 0:  // Normal data
        if (byte == TEL_IAC) m->iac_state = 1;
        else modem_tx_byte(m, byte);  // Pass through to UART
        break;

    case 1:  // Got IAC
        if (byte == TEL_IAC) {
            modem_tx_byte(m, 0xFF);  // Escaped 0xFF literal
            m->iac_state = 0;
        } else if (byte == TEL_WILL || byte == TEL_WONT) {
            m->iac_state = 2;  // Wait for option byte, respond DONT
        } else if (byte == TEL_DO || byte == TEL_DONT) {
            m->iac_state = 3;  // Wait for option byte, respond WONT
        } else if (byte == TEL_SB) {
            m->iac_state = 4;  // Subnegotiation, skip until SE
        } else {
            m->iac_state = 0;  // Unknown, ignore
        }
        break;

    case 2:  // WILL <option> → respond DONT <option>
        { uint8_t resp[] = { TEL_IAC, TEL_DONT, byte };
          send(m->socket_fd, resp, 3, MSG_DONTWAIT); }
        m->iac_state = 0;
        break;

    case 3:  // DO <option> → respond WONT <option>
        { uint8_t resp[] = { TEL_IAC, TEL_WONT, byte };
          send(m->socket_fd, resp, 3, MSG_DONTWAIT); }
        m->iac_state = 0;
        break;

    case 4:  // Subnegotiation — skip until IAC SE
        if (byte == TEL_IAC) m->iac_state = 5;
        break;

    case 5:
        if (byte == TEL_SE) m->iac_state = 0;
        else if (byte != TEL_IAC) m->iac_state = 4;
        break;
    }
}
```

This default-deny approach (WONT everything, DONT everything) is the safest starting point. For better BBS compatibility, selectively accepting `WILL ECHO` and `WILL SUPPRESS-GO-AHEAD` can be added later.

---

### Phase 6: OSD Integration

**Goal:** Show modem status in the MiSTer OSD and expose settings.

**File:** `menu.cpp` — extend the existing `MENU_UART1` / `MENU_UART2` states.

**Additions to the UART OSD menu when mode == 4 (Modem):**

```
┌─────────────────────────────┐
│       UART Mode             │
│                             │
│ Connection:         Modem   │
│ Link:                 TCP   │
│ Baud              (9600)  ▶ │
│ Telnet IAC:            On   │  ← new
│ Status:       CONNECTED     │  ← new (read-only)
│ Remote:  bbs.example:6400   │  ← new (read-only)
│                             │
│ Reset UART connection       │
│ Save                        │
└─────────────────────────────┘
```

**New OSD fields:**
- **Telnet IAC** — toggle telnet protocol filtering (on by default)
- **Status** — read-only, shows current modem state (IDLE / DIALING / CONNECTED / LISTENING)
- **Remote** — read-only, shows connected host:port when online

**Configuration persistence:** Add `telnet_iac` flag to the saved `uartmode.<corename>` config.

---

### Phase 7: Listening Mode (Inbound Connections)

**Goal:** Allow retro software to "receive calls" — other machines connect to the MiSTer.

When `ATS0=1` is set (auto-answer after 1 ring), or `ATA` is issued:

1. Open a listening TCP socket on a configurable port (default 23 or user-defined via `AT*LP=<port>` extension command)
2. When a connection arrives, send `RING\r\n` to the UART
3. If auto-answer count reached or `ATA` issued, accept the connection and send `CONNECT <baud>\r\n`
4. Enter online mode

This enables scenarios like:
- Two MiSTer units playing serial multiplayer games
- A PC connecting to a retro core's BBS software

---

## File Summary

| File | Action | Description |
|------|--------|-------------|
| `modem.h` | **New** | Modem state struct, constants, S-register defaults, public API |
| `modem.cpp` | **New** | AT command parser, state machine, TCP networking, telnet filter, escape detection |
| `user_io.cpp` | **Modify** | Add `modem_poll()` call in `user_io_poll()` when UART mode == 4. Add SPI data read/write via `UIO_SIO_IN` |
| `user_io.h` | **Modify** | Add `UIO_SIO_OUT` if needed for UART TX, expose modem status query functions |
| `menu.cpp` | **Modify** | Extend UART OSD menu with telnet toggle and status display |
| `Makefile` | **Modify** | No changes needed — `*.cpp` in root is auto-included via wildcard |

---

## Dependencies and Constraints

1. **FPGA-side UART framing protocol** — The `UIO_SIO_IN` command (`0x1b`) is defined but unused. The exact read/write protocol (how bytes are framed over SPI, how the core signals data availability) must be verified against the MiSTer FPGA framework's `sys/` modules. This is the single biggest unknown and may require changes to the FPGA framework.

2. **Non-blocking requirement** — The `user_io_poll()` function is called in a tight loop. All socket operations (connect, send, recv, accept, DNS) must be non-blocking. A blocked modem call would freeze the entire MiSTer UI.

3. **Interaction with mlinkutil** — When Hayes mode is active, the external `mlinkutil` should not also be running on the same UART. The `SetUARTMode()` function currently calls `system("uartmode 4")` which launches external utilities. This must be modified to skip external launch when built-in Hayes mode is selected, or a new UART mode value (e.g., mode 7) should be added for the built-in modem.

4. **Thread safety** — The main loop is single-threaded. As long as modem_poll() is called from within user_io_poll(), no locking is needed.

5. **Socket includes** — The codebase does not currently include networking headers. Add: `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<netdb.h>`, `<poll.h>`, `<fcntl.h>`.

---

## Suggested Implementation Order

| Step | Phase | Effort | Description |
|------|-------|--------|-------------|
| 1 | Phase 1 | Medium | Verify SPI UART data protocol against FPGA framework, implement rx/tx byte transfer |
| 2 | Phase 2 | Medium | AT command parser with Tier 1 commands (AT, ATD, ATH, ATE, ATV, ATZ, +++ escape) |
| 3 | Phase 3 | Medium | Non-blocking TCP connect/send/recv, basic dial-out working |
| 4 | Phase 5 | Small | Telnet IAC filter (essential for real-world BBS use) |
| 5 | Phase 4 | Small | Proper escape sequence detection with guard timing |
| 6 | Phase 6 | Small | OSD status display and telnet toggle |
| 7 | Phase 2 | Small | Tier 2 commands (S-registers, ATI, AT&F) |
| 8 | Phase 7 | Medium | Listening mode for inbound connections |

Phase 1 (SPI data path) is the critical path — everything else is blocked on being able to read/write bytes to/from the FPGA UART.
