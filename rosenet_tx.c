// rosenet_tx.c - Network transmitter: subscribes to local topic, sends over network
//
// Usage: rosenet_tx <topic> <host> <port> [--udp]
//        rosenet_tx --topic <t> --host <h> --port <p> [--udp] [--message_size N] [--read_last 0|1]

#include "ros3.h"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// --- CLI helpers -------------------------------------------------------------

static const char *find_arg(int argc, char **argv, const char *name, const char *fallback) {
	for(int i = 1; i + 1 < argc; ++i) {
		if(strcmp(argv[i], name) == 0) return argv[i + 1];
	}
	return fallback;
}

static int has_flag(int argc, char **argv, const char *flag) {
	for(int i = 1; i < argc; ++i) {
		if(strcmp(argv[i], flag) == 0) return 1;
	}
	return 0;
}

static i64 find_arg_i64(int argc, char **argv, const char *name, i64 fallback) {
	const char *s = find_arg(argc, argv, name, NULL);
	if(!s) return fallback;
	return atoll(s);
}

// --- Network helpers ---------------------------------------------------------

// Send exactly len bytes (handles partial writes).
static int send_all(int fd, const void *buf, u64 len) {
	const u8 *p = (const u8 *) buf;
	while(len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if(n <= 0) return -1;
		p += n;
		len -= (u64) n;
	}
	return 0;
}

// --- Main --------------------------------------------------------------------

int main(int argc, char **argv) {
	// Named args take priority over positional
	const char *topic = find_arg(argc, argv, "--topic", NULL);
	const char *host = find_arg(argc, argv, "--host", NULL);
	i64 port = find_arg_i64(argc, argv, "--port", -1);
	int use_udp = has_flag(argc, argv, "--udp");
	i64 message_size = find_arg_i64(argc, argv, "--message_size", 0);
	i64 read_last = find_arg_i64(argc, argv, "--read_last", 0);

	// Fall back to positional: <topic> <host> <port> [--udp]
	if(!topic && argc >= 2 && argv[1][0] != '-') topic = argv[1];
	if(!host && argc >= 3 && argv[2][0] != '-') host = argv[2];
	if(port < 0 && argc >= 4 && argv[3][0] != '-') port = atoi(argv[3]);
	if(!use_udp && argc > 4 && strcmp(argv[4], "--udp") == 0) use_udp = 1;

	if(!topic || !host || port < 0) {
		fprintf(stderr, "Usage: %s <topic> <host> <port> [--udp]\n", argv[0]);
		fprintf(stderr, "       %s --topic T --host H --port P [--udp] [--message_size N] [--read_last 0|1]\n", argv[0]);
		return 1;
	}

	// Compute buffer size: generous formula for benchmark ring buffer compatibility
	i64 buf_size = 65536;
	if(message_size > 0) {
		i64 fixed_bytes = 48; // 6 * sizeof(u64)
		i64 payload_words = (message_size > fixed_bytes)
		    ? (message_size - fixed_bytes + 7) / 8
		    : 0;
		buf_size = (7 + payload_words) * 16 + 64;
		if(buf_size < 4 * 1024 * 1024) buf_size = 4 * 1024 * 1024;
	}

	// Init as rose node
	rose_node *node = rose_init(argc, argv, "rosenet_tx", NULL, NULL);

	// Subscribe to source topic with retry
	rose_subscriber *sub = NULL;
	for(int i = 0; i < 5000 && !sub; ++i) {
		sub = rose_create_sub(node, (char *) topic, -1, 0, NULL);
		if(!sub) rose_sleep(1e-3);
	}
	if(!sub) {
		fprintf(stderr, "rosenet_tx: failed to subscribe to %s\n", topic);
		return 1;
	}

	// Setup network socket
	int sock = socket(AF_INET, use_udp ? SOCK_DGRAM : SOCK_STREAM, 0);
	if(sock < 0) {
		perror("socket");
		return 1;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u16) port);
	if(inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
		fprintf(stderr, "Invalid address: %s\n", host);
		return 1;
	}

	if(!use_udp) {
		rose_info("Connecting to %s:%lld (TCP)...", host, (long long) port);
		while(rose_ok(node)) {
			if(connect(sock, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
				break;
			}
			rose_sleep(0.01);
			close(sock);
			sock = socket(AF_INET, SOCK_STREAM, 0);
		}
		if(!rose_ok(node)) return 1;
		// Disable Nagle for low latency
		int flag = 1;
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
		rose_info("Connected to %s:%lld", host, (long long) port);
	} else {
		rose_info("Sending to %s:%lld (UDP)", host, (long long) port);
	}

	// Main loop: spin while data flows, sleep after 1ms idle
	mirage_msg *msg = mirage_create(buf_size, NULL);
	u64 count = 0;
	u64 idle_start = 0;

	while(rose_ok(node)) {
		msg->length = msg->max_length;
		i64 r = read_last ? rose_readlast(sub, msg) : rose_read(sub, msg);
		if(r >= 0) {
			idle_start = 0;
			if(use_udp) {
				sendto(sock, msg->data, (size_t) msg->length, 0,
				       (struct sockaddr *) &addr, sizeof(addr));
			} else {
				// Length-prefixed frame: [4 bytes LE uint32][payload]
				u32 len = (u32) msg->length;
				if(len == 0) continue;
				if(send_all(sock, &len, sizeof(len)) < 0) break;
				if(send_all(sock, msg->data, len) < 0) break;
			}
			if(++count % 10000 == 0) {
				rose_info("Sent %lu messages", (unsigned long) count);
			}
		} else {
			u64 now = rose_now();
			if(!idle_start) {
				idle_start = now;
			} else if(rose_nanoseconds(now, idle_start) > 1e6) {
				rose_sleep(1e-3);
			}
		}
	}

	close(sock);
	return 0;
}
