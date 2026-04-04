// SPDX-License-Identifier: MIT
// Remote driver client for Monado's remote simulation driver.
//
// Connects to monado-service (started with P_OVERRIDE_ACTIVE_CONFIG=remote)
// on port 4242 and lets AI agents control the simulated HMD/controllers.
//
// Usage: monado-remote-client [host [port]]
//
// Commands (one per stdin line, responses on stdout):
//   head X Y Z QX QY QZ QW          Set HMD center pose
//   left_pose X Y Z QX QY QZ QW     Set left controller pose
//   right_pose X Y Z QX QY QZ QW    Set right controller pose
//   left_trigger VALUE               Left trigger axis [0..1]
//   right_trigger VALUE              Right trigger axis [0..1]
//   left_squeeze VALUE               Left squeeze axis [0..1]
//   right_squeeze VALUE              Right squeeze axis [0..1]
//   left_thumbstick X Y              Left thumbstick [-1..1, -1..1]
//   right_thumbstick X Y             Right thumbstick
//   left_trigger_click 0|1           Left trigger click button
//   left_a_click 0|1                 Left A button
//   left_b_click 0|1                 Left B button
//   left_system_click 0|1            Left system button
//   left_thumbstick_click 0|1        Left thumbstick click
//   right_trigger_click 0|1          (same for right controller)
//   right_a_click 0|1
//   right_b_click 0|1
//   right_system_click 0|1
//   right_thumbstick_click 0|1
//   send                             Transmit current state to Monado
//   state                            Print current state as JSON to stdout
//   reset                            Reset to server's initial state
//   quit / exit                      Disconnect and exit
//
// Each command prints "ok" on success or "error: <message>" on failure.
//
// The struct layout (r_remote_data et al.) is taken directly from monado's
// r_interface.h — never copied — so a recompile always reflects the current
// protocol version.

// r_interface.h must come first (it guards against winsock ordering issues).
#include "remote/r_interface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

// ---------------------------------------------------------------------------
// Minimal TCP helpers (we implement the wire I/O directly so we don't need
// to link against libdrv_remote — just its headers for the struct types).
// ---------------------------------------------------------------------------

static int
tcp_connect(const char *host, uint16_t port)
{
	const char *ip = strcmp(host, "localhost") == 0 ? "127.0.0.1" : host;

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
		fprintf(stderr, "error: invalid address '%s'\n", host);
		return -1;
	}

	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("connect");
		close(fd);
		return -1;
	}
	return fd;
}

static int
read_full(int fd, void *buf, size_t size)
{
	size_t done = 0;
	while (done < size) {
		ssize_t n = read(fd, (uint8_t *)buf + done, size - done);
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}
	return 0;
}

static int
write_full(int fd, const void *buf, size_t size)
{
	size_t done = 0;
	while (done < size) {
		ssize_t n = write(fd, (const uint8_t *)buf + done, size - done);
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}
	return 0;
}

