// roseserial.c - Bidirectional serial bridge for microcontrollers
//
// Usage: roseserial <device> <rx_topic> <tx_topic> <baud> <msg_size> <ring_slots>
//
// Framing: [2-byte LE length][payload] (max 65535 bytes)

#define _GNU_SOURCE

#include "ros3.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static volatile int running = 1;

static void handle_signal(int sig) {
	(void) sig;
	running = 0;
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
	case 500000: return B500000;
	case 576000: return B576000;
	case 921600: return B921600;
	case 1000000: return B1000000;
	case 1152000: return B1152000;
	case 1500000: return B1500000;
	case 2000000: return B2000000;
	case 2500000: return B2500000;
	case 3000000: return B3000000;
	case 3500000: return B3500000;
	case 4000000: return B4000000;
	default: return B0;
	}
}

static int serial_open(const char *device, int baud) {
	speed_t speed = baud_to_speed(baud);
	if(speed == B0) {
		fprintf(stderr, "Unsupported baud: %d\n", baud);
		return -1;
	}

	int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if(fd < 0) {
		perror("open");
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

static ssize_t read_exact(int fd, void *buf, size_t n) {
	uint8_t *p = buf;
	size_t total = 0;

	while(total < n && running) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		if(poll(&pfd, 1, 100) <= 0) {
			continue;
		}

		ssize_t r = read(fd, p + total, n - total);
		if(r > 0) {
			total += (size_t) r;
		} else if(r < 0 && errno != EAGAIN) {
			return -1;
		}
	}
	return running ? (ssize_t) total : -1;
}

static ssize_t write_all(int fd, const void *buf, size_t n) {
	const uint8_t *p = buf;
	size_t total = 0;

	while(total < n) {
		ssize_t w = write(fd, p + total, n - total);
		if(w > 0) {
			total += (size_t) w;
		} else if(w < 0 && errno != EAGAIN) {
			return -1;
		} else {
			quicksand_sleep(100e3);
		}
	}
	return (ssize_t) total;
}

static void print_usage(const char *prog) {
	fprintf(stderr, "Usage: %s <device> <rx_topic> <tx_topic> <baud> <msg_size> <slots>\n", prog);
	fprintf(stderr, "\n");
	fprintf(stderr, "Bidirectional serial bridge for microcontrollers.\n");
	fprintf(stderr, "Creates ring buffers for both rx and tx topics.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Arguments:\n");
	fprintf(stderr, "  device      Serial device (e.g., /dev/ttyUSB0)\n");
	fprintf(stderr, "  rx_topic    Topic for data FROM serial\n");
	fprintf(stderr, "  tx_topic    Topic for data TO serial\n");
	fprintf(stderr, "  baud        Baud rate\n");
	fprintf(stderr, "  msg_size    Maximum message size (1-65535)\n");
	fprintf(stderr, "  slots       Ring buffer slots\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Environment:\n");
	fprintf(stderr, "  ROSE_BASE  Namespace (default: 'rose')\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Framing: [2-byte LE length][payload]\n");
}

int main(int argc, char **argv) {
	if(argc < 7) {
		print_usage(argv[0]);
		return 1;
	}

	const char *device = argv[1];
	const char *rx_topic = argv[2];
	const char *tx_topic = argv[3];
	int baud = atoi(argv[4]);
	int msg_size = atoi(argv[5]);
	int slots = atoi(argv[6]);

	if(msg_size <= 0 || msg_size > 65535 || slots <= 0) {
		fprintf(stderr, "Invalid msg_size (1-65535) or slots\n");
		return 1;
	}

	int fd = serial_open(device, baud);
	if(fd < 0) {
		return 1;
	}

	char shm[512];

	// Create RX ring (data FROM serial)
	rose_topic_to_shm(shm, sizeof(shm), rx_topic);
	quicksand_connection *rx_ring = NULL;
	if(quicksand_connect(&rx_ring, shm, -1, msg_size, slots, NULL) != 0) {
		fprintf(stderr, "Failed to create %s\n", rx_topic);
		return 1;
	}

	// Create TX ring (data TO serial)
	rose_topic_to_shm(shm, sizeof(shm), tx_topic);
	quicksand_connection *tx_ring = NULL;
	if(quicksand_connect(&tx_ring, shm, -1, msg_size, slots, NULL) != 0) {
		fprintf(stderr, "Failed to create %s\n", tx_topic);
		return 1;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	uint8_t *buf = malloc((size_t) msg_size);
	u64 rx_cnt = 0, tx_cnt = 0;

	printf("%s @ %d | rx:%s tx:%s\n", device, baud, rx_topic, tx_topic);

	while(running) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		poll(&pfd, 1, 10);

		// Serial -> RX ring
		if(pfd.revents & POLLIN) {
			uint16_t len;
			if(read_exact(fd, &len, 2) == 2 && len <= msg_size) {
				if(read_exact(fd, buf, len) == len) {
					quicksand_write(rx_ring, buf, len);
					rx_cnt++;
				}
			}
		}

		// TX ring -> Serial
		i64 sz = msg_size;
		if(quicksand_read(tx_ring, buf, &sz) >= 0 && sz > 0) {
			uint16_t len = (uint16_t) sz;
			write_all(fd, &len, 2);
			write_all(fd, buf, (size_t) sz);
			tx_cnt++;
		}
	}

	printf("\nRX: %lu TX: %lu\n", (unsigned long) rx_cnt, (unsigned long) tx_cnt);
	free(buf);
	close(fd);
	return 0;
}
