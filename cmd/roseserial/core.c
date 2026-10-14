// roseserial.c - Bidirectional serial bridge for microcontrollers
//
// Usage: roseserial <device> <rx_topic> <tx_topic> <baud> <msg_size> <slots>
// Framing: 0x7E [version=1][u16 LE length][payload][u32 LE CRC] 0x7E
//          (see frame.h)

#define _GNU_SOURCE

#include "core.h"
#include "ros3.h"

#include "../common/liaison.h"
#include "frame.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int is_help_arg(const char *arg) {
	return strcmp(arg, "-h") == 0
			|| strcmp(arg, "--help") == 0
			|| strcmp(arg, "/h") == 0
			|| strcmp(arg, "/help") == 0
			|| strcmp(arg, "/?") == 0;
}

static void print_usage(const char *argv0) {
	fprintf(stderr, "Usage: %s <device> <rx_topic> <tx_topic> <baud> <msg_size> <slots>\n", argv0);
}

static int parse_int_arg(const char *text, int *value) {
	char *end = NULL;
	long parsed;

	if(!text || !value) {
		return -1;
	}

	errno = 0;
	parsed = strtol(text, &end, 10);
	if(errno != 0 || end == text || *end != '\0'
	   || parsed < INT_MIN || parsed > INT_MAX) {
		return -1;
	}

	*value = (int) parsed;
	return 0;
}

static speed_t baud_to_speed(int baud) {
	switch(baud) {
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 921600: return B921600;
	case 1000000: return B1000000;
	case 2000000: return B2000000;
	case 3000000: return B3000000;
	case 4000000: return B4000000;
	default: return B0;
	}
}

static int serial_open(const char *device, int baud) {
	speed_t speed = baud_to_speed(baud);
	if(speed == B0) {
		return -1;
	}

	int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if(fd < 0) {
		return -1;
	}

	struct termios tty;
	if(tcgetattr(fd, &tty) != 0) {
		goto fail;
	}
	cfmakeraw(&tty);
	tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8 | CLOCAL | CREAD;
	tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
	tty.c_iflag &= ~(IXON | IXOFF | IXANY);
	tty.c_cc[VMIN] = 0;
	tty.c_cc[VTIME] = 0;
	if(cfsetspeed(&tty, speed) != 0 || tcsetattr(fd, TCSANOW, &tty) != 0
	   || tcflush(fd, TCIOFLUSH) != 0) {
		goto fail;
	}

	return fd;
fail: {
	int saved = errno;
	close(fd);
	errno = saved;
}
	return -1;
}

