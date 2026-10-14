// rosenet_rx.c - Network receiver: listens on port, publishes to local topic
//
// Usage: rosenet_rx <topic> <port> <msg_size> <message_rate> [--udp]
//        rosenet_rx --topic T --port P [--udp] [--message_size N] [--message_rate R]

#include "core.h"
#include "ros3.h"

#include "../common/liaison.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// --- CLI helpers -------------------------------------------------------------

static int is_help_arg(const char *arg) {
	return strcmp(arg, "-h") == 0
			|| strcmp(arg, "--help") == 0
			|| strcmp(arg, "/h") == 0
			|| strcmp(arg, "/help") == 0
			|| strcmp(arg, "/?") == 0;
}

static int is_option_token(const char *arg) {
	return arg && arg[0] == '-';
}

static int arg_is(const char *arg, const char *name) {
	if(strcmp(arg, name) == 0) {
		return 1;
	}
	if(name[0] == '-' && name[1] == '-') {
		char dos_name[64];
		snprintf(dos_name, sizeof(dos_name), "/%s", name + 2);
		return strcmp(arg, dos_name) == 0;
	}
	return 0;
}

static void print_usage(const char *argv0) {
	fprintf(stderr, "Usage: %s <topic> <port> <msg_size> <message_rate> [--udp]\n", argv0);
	fprintf(stderr, "       %s --topic T --port P [--udp] [--message_size N] [--message_rate R]\n", argv0);
	fprintf(stderr, "       %s /topic T /port P [/udp] [/message_size N] [/message_rate R]\n", argv0);
}

static const char *find_arg(int argc, char **argv, const char *name, const char *fallback) {
	for(int i = 1; i + 1 < argc; ++i) {
		if(arg_is(argv[i], name)) {
			return argv[i + 1];
		}
	}
	return fallback;
}

static int has_flag(int argc, char **argv, const char *flag) {
	for(int i = 1; i < argc; ++i) {
		if(arg_is(argv[i], flag)) {
			return 1;
		}
	}
	return 0;
}

static int parse_i64_arg(const char *text, i64 *value) {
	char *end = NULL;
	long long parsed;

	if(!text || !value) {
		return -1;
	}

	errno = 0;
	parsed = strtoll(text, &end, 10);
	if(errno != 0 || end == text || *end != '\0') {
		return -1;
	}

	*value = (i64) parsed;
	return 0;
}

static i64 find_arg_i64(int argc, char **argv, const char *name, i64 fallback) {
	const char *s = find_arg(argc, argv, name, NULL);
	if(!s) {
		return has_flag(argc, argv, name) ? -1 : fallback;
	}
	if(parse_i64_arg(s, &fallback) != 0) {
		return -1;
	}
	return fallback;
}

