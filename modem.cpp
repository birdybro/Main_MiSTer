#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "modem.h"
#include "hardware.h"
#include "spi.h"
#include "user_io.h"

// ---------------------------------------------------------------------------
// Modem state
// ---------------------------------------------------------------------------

static struct {
	modem_state_t state;
	int           socket_fd;
	int           listen_fd;

	// Command buffer
	char          cmd_buf[MODEM_CMD_BUF_SIZE];
	int           cmd_pos;

	// S-registers
	uint8_t       s_reg[MODEM_S_REG_COUNT];

	// Settings
	bool          echo;
	bool          verbose;
	bool          quiet;
	bool          telnet_mode;

	// Escape sequence detection
	unsigned long last_data_time;
	int           plus_count;
	unsigned long first_plus_time;

	// Telnet IAC state machine
	int           iac_state;

	// TX buffer (modem -> FPGA UART)
	uint8_t       tx_buf[MODEM_TX_BUF_SIZE];
	int           tx_head;
	int           tx_tail;

	// Connected remote info for OSD
	char          remote_host[128];
	int           remote_port;
	char          remote_str[160];

	// Listen port
	int           listen_port;

	// Initialized flag
	bool          active;
} m;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void modem_reset();
static void modem_send_response(int code);
static void modem_send_string(const char *str);
static void modem_tx_byte(uint8_t byte);
static void modem_exec_command(const char *cmd);
static void modem_dial(const char *addr);
static void modem_hangup();
static void modem_connected();
static void modem_online_rx(uint8_t byte);
static void modem_telnet_filter(uint8_t byte);
static void modem_poll_spi_rx();
static void modem_poll_spi_tx();
static void modem_poll_dialing();
static void modem_poll_online();
static void modem_poll_escape();
static void modem_poll_listen();

// ---------------------------------------------------------------------------
// TX buffer helpers
// ---------------------------------------------------------------------------

static int tx_buf_count()
{
	int count = m.tx_head - m.tx_tail;
	if (count < 0) count += MODEM_TX_BUF_SIZE;
	return count;
}

static void tx_buf_push(uint8_t byte)
{
	int next = (m.tx_head + 1) % MODEM_TX_BUF_SIZE;
	if (next == m.tx_tail) return; // full, drop byte
	m.tx_buf[m.tx_head] = byte;
	m.tx_head = next;
}

static int tx_buf_pop(uint8_t *byte)
{
	if (m.tx_head == m.tx_tail) return 0; // empty
	*byte = m.tx_buf[m.tx_tail];
	m.tx_tail = (m.tx_tail + 1) % MODEM_TX_BUF_SIZE;
	return 1;
}

// ---------------------------------------------------------------------------
// Init / Shutdown / Reset
// ---------------------------------------------------------------------------

static void modem_defaults()
{
	memset(m.s_reg, 0, sizeof(m.s_reg));
	m.s_reg[SREG_AUTO_ANSWER] = 0;
	m.s_reg[SREG_RING_COUNT]  = 0;
	m.s_reg[SREG_ESCAPE_CHAR] = '+';
	m.s_reg[SREG_CR_CHAR]     = 13;
	m.s_reg[SREG_LF_CHAR]     = 10;
	m.s_reg[SREG_BS_CHAR]     = 8;
	m.s_reg[SREG_GUARD_TIME]  = 50; // 1 second in 1/50ths

	m.echo = true;
	m.verbose = true;
	m.quiet = false;
	m.telnet_mode = true;
	m.listen_port = MODEM_LISTEN_PORT;
}

static void modem_reset()
{
	modem_hangup();
	modem_defaults();
	m.cmd_pos = 0;
	m.plus_count = 0;
	m.iac_state = 0;
	m.tx_head = 0;
	m.tx_tail = 0;
}

void modem_init()
{
	memset(&m, 0, sizeof(m));
	m.socket_fd = -1;
	m.listen_fd = -1;
	m.state = MODEM_STATE_COMMAND;
	modem_defaults();
	m.active = true;
	printf("Hayes modem emulation initialized\n");
}

void modem_shutdown()
{
	if (!m.active) return;
	modem_hangup();
	if (m.listen_fd >= 0) { close(m.listen_fd); m.listen_fd = -1; }
	m.active = false;
	printf("Hayes modem emulation shut down\n");
}