int roseserial_main(int argc, char **argv) {
	if(argc > 1 && is_help_arg(argv[1])) {
		print_usage(argv[0]);
		return 0;
	}
	if(argc < 7) {
		print_usage(argv[0]);
		return 1;
	}

	const char *device = argv[1];
	const char *rx_topic = argv[2];
	const char *tx_topic = argv[3];
	int baud = 0;
	int msg_size = 0;
	int slots = 0;

	if(parse_int_arg(argv[4], &baud) != 0
	   || parse_int_arg(argv[5], &msg_size) != 0
	   || parse_int_arg(argv[6], &slots) != 0) {
		print_usage(argv[0]);
		return 1;
	}

	if(msg_size <= 0 || msg_size > UINT16_MAX || !liaison_sizes_valid(msg_size, slots)) {
		fprintf(stderr, "roseserial: invalid size/slots (size 1..65535, ring budget 64MiB)\n");
		return 1;
	}
	int status = 1, fd = -1;
	rose_node *node = NULL;
	mirage_msg *tx_msg = NULL;
	uint8_t *wire = NULL;
	struct serial_parser *parser = NULL;
	fd = serial_open(device, baud);
	if(fd < 0) {
		perror("roseserial: serial setup");
		goto cleanup;
	}
	node = rose_init(argc, argv, "roseserial", NULL, NULL);
	if(!node) {
		goto cleanup;
	}
	liaison_signals();
	rose_publisher *pub = rose_create_pub(node, (char *) rx_topic, -1, msg_size, slots);
	if(!pub) {
		fprintf(stderr, "roseserial: publisher creation failed\n");
		goto cleanup;
	}
	rose_subscriber *sub = NULL;
	wire = malloc(2 * ((size_t) msg_size + 7) + 2);
	parser = calloc(1, sizeof(*parser));
	if(!wire || !parser) {
		fprintf(stderr, "roseserial: allocation failed\n");
		goto cleanup;
	}
	size_t tx_len = 0, tx_off = 0;
	u64 tx_start = 0, retry = 0, received = 0, sent = 0, dropped = 0;
	status = 0;
	while(liaison_running(node)) {
		if(!sub && (!retry || rose_elapsed_seconds(retry) >= 1.0)) {
			sub = rose_create_sub(node, (char *) tx_topic, -1, 0, NULL);
			retry = rose_now();
			if(sub && !(tx_msg = liaison_source_buffer(sub))) {
				status = 1;
				break;
			}
		}
		if(sub && tx_off == tx_len) {
			i64 r = rose_read(sub, tx_msg);
			if(r >= 0) {
				if(tx_msg->length <= 0 || tx_msg->length > msg_size || tx_msg->length > UINT16_MAX) {
					++dropped;
					fprintf(stderr, "roseserial: dropped oversized source sample (%llu bytes)\n", (unsigned long long) tx_msg->length);
				} else {
					tx_len = serial_encode(wire, tx_msg->data, tx_msg->length);
					tx_off = 0;
					tx_start = rose_now();
				}
			} else if(r < -1) {
				fprintf(stderr, "roseserial: source read failed (%lld)\n", (long long) r);
				status = 1;
				break;
			}
		}
		if((parser->used || parser->escaped) && !parser->dropping && !liaison_remaining(parser->start)) {
			parser->dropping = 1; /* Wait for an explicit delimiter before resuming. */
		}
		if(tx_off < tx_len && !liaison_remaining(tx_start)) {
			fprintf(stderr, "roseserial: transmit frame timed out\n");
			status = 1;
			break;
		}
		struct pollfd pfd = {.fd = fd, .events = POLLIN | (tx_off < tx_len ? POLLOUT : 0)};
		int pr = poll(&pfd, 1, tx_off < tx_len ? 0 : 1);
		if(pr < 0) {
			if(errno == EINTR) {
				continue;
			}
			perror("roseserial: poll");
			status = 1;
			break;
		}
		if(pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "roseserial: device disconnected\n");
			status = 1;
			break;
		}
		if(pfd.revents & POLLIN) {
			uint8_t chunk[4096];
			ssize_t n = read(fd, chunk, sizeof(chunk));
			if(n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
				perror("roseserial: read");
				status = 1;
				break;
			}
			/* VMIN=0 permits a zero-byte read: preserve parser state. */
			for(ssize_t i = 0; i < n; ++i) {
				int r = serial_feed(parser, chunk[i], msg_size);
				if(r < 0) {
					++dropped;
					continue;
				}
				if(r == 1) {
					size_t len = parser->raw[1] | ((size_t) parser->raw[2] << 8);
					mirage_msg msg = {.data = parser->raw + 3, .max_length = len, .length = len, .cursor = len};
					if(rose_publish(pub, &msg) < 0) {
						fprintf(stderr, "roseserial: publish failed\n");
						status = 1;
						break;
					}
					++received;
				}
			}
			if(status) {
				break;
			}
		}
		if((pfd.revents & POLLOUT) && tx_off < tx_len) {
			ssize_t n = write(fd, wire + tx_off, tx_len - tx_off);
			if(n > 0) {
				tx_off += (size_t) n;
				if(tx_off == tx_len) {
					++sent;
				}
			} else if(n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
				perror("roseserial: write");
				status = 1;
				break;
			}
		}
	}
	fprintf(stderr, "roseserial: received=%llu sent=%llu dropped=%llu\n", (unsigned long long) received, (unsigned long long) sent, (unsigned long long) dropped);
cleanup:
	if(liaison_stop) {
		status = 0;
	}
	free(parser);
	free(wire);
	if(tx_msg) {
		mirage_destroy(&tx_msg, NULL);
	}
	if(node) {
		rose_shutdown(node);
	}
	if(fd >= 0) {
		close(fd);
	}
	return status;
}