static int
read_packet(int fd, struct r_remote_data *out)
{
	if (read_full(fd, out, sizeof(*out)) < 0)
		return -1;
	if (out->header != R_HEADER_VALUE) {
		fprintf(stderr, "error: bad packet header\n");
		return -1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// State printing
// ---------------------------------------------------------------------------

static void
print_pose_json(const char *key, const struct xrt_pose *p)
{
	printf("    \"%s\":{\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,"
	       "\"qx\":%.4f,\"qy\":%.4f,\"qz\":%.4f,\"qw\":%.4f}",
	       key, p->position.x, p->position.y, p->position.z,
	       p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w);
}

static void
print_controller_json(const struct r_remote_controller_data *c)
{
	print_pose_json("pose", &c->pose);
	printf(",\n    \"trigger\":%.4f,\"squeeze\":%.4f,"
	       "\"thumbstick\":[%.4f,%.4f],\n"
	       "    \"trigger_click\":%s,\"a_click\":%s,\"b_click\":%s,"
	       "\"system_click\":%s,\"thumbstick_click\":%s\n",
	       c->trigger_value.x, c->squeeze_value.x,
	       c->thumbstick.x, c->thumbstick.y,
	       c->trigger_click ? "true" : "false",
	       c->a_click ? "true" : "false",
	       c->b_click ? "true" : "false",
	       c->system_click ? "true" : "false",
	       c->thumbstick_click ? "true" : "false");
}

static void
print_state_json(const struct r_remote_data *d)
{
	printf("{\n");
	print_pose_json("head", &d->head.center);
	printf(",\n  \"left\":{\n");
	print_controller_json(&d->left);
	printf("  },\n  \"right\":{\n");
	print_controller_json(&d->right);
	printf("  }\n}\n");
}

// ---------------------------------------------------------------------------
// Command processing
// ---------------------------------------------------------------------------

static bool
parse_pose(const char *str, struct xrt_pose *out)
{
	return sscanf(str, "%f %f %f %f %f %f %f", &out->position.x, &out->position.y,
	              &out->position.z, &out->orientation.x, &out->orientation.y,
	              &out->orientation.z, &out->orientation.w) == 7;
}

static bool
process_line(char *line, int fd, struct r_remote_data *state, const struct r_remote_data *initial)
{
	char *end = line + strlen(line) - 1;
	while (end >= line && (*end == '\n' || *end == '\r' || *end == ' '))
		*end-- = '\0';

	if (line[0] == '\0' || line[0] == '#')
		return true;

	char cmd[64] = {0};
	const char *rest = "";
	int n = 0;
	sscanf(line, "%63s%n", cmd, &n);
	if (n > 0 && line[n] == ' ')
		rest = line + n + 1;

	if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0)
		return false;

	if (strcmp(cmd, "send") == 0) {
		state->header = R_HEADER_VALUE;
		if (write_full(fd, state, sizeof(*state)) < 0) {
			puts("error: write failed - disconnected?");
			return false;
		}
		puts("ok");
		goto done;
	}

	if (strcmp(cmd, "state") == 0) {
		print_state_json(state);
		puts("ok");
		goto done;
	}

	if (strcmp(cmd, "reset") == 0) {
		memcpy(state, initial, sizeof(*state));
		puts("ok");
		goto done;
	}

	if (strcmp(cmd, "head") == 0) {
		if (!parse_pose(rest, &state->head.center))
			puts("error: head requires X Y Z QX QY QZ QW");
		else
			puts("ok");
		goto done;
	}

	if (strcmp(cmd, "left_pose") == 0 || strcmp(cmd, "right_pose") == 0) {
		struct xrt_pose p;
		if (!parse_pose(rest, &p)) {
			printf("error: %s requires X Y Z QX QY QZ QW\n", cmd);
		} else {
			(cmd[0] == 'l' ? &state->left : &state->right)->pose = p;
			puts("ok");
		}
		goto done;
	}

#define AXIS_CMD(name, field)                                                                      \
	if (strcmp(cmd, name) == 0) {                                                              \
		float v;                                                                           \
		if (sscanf(rest, "%f", &v) != 1)                                                   \
			printf("error: %s requires VALUE\n", name);                                \
		else {                                                                             \
			(field) = v;                                                               \
			puts("ok");                                                                \
		}                                                                                  \
		goto done;                                                                         \
	}

	AXIS_CMD("left_trigger", state->left.trigger_value.x)
	AXIS_CMD("right_trigger", state->right.trigger_value.x)
	AXIS_CMD("left_squeeze", state->left.squeeze_value.x)
	AXIS_CMD("right_squeeze", state->right.squeeze_value.x)

#undef AXIS_CMD

	if (strcmp(cmd, "left_thumbstick") == 0 || strcmp(cmd, "right_thumbstick") == 0) {
		struct xrt_vec2 *ts =
		    cmd[0] == 'l' ? &state->left.thumbstick : &state->right.thumbstick;
		if (sscanf(rest, "%f %f", &ts->x, &ts->y) != 2)
			printf("error: %s requires X Y\n", cmd);
		else
			puts("ok");
		goto done;
	}

#define BTN_CMD(name, field)                                                                       \
	if (strcmp(cmd, name) == 0) {                                                              \
		int v;                                                                             \
		if (sscanf(rest, "%d", &v) != 1)                                                   \
			printf("error: %s requires 0 or 1\n", name);                               \
		else {                                                                             \
			(field) = (v != 0);                                                        \
			puts("ok");                                                                \
		}                                                                                  \
		goto done;                                                                         \
	}

	BTN_CMD("left_trigger_click", state->left.trigger_click)
	BTN_CMD("left_a_click", state->left.a_click)
	BTN_CMD("left_b_click", state->left.b_click)
	BTN_CMD("left_system_click", state->left.system_click)
	BTN_CMD("left_thumbstick_click", state->left.thumbstick_click)
	BTN_CMD("right_trigger_click", state->right.trigger_click)
	BTN_CMD("right_a_click", state->right.a_click)
	BTN_CMD("right_b_click", state->right.b_click)
	BTN_CMD("right_system_click", state->right.system_click)
	BTN_CMD("right_thumbstick_click", state->right.thumbstick_click)

#undef BTN_CMD

	printf("error: unknown command '%s'\n", cmd);

done:
	fflush(stdout);
	return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int
main(int argc, char **argv)
{
	const char *host = "127.0.0.1";
	uint16_t port = 4242;

	if (argc >= 2) {
		if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
			fprintf(stderr,
			        "Usage: %s [host [port]]\n"
			        "Default: 127.0.0.1 4242\n"
			        "Requires monado-service started with:\n"
			        "  P_OVERRIDE_ACTIVE_CONFIG=remote XRT_COMPOSITOR_FORCE_XCB=1 monado-service\n",
			        argv[0]);
			return 0;
		}
		host = argv[1];
	}
	if (argc >= 3)
		port = (uint16_t)atoi(argv[2]);

	fprintf(stderr, "Connecting to %s:%u ...\n", host, port);
	int fd = tcp_connect(host, port);
	if (fd < 0) {
		fprintf(stderr,
		        "error: connection failed - is monado-service running with "
		        "P_OVERRIDE_ACTIVE_CONFIG=remote ?\n");
		return 1;
	}
	fprintf(stderr, "Connected.\n");

	// Server sends reset state then latest state on connect.
	struct r_remote_data initial = {0};
	struct r_remote_data latest = {0};
	if (read_packet(fd, &initial) < 0 || read_packet(fd, &latest) < 0) {
		fprintf(stderr, "error: failed to read initial state from server\n");
		close(fd);
		return 1;
	}
	fprintf(stderr, "Ready.\n");

	struct r_remote_data state = latest;

	char line[512];
	while (fgets(line, sizeof(line), stdin)) {
		if (!process_line(line, fd, &state, &initial))
			break;
	}

	fprintf(stderr, "Disconnecting.\n");
	close(fd);
	return 0;
}