// ---------------------------------------------------------------------------
// Response output
// ---------------------------------------------------------------------------

static void modem_tx_byte(uint8_t byte)
{
	tx_buf_push(byte);
}

static void modem_send_crlf()
{
	modem_tx_byte(m.s_reg[SREG_CR_CHAR]);
	modem_tx_byte(m.s_reg[SREG_LF_CHAR]);
}

static void modem_send_string(const char *str)
{
	while (*str) modem_tx_byte((uint8_t)*str++);
}

static const char *result_code_verbose[] = {
	"OK",           // 0
	"CONNECT",      // 1
	"RING",         // 2
	"NO CARRIER",   // 3
	"ERROR",        // 4
	"",             // 5 (unused)
	"",             // 6 (unused)
	"BUSY",         // 7
	"NO ANSWER"     // 8
};

static void modem_send_response(int code)
{
	if (m.quiet) return;

	if (m.verbose)
	{
		modem_send_crlf();
		modem_send_string(result_code_verbose[code]);
		modem_send_crlf();
	}
	else
	{
		char buf[8];
		snprintf(buf, sizeof(buf), "%d", code);
		modem_send_string(buf);
		modem_tx_byte(m.s_reg[SREG_CR_CHAR]);
	}
}

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------

static void set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void modem_dial(const char *addr)
{
	// Parse host:port from ATD parameter
	// Formats: ATDhost:port  ATDhost port  ATDhost (default port 23)
	char host[128] = {};
	int port = 23;

	const char *colon = strrchr(addr, ':');
	const char *space = strrchr(addr, ' ');
	const char *sep = colon ? colon : space;

	if (sep)
	{
		int hlen = (int)(sep - addr);
		if (hlen >= (int)sizeof(host)) hlen = sizeof(host) - 1;
		memcpy(host, addr, hlen);
		host[hlen] = 0;
		port = atoi(sep + 1);
		if (port <= 0 || port > 65535) port = 23;
	}
	else
	{
		snprintf(host, sizeof(host), "%s", addr);
	}

	// Trim whitespace
	char *p = host + strlen(host) - 1;
	while (p >= host && isspace(*p)) *p-- = 0;
	p = host;
	while (*p && isspace(*p)) p++;
	if (p != host) memmove(host, p, strlen(p) + 1);

	if (!host[0])
	{
		modem_send_response(MODEM_RC_ERROR);
		return;
	}

	printf("Modem: dialing %s:%d\n", host, port);

	// Resolve hostname (synchronous — typically fast for IPs, may block for DNS)
	struct addrinfo hints = {}, *res = NULL;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char port_str[8];
	snprintf(port_str, sizeof(port_str), "%d", port);

	int err = getaddrinfo(host, port_str, &hints, &res);
	if (err || !res)
	{
		printf("Modem: DNS resolution failed for %s: %s\n", host, gai_strerror(err));
		modem_send_response(MODEM_RC_NO_ANSWER);
		return;
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		printf("Modem: socket() failed: %s\n", strerror(errno));
		freeaddrinfo(res);
		modem_send_response(MODEM_RC_NO_CARRIER);
		return;
	}

	set_nonblocking(fd);

	// Disable Nagle for low-latency serial emulation
	int opt = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

	int ret = connect(fd, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);

	if (ret == 0)
	{
		// Immediate connect (unlikely but possible for localhost)
		m.socket_fd = fd;
		snprintf(m.remote_host, sizeof(m.remote_host), "%s", host);
		m.remote_port = port;
		modem_connected();
	}
	else if (errno == EINPROGRESS)
	{
		m.socket_fd = fd;
		snprintf(m.remote_host, sizeof(m.remote_host), "%s", host);
		m.remote_port = port;
		m.state = MODEM_STATE_DIALING;
	}
	else
	{
		printf("Modem: connect() failed: %s\n", strerror(errno));
		close(fd);
		modem_send_response(MODEM_RC_NO_CARRIER);
	}
}

