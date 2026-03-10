// roseserial.c - Bidirectional serial bridge for microcontrollers
//
// Usage: roseserial <device> <rx_topic> <tx_topic> <baud> <msg_size> <slots>
// Framing: [2-byte LE length][payload]

#define _GNU_SOURCE

#include "core.h"
#include "ros3.h"

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
	tcgetattr(fd, &tty);
	cfmakeraw(&tty);
	tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8 | CLOCAL | CREAD;
	tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
	tty.c_iflag &= ~(IXON | IXOFF | IXANY);
	tty.c_cc[VMIN] = 0;
	tty.c_cc[VTIME] = 0;
	cfsetspeed(&tty, speed);
	tcsetattr(fd, TCSANOW, &tty);
	tcflush(fd, TCIOFLUSH);

	return fd;
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

	int fd = serial_open(device, baud);
	if(fd < 0) {
		fprintf(stderr, "Failed to open %s @ %d\n", device, baud);
		return 1;
	}

	// Init as rose node
	rose_node *node = rose_init(argc, argv, "roseserial", NULL, NULL);

	// RX: serial -> topic (we publish)
	rose_publisher *rx_pub = rose_create_pub(node, (char *) rx_topic, -1, msg_size, slots);
	// TX: topic -> serial (we subscribe)
	rose_subscriber *tx_sub = rose_create_sub(node, (char *) tx_topic, -1, 0, NULL);

	if(!rx_pub || !tx_sub) {
		fprintf(stderr, "Failed to create topics\n");
		if(close(fd) != 0) {}
		return 1;
	}

	rose_info("%s @ %d | rx:%s tx:%s", device, baud, rx_topic, tx_topic);

	mirage_msg *msg = mirage_create(msg_size, NULL);
	uint8_t hdr[2];

	while(rose_ok(node)) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		poll(&pfd, 1, 10);

		// Serial -> RX topic
		if(pfd.revents & POLLIN) {
			if(read(fd, hdr, 2) == 2) {
				uint16_t len = hdr[0] | (hdr[1] << 8);
				if(len <= msg_size) {
					ssize_t n = read(fd, msg->data, len);
					if(n == len) {
						msg->cursor = len;
						rose_publish(rx_pub, msg);
					}
				}
			}
		}

		// TX topic -> Serial
		msg->length = msg->max_length;
		if(rose_read(tx_sub, msg) >= 0) {
			hdr[0] = msg->length & 0xFF;
			hdr[1] = (msg->length >> 8) & 0xFF;
			if(write(fd, hdr, 2) != 2) {
				break;
			}
			if(write(fd, msg->data, msg->length) != msg->length) {
				break;
			}
		}
	}

	if(close(fd) != 0) {}
	return 0;
}
