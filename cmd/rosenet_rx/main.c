// rosenet_rx.c - Network receiver: listens on port, publishes to local topic
//
// Usage: rosenet_rx <topic> <port> <msg_size> <ring_slots> [--udp]
//        rosenet_rx --topic T --port P [--udp] [--message_size N] [--queue_depth D]

#include "ros3.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
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

// Read exactly n bytes from socket (blocking).
static ssize_t recv_exact(int sock, void *buf, u64 n) {
	u8 *p = (u8 *) buf;
	u64 total = 0;
	while(total < n) {
		ssize_t r = recv(sock, p + total, n - total, 0);
		if(r <= 0) return r;
		total += (u64) r;
	}
	return (ssize_t) total;
}

// --- Main --------------------------------------------------------------------

int main(int argc, char **argv) {
	// Named args take priority over positional
	const char *topic = find_arg(argc, argv, "--topic", NULL);
	i64 port = find_arg_i64(argc, argv, "--port", -1);
	int use_udp = has_flag(argc, argv, "--udp");
	i64 message_size = find_arg_i64(argc, argv, "--message_size", 0);
	i64 queue_depth = find_arg_i64(argc, argv, "--queue_depth", 0);

	// Fall back to positional: <topic> <port> <msg_size> <slots> [--udp]
	i64 pos_msg_size = 0;
	i64 pos_slots = 0;
	if(!topic && argc >= 2 && argv[1][0] != '-') topic = argv[1];
	if(port < 0 && argc >= 3 && argv[2][0] != '-') port = atoi(argv[2]);
	if(argc >= 4 && argv[3][0] != '-') pos_msg_size = atoi(argv[3]);
	if(argc >= 5 && argv[4][0] != '-') pos_slots = atoi(argv[4]);
	if(!use_udp && argc > 5 && strcmp(argv[5], "--udp") == 0) use_udp = 1;

	if(!topic || port < 0) {
		fprintf(stderr, "Usage: %s <topic> <port> <msg_size> <slots> [--udp]\n", argv[0]);
		fprintf(stderr, "       %s --topic T --port P [--udp] [--message_size N] [--queue_depth D]\n", argv[0]);
		return 1;
	}

	// Compute buffer size and queue depth
	i64 buf_size;
	i64 slots;

	if(message_size > 0) {
		// Benchmark mode: generous formula from benchmark_common.h
		i64 fixed_bytes = 48; // 6 * sizeof(u64)
		i64 payload_words = (message_size > fixed_bytes)
		    ? (message_size - fixed_bytes + 7) / 8
		    : 0;
		buf_size = (7 + payload_words) * 16 + 64;
		if(buf_size < 4 * 1024 * 1024) buf_size = 4 * 1024 * 1024;

		if(queue_depth > 0) {
			slots = queue_depth;
		} else {
			// Auto-compute: target 512 MB total ring buffer
			u64 padded = (16 + (u64) buf_size + 63) & ~(u64) 63;
			slots = (i64) ((512ULL * 1024 * 1024) / padded);
			if(slots < 2) slots = 2;
		}
	} else if(pos_msg_size > 0) {
		// Positional fallback
		buf_size = pos_msg_size;
		slots = pos_slots > 0 ? pos_slots : 1024;
	} else {
		buf_size = 65536;
		slots = 1024;
	}

	// Init as rose node
	rose_node *node = rose_init(argc, argv, "rosenet_rx", NULL, NULL);

	// Create publisher with retry
	rose_publisher *pub = NULL;
	for(int i = 0; i < 5000 && !pub; ++i) {
		pub = rose_create_pub(node, (char *) topic, -1, (u64) buf_size, (u64) slots);
		if(!pub) rose_sleep(1e-3);
	}
	if(!pub) {
		fprintf(stderr, "rosenet_rx: failed to create topic %s\n", topic);
		return 1;
	}

	// Setup network socket
	int sock = socket(AF_INET, use_udp ? SOCK_DGRAM : SOCK_STREAM, 0);
	if(sock < 0) {
		perror("socket");
		return 1;
	}

	int opt = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u16) port);
	addr.sin_addr.s_addr = INADDR_ANY;

	if(bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}

	int client = sock;
	if(!use_udp) {
		listen(sock, 1);
		rose_info("Waiting for TCP on port %lld...", (long long) port);
		client = accept(sock, NULL, NULL);
		if(client < 0) {
			perror("accept");
			return 1;
		}
		int flag = 1;
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
		close(sock);
		rose_info("Connected on port %lld", (long long) port);
	} else {
		rose_info("Listening for UDP on port %lld", (long long) port);
	}

	// Main loop: try non-blocking recv first, fall back to poll on EAGAIN
	mirage_msg *msg = mirage_create(buf_size, NULL);
	u64 count = 0;
	struct pollfd pfd = {.fd = client, .events = POLLIN};

	while(rose_ok(node)) {
		ssize_t n;
		if(use_udp) {
			n = recvfrom(client, msg->data, (size_t) buf_size, MSG_DONTWAIT, NULL, NULL);
			if(n < 0) {
				if(errno != EAGAIN && errno != EWOULDBLOCK) break;
				if(poll(&pfd, 1, 1) <= 0) continue;
				if(pfd.revents & (POLLERR | POLLHUP)) break;
				continue;
			}
		} else {
			// Non-blocking probe for frame header
			u32 len;
			ssize_t hr = recv(client, &len, 4, MSG_DONTWAIT);
			if(hr < 0) {
				if(errno != EAGAIN && errno != EWOULDBLOCK) break;
				if(poll(&pfd, 1, 1) <= 0) continue;
				if(pfd.revents & (POLLERR | POLLHUP)) break;
				continue;
			}
			if(hr == 0) break;
			// Complete partial header if needed (blocking)
			if(hr < 4 && recv_exact(client, (u8 *) &len + hr, (u64) (4 - hr)) != (ssize_t) (4 - hr))
				break;
			if(len == 0 || len > (u32) buf_size) break;
			// Read payload (blocking — committed once we have the header)
			n = recv_exact(client, msg->data, len);
			if(n != (ssize_t) len) break;
		}

		if(n > 0) {
			msg->length = n;
			msg->cursor = n;
			rose_publish(pub, msg);
			if(++count % 10000 == 0) {
				rose_info("Received %lu messages", (unsigned long) count);
			}
		}
	}

	close(client);
	return 0;
}