static void modem_connected()
{
	m.state = MODEM_STATE_ONLINE;
	m.last_data_time = GetTimer(0);
	m.plus_count = 0;
	m.iac_state = 0;

	snprintf(m.remote_str, sizeof(m.remote_str), "%s:%d", m.remote_host, m.remote_port);
	printf("Modem: connected to %s\n", m.remote_str);

	if (m.verbose)
	{
		modem_send_crlf();
		char buf[64];
		snprintf(buf, sizeof(buf), "CONNECT %d", GetUARTbaud(GetUARTMode()));
		modem_send_string(buf);
		modem_send_crlf();
	}
	else
	{
		modem_send_response(MODEM_RC_CONNECT);
	}
}

static void modem_hangup()
{
	if (m.socket_fd >= 0)
	{
		close(m.socket_fd);
		m.socket_fd = -1;
		printf("Modem: disconnected from %s\n", m.remote_str);
	}
	m.state = MODEM_STATE_COMMAND;
	m.remote_host[0] = 0;
	m.remote_port = 0;
	m.remote_str[0] = 0;
	m.plus_count = 0;
	m.cmd_pos = 0;
}

// ---------------------------------------------------------------------------
// Listening (inbound connections)
// ---------------------------------------------------------------------------

static void modem_start_listen()
{
	if (m.listen_fd >= 0) return; // already listening

	m.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (m.listen_fd < 0)
	{
		printf("Modem: listen socket() failed: %s\n", strerror(errno));
		return;
	}

	int opt = 1;
	setsockopt(m.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	set_nonblocking(m.listen_fd);

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(m.listen_port);

	if (bind(m.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		printf("Modem: bind() port %d failed: %s\n", m.listen_port, strerror(errno));
		close(m.listen_fd);
		m.listen_fd = -1;
		return;
	}

	if (listen(m.listen_fd, 1) < 0)
	{
		printf("Modem: listen() failed: %s\n", strerror(errno));
		close(m.listen_fd);
		m.listen_fd = -1;
		return;
	}

	printf("Modem: listening on port %d\n", m.listen_port);
}

static void modem_stop_listen()
{
	if (m.listen_fd >= 0)
	{
		close(m.listen_fd);
		m.listen_fd = -1;
		printf("Modem: stopped listening\n");
	}
}

// ---------------------------------------------------------------------------
// Telnet IAC filter
// ---------------------------------------------------------------------------

static void modem_telnet_filter(uint8_t byte)
{
	switch (m.iac_state)
	{
	case 0: // Normal data
		if (byte == TEL_IAC)
			m.iac_state = 1;
		else
			modem_tx_byte(byte);
		break;

	case 1: // Got IAC
		if (byte == TEL_IAC)
		{
			modem_tx_byte(0xFF); // Escaped 0xFF literal
			m.iac_state = 0;
		}
		else if (byte == TEL_WILL || byte == TEL_WONT)
		{
			m.iac_state = 2; // Wait for option, respond DONT
		}
		else if (byte == TEL_DO || byte == TEL_DONT)
		{
			m.iac_state = 3; // Wait for option, respond WONT
		}
		else if (byte == TEL_SB)
		{
			m.iac_state = 4; // Subnegotiation, skip until SE
		}
		else
		{
			m.iac_state = 0; // Unknown command byte, ignore
		}
		break;

	case 2: // WILL <option> -> respond DONT <option>
		{
			uint8_t resp[] = { TEL_IAC, TEL_DONT, byte };
			if (m.socket_fd >= 0) send(m.socket_fd, resp, 3, MSG_DONTWAIT | MSG_NOSIGNAL);
		}
		m.iac_state = 0;
		break;

	case 3: // DO <option> -> respond WONT <option>
		{
			uint8_t resp[] = { TEL_IAC, TEL_WONT, byte };
			if (m.socket_fd >= 0) send(m.socket_fd, resp, 3, MSG_DONTWAIT | MSG_NOSIGNAL);
		}
		m.iac_state = 0;
		break;

	case 4: // Subnegotiation - skip until IAC
		if (byte == TEL_IAC) m.iac_state = 5;
		break;

	case 5: // After IAC in subnegotiation
		if (byte == TEL_SE) m.iac_state = 0;
		else if (byte != TEL_IAC) m.iac_state = 4;
		break;

	default:
		m.iac_state = 0;
		break;
	}
}

// ---------------------------------------------------------------------------
// Escape sequence detection (+++)
// ---------------------------------------------------------------------------

static void modem_online_rx(uint8_t byte)
{
	unsigned long now = GetTimer(0);
	unsigned long guard = (unsigned long)m.s_reg[SREG_GUARD_TIME] * 20; // 1/50ths sec -> ms
	uint8_t esc_char = m.s_reg[SREG_ESCAPE_CHAR];

	if (esc_char == 0)
	{
		// Escape disabled (S2=0): just forward everything
		if (m.socket_fd >= 0) send(m.socket_fd, &byte, 1, MSG_DONTWAIT | MSG_NOSIGNAL);
		m.last_data_time = now;
		return;
	}

	if (byte == esc_char)
	{
		if (m.plus_count == 0)
		{
			// First escape char: check guard time since last data
			unsigned long elapsed = now - m.last_data_time;
			if (elapsed >= guard)
			{
				m.plus_count = 1;
				m.first_plus_time = now;
			}
			else
			{
				// Within guard time - send as data
				if (m.socket_fd >= 0) send(m.socket_fd, &byte, 1, MSG_DONTWAIT | MSG_NOSIGNAL);
				m.last_data_time = now;
			}
		}
		else if (m.plus_count < 3)
		{
			m.plus_count++;
			if (m.plus_count == 3)
			{
				// Got 3 escape chars - start post-guard timer
				m.state = MODEM_STATE_ESCAPE;
				m.first_plus_time = now;
			}
		}
	}
	else
	{
		// Non-escape character: flush any pending escape chars as data
		if (m.plus_count > 0 && m.socket_fd >= 0)
		{
			for (int i = 0; i < m.plus_count; i++)
			{
				send(m.socket_fd, &esc_char, 1, MSG_DONTWAIT | MSG_NOSIGNAL);
			}
			m.plus_count = 0;
		}
		if (m.socket_fd >= 0) send(m.socket_fd, &byte, 1, MSG_DONTWAIT | MSG_NOSIGNAL);
		m.last_data_time = now;
	}
}

// ---------------------------------------------------------------------------
// AT command parser
// ---------------------------------------------------------------------------

static void modem_exec_command(const char *cmd)
{
	// Skip leading whitespace
	while (*cmd && isspace(*cmd)) cmd++;

	// Must start with "AT" (or just "A/")
	if (cmd[0] == 'A' && cmd[1] == '/')
	{
		// A/ = repeat last command — not implemented yet
		modem_send_response(MODEM_RC_OK);
		return;
	}

	if (toupper(cmd[0]) != 'A' || toupper(cmd[1]) != 'T')
	{
		modem_send_response(MODEM_RC_ERROR);
		return;
	}

	cmd += 2; // skip "AT"

	// Bare "AT" -> OK
	if (!*cmd)
	{
		modem_send_response(MODEM_RC_OK);
		return;
	}

	// Process commands left to right
	while (*cmd)
	{
		while (*cmd == ' ') cmd++; // skip spaces
		if (!*cmd) break;

		char c = toupper(*cmd++);

		switch (c)
		{
		case 'D': // Dial
			modem_dial(cmd);
			return; // rest of string is the address

		case 'H': // Hang up
			{
				int n = 0;
				if (isdigit(*cmd)) n = *cmd++ - '0';
				(void)n; // ATH0 and ATH both hang up
				modem_hangup();
				modem_send_response(MODEM_RC_OK);
			}
			break;

		case 'A': // Answer
			if (m.listen_fd >= 0)
			{
				struct sockaddr_in peer;
				socklen_t peerlen = sizeof(peer);
				int fd = accept(m.listen_fd, (struct sockaddr *)&peer, &peerlen);
				if (fd >= 0)
				{
					set_nonblocking(fd);
					int opt = 1;
					setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
					m.socket_fd = fd;
					inet_ntop(AF_INET, &peer.sin_addr, m.remote_host, sizeof(m.remote_host));
					m.remote_port = ntohs(peer.sin_port);
					modem_connected();
					return;
				}
			}
			modem_send_response(MODEM_RC_NO_CARRIER);
			return;

		case 'O': // Return online
			if (m.socket_fd >= 0)
			{
				m.state = MODEM_STATE_ONLINE;
				m.last_data_time = GetTimer(0);
				m.plus_count = 0;
				modem_send_response(MODEM_RC_CONNECT);
			}
			else
			{
				modem_send_response(MODEM_RC_NO_CARRIER);
			}
			return;

		case 'Z': // Reset
			modem_reset();
			modem_send_response(MODEM_RC_OK);
			return;

		case 'E': // Echo
			{
				int n = 1;
				if (isdigit(*cmd)) n = *cmd++ - '0';
				m.echo = (n != 0);
			}
			break;

		case 'V': // Verbose
			{
				int n = 1;
				if (isdigit(*cmd)) n = *cmd++ - '0';
				m.verbose = (n != 0);
			}
			break;

		case 'Q': // Quiet
			{
				int n = 0;
				if (isdigit(*cmd)) n = *cmd++ - '0';
				m.quiet = (n != 0);
			}
			break;

		case 'I': // Identification
			{
				int n = 0;
				if (isdigit(*cmd)) n = *cmd++ - '0';
				(void)n;
				modem_send_crlf();
				modem_send_string("MiSTer Hayes Modem v1.0");
				modem_send_crlf();
			}
			break;

		case 'S': // S-registers
			{
				int reg = 0;
				while (isdigit(*cmd)) reg = reg * 10 + (*cmd++ - '0');

				if (*cmd == '=')
				{
					// Set register
					cmd++;
					int val = 0;
					while (isdigit(*cmd)) val = val * 10 + (*cmd++ - '0');

					if (reg < MODEM_S_REG_COUNT)
					{
						m.s_reg[reg] = (uint8_t)(val & 0xFF);
						printf("Modem: S%d=%d\n", reg, val);

						// S0: auto-answer
						if (reg == SREG_AUTO_ANSWER)
						{
							if (val > 0) modem_start_listen();
							else modem_stop_listen();
						}
					}
					else
					{
						modem_send_response(MODEM_RC_ERROR);
						return;
					}
				}
				else if (*cmd == '?')
				{
					// Query register
					cmd++;
					if (reg < MODEM_S_REG_COUNT)
					{
						char buf[8];
						snprintf(buf, sizeof(buf), "%03d", m.s_reg[reg]);
						modem_send_crlf();
						modem_send_string(buf);
						modem_send_crlf();
					}
					else
					{
						modem_send_response(MODEM_RC_ERROR);
						return;
					}
				}
			}
			break;

		case '&': // Extended commands
			{
				char c2 = toupper(*cmd++);
				switch (c2)
				{
				case 'F': // Factory defaults
					modem_defaults();
					break;
				case 'D': // DTR handling — ignore (no real DTR)
					if (isdigit(*cmd)) cmd++;
					break;
				default:
					// Unknown & command — skip digit if present
					if (isdigit(*cmd)) cmd++;
					break;
				}
			}
			break;

		case '*': // MiSTer extension commands
			{
				char c2 = toupper(*cmd++);
				if (c2 == 'L' && toupper(*cmd) == 'P')
				{
					// AT*LP=port — set listen port
					cmd++;
					if (*cmd == '=')
					{
						cmd++;
						int port = 0;
						while (isdigit(*cmd)) port = port * 10 + (*cmd++ - '0');
						if (port > 0 && port <= 65535)
						{
							modem_stop_listen();
							m.listen_port = port;
							printf("Modem: listen port set to %d\n", port);
							if (m.s_reg[SREG_AUTO_ANSWER] > 0) modem_start_listen();
						}
						else
						{
							modem_send_response(MODEM_RC_ERROR);
							return;
						}
					}
				}
				else if (c2 == 'T')
				{
					// AT*T0 / AT*T1 — telnet mode off/on
					int n = 1;
					if (isdigit(*cmd)) n = *cmd++ - '0';
					m.telnet_mode = (n != 0);
					printf("Modem: telnet mode %s\n", m.telnet_mode ? "on" : "off");
				}
			}
			break;

		default:
			// Unknown command letter — skip any trailing digit
			if (isdigit(*cmd)) cmd++;
			break;
		}
	}

	modem_send_response(MODEM_RC_OK);
}

// ---------------------------------------------------------------------------
// Handle byte received from FPGA UART
// ---------------------------------------------------------------------------

static void modem_rx_byte(uint8_t byte)
{
	switch (m.state)
	{
	case MODEM_STATE_COMMAND:
		if (m.echo) modem_tx_byte(byte);

		if (byte == m.s_reg[SREG_CR_CHAR])
		{
			// End of command line
			if (m.echo) modem_send_crlf();
			m.cmd_buf[m.cmd_pos] = 0;
			modem_exec_command(m.cmd_buf);
			m.cmd_pos = 0;
		}
		else if (byte == m.s_reg[SREG_BS_CHAR])
		{
			// Backspace
			if (m.cmd_pos > 0) m.cmd_pos--;
		}
		else if (byte == m.s_reg[SREG_LF_CHAR])
		{
			// Ignore LF in command mode
		}
		else
		{
			if (m.cmd_pos < MODEM_CMD_BUF_SIZE - 1)
				m.cmd_buf[m.cmd_pos++] = byte;
		}
		break;

	case MODEM_STATE_ONLINE:
		modem_online_rx(byte);
		break;

	case MODEM_STATE_ESCAPE:
		// Any data during escape post-guard cancels the escape
		{
			uint8_t esc = m.s_reg[SREG_ESCAPE_CHAR];
			// Flush the 3 pending escape chars as data
			for (int i = 0; i < 3; i++)
			{
				if (m.socket_fd >= 0) send(m.socket_fd, &esc, 1, MSG_DONTWAIT | MSG_NOSIGNAL);
			}
			m.plus_count = 0;
			m.state = MODEM_STATE_ONLINE;
			modem_online_rx(byte);
		}
		break;

	case MODEM_STATE_DIALING:
		// Ignore data while dialing
		break;
	}
}

// ---------------------------------------------------------------------------
// SPI UART data transfer
// ---------------------------------------------------------------------------

static void modem_poll_spi_rx()
{
	// Read bytes from FPGA UART via UIO_SIO_IN
	// The status word returned by spi_uio_cmd_cont indicates data availability.
	// Bit 0 of the status word indicates at least one byte is available.
	// We read up to a reasonable limit per poll cycle to avoid stalling.

	uint16_t status = spi_uio_cmd_cont(UIO_SIO_IN);
	int count = 0;
	while ((status & 1) && count < 64)
	{
		uint8_t byte = spi_in();
		modem_rx_byte(byte);
		status = spi_w(0); // get next status
		count++;
	}
	DisableIO();
}

static void modem_poll_spi_tx()
{
	// Write buffered bytes to FPGA UART
	// We reuse UIO_SIO_IN for bidirectional transfer:
	// after the initial status read, each spi_w() call both sends a byte
	// and receives the next status. We send bytes while the TX FIFO accepts them.

	if (tx_buf_count() == 0) return;

	spi_uio_cmd_cont(UIO_SIO_IN);
	int count = 0;
	uint8_t byte;
	while (tx_buf_pop(&byte) && count < 64)
	{
		spi_w(byte);
		count++;
	}
	DisableIO();
}

// ---------------------------------------------------------------------------
// Poll sub-functions by state
// ---------------------------------------------------------------------------

static void modem_poll_dialing()
{
	if (m.socket_fd < 0) return;

	struct pollfd pfd = { m.socket_fd, POLLOUT, 0 };
	int ret = poll(&pfd, 1, 0);
	if (ret > 0)
	{
		int err = 0;
		socklen_t len = sizeof(err);
		getsockopt(m.socket_fd, SOL_SOCKET, SO_ERROR, &err, &len);

		if (err == 0)
		{
			modem_connected();
		}
		else
		{
			printf("Modem: connect failed: %s\n", strerror(err));
			close(m.socket_fd);
			m.socket_fd = -1;
			m.state = MODEM_STATE_COMMAND;
			modem_send_response(MODEM_RC_NO_CARRIER);
		}
	}
	else if (ret < 0 && errno != EINTR)
	{
		close(m.socket_fd);
		m.socket_fd = -1;
		m.state = MODEM_STATE_COMMAND;
		modem_send_response(MODEM_RC_NO_CARRIER);
	}
}

static void modem_poll_online()
{
	if (m.socket_fd < 0)
	{
		m.state = MODEM_STATE_COMMAND;
		modem_send_response(MODEM_RC_NO_CARRIER);
		return;
	}

	// Read from TCP socket -> TX to FPGA UART
	uint8_t buf[256];
	int n = recv(m.socket_fd, buf, sizeof(buf), MSG_DONTWAIT);
	if (n > 0)
	{
		for (int i = 0; i < n; i++)
		{
			if (m.telnet_mode)
				modem_telnet_filter(buf[i]);
			else
				modem_tx_byte(buf[i]);
		}
	}
	else if (n == 0)
	{
		// Remote closed connection
		modem_hangup();
		modem_send_response(MODEM_RC_NO_CARRIER);
	}
	else if (errno != EAGAIN && errno != EWOULDBLOCK)
	{
		// Socket error
		modem_hangup();
		modem_send_response(MODEM_RC_NO_CARRIER);
	}
}

static void modem_poll_escape()
{
	unsigned long guard = (unsigned long)m.s_reg[SREG_GUARD_TIME] * 20;
	unsigned long elapsed = GetTimer(0) - m.first_plus_time;

	if (elapsed >= guard)
	{
		// Post-guard time elapsed — enter command mode
		m.state = MODEM_STATE_COMMAND;
		m.plus_count = 0;
		m.cmd_pos = 0;
		modem_send_response(MODEM_RC_OK);
		printf("Modem: escaped to command mode\n");
	}
}

static void modem_poll_listen()
{
	if (m.listen_fd < 0) return;
	if (m.state != MODEM_STATE_COMMAND) return; // Only accept while idle

	struct sockaddr_in peer;
	socklen_t peerlen = sizeof(peer);
	int fd = accept(m.listen_fd, (struct sockaddr *)&peer, &peerlen);
	if (fd < 0) return; // No incoming connection

	// Send RING
	modem_send_response(MODEM_RC_RING);
	m.s_reg[SREG_RING_COUNT]++;

	if (m.s_reg[SREG_AUTO_ANSWER] > 0 && m.s_reg[SREG_RING_COUNT] >= m.s_reg[SREG_AUTO_ANSWER])
	{
		// Auto-answer
		set_nonblocking(fd);
		int opt = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
		m.socket_fd = fd;
		inet_ntop(AF_INET, &peer.sin_addr, m.remote_host, sizeof(m.remote_host));
		m.remote_port = ntohs(peer.sin_port);
		m.s_reg[SREG_RING_COUNT] = 0;
		modem_connected();
	}
	else
	{
		// Not auto-answering yet — reject for now (user must issue ATA)
		// In a real modem we'd hold the connection and keep ringing,
		// but that adds complexity. For now, close and wait for next.
		close(fd);
	}
}

// ---------------------------------------------------------------------------
// Main poll entry point
// ---------------------------------------------------------------------------

void modem_poll()
{
	if (!m.active) return;

	// Read bytes from FPGA UART
	modem_poll_spi_rx();

	// State-specific processing
	switch (m.state)
	{
	case MODEM_STATE_COMMAND:
		break; // Commands processed in modem_rx_byte()

	case MODEM_STATE_DIALING:
		modem_poll_dialing();
		break;

	case MODEM_STATE_ONLINE:
		modem_poll_online();
		break;

	case MODEM_STATE_ESCAPE:
		modem_poll_escape();
		// Also poll online data (socket -> UART) while in escape state
		if (m.state == MODEM_STATE_ESCAPE && m.socket_fd >= 0)
			modem_poll_online();
		break;
	}

	// Check for incoming connections
	modem_poll_listen();

	// Write buffered bytes to FPGA UART
	modem_poll_spi_tx();
}

// ---------------------------------------------------------------------------
// OSD query functions
// ---------------------------------------------------------------------------

modem_state_t modem_get_state()
{
	return m.state;
}

const char *modem_get_remote()
{
	return m.remote_str;
}

bool modem_get_telnet_mode()
{
	return m.telnet_mode;
}

void modem_set_telnet_mode(bool enabled)
{
	m.telnet_mode = enabled;
}
