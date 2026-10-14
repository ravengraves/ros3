// rosenet_tx.c - Network transmitter: subscribes to local topic, sends over network
//
// Usage: rosenet_tx <topic> <host> <port> [--udp]
//        rosenet_tx --topic <t> --host <h> --port <p> [--udp] [--message_size N] [--read_last 0|1]

#include "core.h"
#include "ros3.h"

#include "../common/liaison.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
	fprintf(stderr, "Usage: %s <topic> <host> <port> [--udp]\n", argv0);
	fprintf(stderr, "       %s --topic T --host H --port P [--udp] [--message_size N] [--read_last 0|1]\n", argv0);
	fprintf(stderr, "       %s /topic T /host H /port P [/udp] [/message_size N] [/read_last 0|1]\n", argv0);
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

/* Retain the existing little-endian TCP wire format, independent of host CPU. */
static int send_frame(int fd, const void *data, u32 len) {
	uint8_t hdr[4] = {len & 255, (len >> 8) & 255, (len >> 16) & 255, (len >> 24) & 255};
	u64 start = rose_now();
	if(liaison_socket_io(fd, hdr, 4, 1, start)) {
		return -1;
	}
	return liaison_socket_io(fd, (void *) data, len, 1, start);
}

int rosenet_tx_main(int argc, char **argv) {
	if(argc > 1 && is_help_arg(argv[1])) {
		print_usage(argv[0]);
		return 0;
	}
	signal(SIGPIPE, SIG_IGN);
	const char *topic = find_arg(argc, argv, "--topic", NULL);
	const char *host = find_arg(argc, argv, "--host", NULL);
	i64 port = find_arg_i64(argc, argv, "--port", 0);
	i64 size = find_arg_i64(argc, argv, "--message_size", 65536);
	i64 latest = find_arg_i64(argc, argv, "--read_last", 0);
	int udp = has_flag(argc, argv, "--udp");
	if(!topic && argc > 1 && !is_option_token(argv[1])) {
		topic = argv[1];
	}
	if(!host && argc > 2 && !is_option_token(argv[2])) {
		host = argv[2];
	}
	if(!find_arg(argc, argv, "--port", NULL) && argc > 3 && parse_i64_arg(argv[3], &port)) {
		port = 0;
	}
	if(!topic || !host || port <= 0 || port > UINT16_MAX || size <= 0 || size > LIAISON_MAX_MESSAGE || (latest != 0 && latest != 1)) {
		fprintf(stderr, "rosenet_tx: invalid configuration (size 1..16MiB, read_last 0|1)\n");
		return 1;
	}
	int status = 1, sock = -1;
	rose_node *node = NULL;
	mirage_msg *msg = NULL;
	u64 count = 0, dropped = 0;
	node = rose_init(argc, argv, "rosenet_tx", NULL, NULL);
	if(!node) {
		goto cleanup;
	}
	liaison_signals();
	rose_subscriber *sub = NULL;
	for(int i = 0; i < 5000 && liaison_running(node) && !sub; ++i) {
		sub = rose_create_sub(node, (char *) topic, -1, 0, NULL);
		if(!sub) {
			rose_sleep(0.001);
		}
	}
	if(!sub || !(msg = liaison_source_buffer(sub))) {
		fprintf(stderr, "rosenet_tx: source unavailable/invalid\n");
		goto cleanup;
	}
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((u16) port)};
	if(inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		fprintf(stderr, "rosenet_tx: invalid IPv4 address\n");
		goto cleanup;
	}
	sock = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
	if(sock < 0) {
		perror("rosenet_tx: socket");
		goto cleanup;
	}
	if(!udp) {
		/* Allow receiver startup races, but bound the entire connect phase. */
		u64 start = rose_now();
		for(;;) {
			int flags = fcntl(sock, F_GETFL, 0);
			if(flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK)) {
				goto cleanup;
			}
			int rc = connect(sock, (struct sockaddr *) &addr, sizeof(addr));
			int err = rc < 0 ? errno : 0;
			if(err == EINPROGRESS || err == EINTR) {
				if(liaison_wait(sock, POLLOUT, start)) {
					err = errno;
				} else {
					socklen_t len = sizeof(err);
					if(getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len)) {
						err = errno;
					}
				}
			}
			if(!err) {
				break;
			}
			if(!liaison_running(node)) {
				goto cleanup;
			}
			if(!liaison_remaining(start) || (err != ECONNREFUSED && err != EIO)) {
				errno = err;
				perror("rosenet_tx: connect");
				goto cleanup;
			}
			close(sock);
			rose_sleep(0.01);
			sock = socket(AF_INET, SOCK_STREAM, 0);
			if(sock < 0) {
				perror("rosenet_tx: socket");
				goto cleanup;
			}
		}
		int flag = 1;
		if(setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag))) {
			goto cleanup;
		}
	}

	status = 0;
	while(liaison_running(node)) {
		i64 r = latest ? rose_readlast(sub, msg) : rose_read(sub, msg);
		if(r == -1) {
			rose_sleep(0.001);
			continue;
		}
		if(r < -1) {
			fprintf(stderr, "rosenet_tx: source read failed (%lld)\n", (long long) r);
			status = 1;
			break;
		}
		if(!msg->length || msg->length > size || msg->length > UINT32_MAX || (udp && msg->length > LIAISON_UDP_MAX)) {
			++dropped;
			fprintf(stderr, "rosenet_tx: dropped empty/oversized source sample (%llu bytes)\n", (unsigned long long) msg->length);
			continue;
		}
		if(udp) {
			u64 start = rose_now();
			ssize_t n;
			do {
				n = sendto(sock, msg->data, msg->length, MSG_DONTWAIT, (struct sockaddr *) &addr, sizeof(addr));
			} while(n < 0 && errno == EINTR && liaison_remaining(start));
			if(n != (ssize_t) msg->length) {
				perror("rosenet_tx: sendto");
				status = 1;
				break;
			}
		} else if(send_frame(sock, msg->data, msg->length)) {
			perror("rosenet_tx: send frame");
			status = 1;
			break;
		}
		++count;
	}
cleanup:
	if(liaison_stop) {
		status = 0;
	}
	fprintf(stderr, "rosenet_tx: sent=%llu dropped=%llu\n", (unsigned long long) count, (unsigned long long) dropped);
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
