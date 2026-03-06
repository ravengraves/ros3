// rosenet_tx.c - Network transmitter: subscribes to local topic, sends over network
//
// Usage: rosenet_tx <topic> <host> <port> [--udp]
//        rosenet_tx --topic <t> --host <h> --port <p> [--udp] [--message_size N] [--read_last 0|1]

#include "ros3.h"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sched.h> 
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
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

// Send length-prefixed frame [4-byte LE u32 header][payload] via scatter-gather I/O.
// Single writev() syscall for header+payload instead of two separate send() calls.
static int send_frame(int fd, const void *data, u32 len) {
	struct iovec iov[2] = {
	    {.iov_base = &len, .iov_len = 4},
	    {.iov_base = (void *) data, .iov_len = len},
	};
	struct iovec *v = iov;
	int cnt = 2;
	while(cnt > 0) {
		ssize_t n = writev(fd, v, cnt);
		if(n <= 0) return -1;
		while(cnt > 0 && (size_t) n >= v[0].iov_len) {
			n -= (ssize_t) v[0].iov_len;
			++v;
			--cnt;
		}
		if(cnt > 0 && n > 0) {
			v[0].iov_base = (u8 *) v[0].iov_base + n;
			v[0].iov_len -= (size_t) n;
		}
	}
	return 0;
}

// --- Main --------------------------------------------------------------------

int main(int argc, char **argv) {
	signal(SIGPIPE, SIG_IGN);

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
		int flag = 1;
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
		rose_info("Connected to %s:%lld", host, (long long) port);
	} else {
		rose_info("Sending to %s:%lld (UDP)", host, (long long) port);
	}

	// Main loop: spin while data flows, sleep after 1ms idle
	mirage_msg *msg = mirage_create(buf_size, NULL);
	u64 count = 0;
	u64 idle_spins = 0;

	while(rose_ok(node)) {
		msg->length = msg->max_length;
		i64 r = read_last ? rose_readlast(sub, msg) : rose_read(sub, msg);
		if(r >= 0) {
			idle_spins = 0;
			if(use_udp) {
				sendto(sock, msg->data, (size_t) msg->length, 0,
				       (struct sockaddr *) &addr, sizeof(addr));
			} else {
				u32 len = (u32) msg->length;
				if(len == 0) continue;
				if(send_frame(sock, msg->data, len) < 0) break;
			}
			if(++count % 10000 == 0) {
				rose_info("Sent %lu messages", (unsigned long) count);
			}
		} else {
			++idle_spins;                                                      
			if(idle_spins < 1000) {                                            
				sched_yield();                                                   
			} else {
				continue;
				// rose_sleep(1e-3);
			}
		}
	}

	close(sock);
	return 0;
}
