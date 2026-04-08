#ifndef MODEM_H
#define MODEM_H

#include <inttypes.h>
#include <stdbool.h>

// Modem state machine states
typedef enum {
	MODEM_STATE_COMMAND,   // Accepting AT commands
	MODEM_STATE_DIALING,   // TCP connect() in progress
	MODEM_STATE_ONLINE,    // Transparent data passthrough
	MODEM_STATE_ESCAPE     // Detected potential +++ escape, waiting guard time
} modem_state_t;

// Result codes (numeric values for ATV0 mode)
#define MODEM_RC_OK          0
#define MODEM_RC_CONNECT     1
#define MODEM_RC_RING        2
#define MODEM_RC_NO_CARRIER  3
#define MODEM_RC_ERROR       4
#define MODEM_RC_NO_ANSWER   8
#define MODEM_RC_BUSY        7

// S-register indices
#define SREG_AUTO_ANSWER     0   // Ring count for auto-answer (0=disabled)
#define SREG_RING_COUNT      1   // Current ring counter
#define SREG_ESCAPE_CHAR     2   // Escape character (default '+')
#define SREG_CR_CHAR         3   // Carriage return character (default 13)
#define SREG_LF_CHAR         4   // Line feed character (default 10)
#define SREG_BS_CHAR         5   // Backspace character (default 8)
#define SREG_GUARD_TIME     12   // Escape guard time in 1/50ths sec (default 50 = 1s)

// Telnet protocol constants
#define TEL_IAC   0xFF
#define TEL_WILL  0xFB
#define TEL_WONT  0xFC
#define TEL_DO    0xFD
#define TEL_DONT  0xFE
#define TEL_SB    0xFA
#define TEL_SE    0xF0

#define MODEM_CMD_BUF_SIZE  256
#define MODEM_TX_BUF_SIZE   1024
#define MODEM_RX_BUF_SIZE   1024
#define MODEM_S_REG_COUNT   16
#define MODEM_LISTEN_PORT   23

// Initialize modem subsystem
void modem_init();

// Poll modem - called from user_io_poll() when UART mode == 4
void modem_poll();

// Shut down modem (close sockets, reset state)
void modem_shutdown();

// Get current modem state for OSD display
modem_state_t modem_get_state();

// Get connected host string for OSD display (empty if not connected)
const char *modem_get_remote();

// Get/set telnet IAC filter mode
bool modem_get_telnet_mode();
void modem_set_telnet_mode(bool enabled);

#endif // MODEM_H