int rosenet_rx_main(int argc, char **argv) {
	if(argc > 1 && is_help_arg(argv[1])) {
		print_usage(argv[0]);
		return 0;
	}
	const char *topic = find_arg(argc, argv, "--topic", NULL);
	i64 port = find_arg_i64(argc, argv, "--port", 0);
	i64 size = find_arg_i64(argc, argv, "--message_size", 65536);
	i64 slots = find_arg_i64(argc, argv, "--message_rate", find_arg_i64(argc, argv, "--queue_depth", 64));
	int udp = has_flag(argc, argv, "--udp");
	if(!topic && argc >= 2 && !is_option_token(argv[1])) {
		topic = argv[1];
		if(argc < 5 || parse_i64_arg(argv[2], &port) || parse_i64_arg(argv[3], &size) || parse_i64_arg(argv[4], &slots)) {
			print_usage(argv[0]);
			return 1;
		}
		/* Explicit named sizing has priority over positional values. */
		size = find_arg_i64(argc, argv, "--message_size", size);
		slots = find_arg_i64(argc, argv, "--message_rate", slots);
		port = find_arg_i64(argc, argv, "--port", port);
	}
	if(!topic || port <= 0 || port > UINT16_MAX || size <= 0 || slots <= 0 || !liaison_sizes_valid(size, slots)) {
		fprintf(stderr, "rosenet_rx: invalid configuration (size 1..16MiB; ring budget 64MiB)\n");
		return 1;
	}
	int status = 1, sock = -1, client = -1;
	rose_node *node = NULL;
	mirage_msg *msg = NULL;
	u64 count = 0, dropped = 0;
	node = rose_init(argc, argv, "rosenet_rx", NULL, NULL);
	if(!node) {
		goto cleanup;
	}
	liaison_signals();
	rose_publisher *pub = rose_create_pub(node, (char *) topic, -1, size, slots);
	if(!pub) {
		fprintf(stderr, "rosenet_rx: publisher creation failed\n");
		goto cleanup;
	}
	msg = mirage_create(size, NULL);
	if(!msg) {
		fprintf(stderr, "liaison: message allocation failed\n");
		goto cleanup;
	}
	sock = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
	if(sock < 0) {
		perror("rosenet_rx: socket");
		goto cleanup;
	}
	int opt = 1;
	if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
		perror("rosenet_rx: setsockopt");
		goto cleanup;
	}
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((u16) port), .sin_addr.s_addr = INADDR_ANY};
	if(bind(sock, (struct sockaddr *) &addr, sizeof(addr))) {
		perror("rosenet_rx: bind");
		goto cleanup;
	}
	client = sock;
	if(!udp) {
		if(listen(sock, 1)) {
			perror("rosenet_rx: listen");
			goto cleanup;
		}
		while(liaison_running(node)) {
			struct pollfd pfd = {.fd = sock, .events = POLLIN};
			int r = poll(&pfd, 1, 10);
			if(r < 0 && errno == EINTR) {
				continue;
			}
			if(r < 0 || (r > 0 && !(pfd.revents & POLLIN))) {
				goto cleanup;
			}
			if(!r) {
				continue;
			}
			client = accept(sock, NULL, NULL);
			if(client < 0 && errno == EINTR) {
				continue;
			}
			if(client < 0) {
				perror("rosenet_rx: accept");
				goto cleanup;
			}
			break;
		}
		if(!liaison_running(node)) {
			status = 0;
			goto cleanup;
		}
	}
	status = 0;
	while(liaison_running(node)) {
		struct pollfd pfd = {.fd = client, .events = POLLIN};
		int r = poll(&pfd, 1, 10);
		if(r < 0 && errno == EINTR) {
			continue;
		}
		if(r < 0 || (r > 0 && !(pfd.revents & POLLIN))) {
			status = 1;
			break;
		}
		if(!r) {
			continue;
		}
		ssize_t n;
		if(udp) {
			struct iovec iov = {.iov_base = msg->data, .iov_len = (size_t) size};
			struct msghdr packet = {.msg_iov = &iov, .msg_iovlen = 1};
			n = recvmsg(client, &packet, MSG_DONTWAIT);
			if(n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
				continue;
			}
			if(n < 0) {
				perror("rosenet_rx: recvmsg");
				status = 1;
				break;
			}
			if((packet.msg_flags & MSG_TRUNC) || (uint64_t) n > (uint64_t) size || n > LIAISON_UDP_MAX || n == 0) {
				++dropped;
				fprintf(stderr, "rosenet_rx: dropped empty/oversized/truncated UDP datagram\n");
				continue;
			}
		} else {
			uint8_t hdr[4];
			u64 start = rose_now();
			if(liaison_socket_io(client, hdr, 4, 0, start)) {
				perror("rosenet_rx: frame header");
				status = 1;
				break;
			}
			uint32_t len = (uint32_t) hdr[0] | ((uint32_t) hdr[1] << 8) | ((uint32_t) hdr[2] << 16) | ((uint32_t) hdr[3] << 24);
			if(!len || len > (uint64_t) size) {
				fprintf(stderr, "rosenet_rx: invalid TCP frame length\n");
				status = 1;
				break;
			}
			if(liaison_socket_io(client, msg->data, len, 0, start)) {
				perror("rosenet_rx: payload");
				status = 1;
				break;
			}
			n = len;
		}
		msg->length = n;
		msg->cursor = n;
		if(rose_publish(pub, msg) < 0) {
			fprintf(stderr, "rosenet_rx: publish failed\n");
			status = 1;
			break;
		}
		++count;
	}
cleanup:
	if(liaison_stop) {
		status = 0;
	}
	fprintf(stderr, "rosenet_rx: published=%llu dropped=%llu\n", (unsigned long long) count, (unsigned long long) dropped);
	if(client >= 0 && client != sock) {
		close(client);
	}
	if(sock >= 0) {
		close(sock);
	}
	if(msg) {
		mirage_destroy(&msg, NULL);
	}
	if(node) {
		rose_shutdown(node);
	}
	return status;
}
