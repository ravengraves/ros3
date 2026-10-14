#ifndef ROSE_LIAISON_H
#define ROSE_LIAISON_H

#include "ros3.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#define LIAISON_MAX_MESSAGE (16U * 1024U * 1024U)
#define LIAISON_RING_BUDGET (64U * 1024U * 1024U)
#define LIAISON_UDP_MAX 65507U
#define LIAISON_TIMEOUT_MS 1000

static volatile sig_atomic_t liaison_stop;
static inline void liaison_signal(int sig) {
	(void) sig;
	liaison_stop = 1;
}
static inline void liaison_signals(void) {
	liaison_stop = 0;
	signal(SIGINT, liaison_signal);
	signal(SIGTERM, liaison_signal);
}
static inline int liaison_running(rose_node *node) {
	return !liaison_stop && rose_ok(node);
}

/* Quicksand rounds the slot (20-byte preamble/CRC included) and ring count
 * to powers/alignment boundaries. Bound that actual allocation, not size*rate. */
static inline int liaison_sizes_valid(uint64_t size, uint64_t slots) {
	if(size == 0 || size > LIAISON_MAX_MESSAGE || slots == 0 || slots > 1048576) {
		return 0;
	}
	uint64_t stride = (size + 20 + 63) & ~UINT64_C(63);
	uint64_t count = 2;
	while(count < slots) {
		count *= 2;
	}
	return stride * count + ((sizeof(quicksand_ringbuffer) + 63U) & ~UINT64_C(63)) <= LIAISON_RING_BUDGET;
}

/* Read the full validated source slot so an over-limit sample is consumed and
 * can be explicitly dropped. Never retry a too-small destination forever. */
static inline mirage_msg *liaison_source_buffer(rose_subscriber *sub) {
	uint64_t size = sub->sub->buffer->message_size;
	if(size < 64 || size > LIAISON_MAX_MESSAGE + 64U) {
		fprintf(stderr, "liaison: source slot capacity is outside supported bounds\n");
		return NULL;
	}
	return mirage_create(size, NULL);
}

static inline int liaison_remaining(u64 start) {
	double elapsed = rose_nanoseconds(rose_now(), start) / 1000000.0;
	return elapsed >= LIAISON_TIMEOUT_MS ? 0 : LIAISON_TIMEOUT_MS - (int) elapsed;
}

/* One absolute operation deadline, including progress and EINTR. */
static inline int liaison_wait(int fd, short events, u64 start) {
	for(;;) {
		int ms = liaison_remaining(start);
		if(!ms) {
			errno = ETIMEDOUT;
			return -1;
		}
		struct pollfd pfd = {.fd = fd, .events = events};
		int r = poll(&pfd, 1, ms);
		if(r < 0 && errno == EINTR) {
			continue;
		}
		if(r <= 0) {
			if(!r) {
				errno = ETIMEDOUT;
			}
			return -1;
		}
		if(pfd.revents & events) {
			return 0;
		}
		errno = EIO;
		return -1;
	}
}

static inline int liaison_socket_io(int fd, void *data, size_t len, int sending, u64 start) {
	size_t off = 0;
	while(off < len) {
		if(!liaison_remaining(start)) {
			errno = ETIMEDOUT;
			return -1;
		}
		ssize_t n = sending ? send(fd, (uint8_t *) data + off, len - off, MSG_DONTWAIT)
				    : recv(fd, (uint8_t *) data + off, len - off, MSG_DONTWAIT);
		if(n > 0) {
			off += (size_t) n;
			continue;
		}
		if(n < 0 && errno == EINTR) {
			continue;
		}
		if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if(liaison_wait(fd, sending ? POLLOUT : POLLIN, start) == 0) {
				continue;
			}
		} else if(n == 0) {
			errno = ECONNRESET;
		}
		return -1;
	}
	return 0;
}
#endif
