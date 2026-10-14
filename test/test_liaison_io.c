#define _GNU_SOURCE
#include "../cmd/roseserial/core.c"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>

static void nothing(int sig) { (void) sig; }
struct writer {
	int fd;
	pthread_t target;
};
static void *fragmented(void *arg) {
	struct writer *w = arg;
	for(unsigned char c = 0; c < 32; ++c) {
		usleep(1000);
		pthread_kill(w->target, SIGUSR1);
		assert(write(w->fd, &c, 1) == 1);
	}
	return NULL;
}
int main(void) {
	assert(!liaison_sizes_valid(UINT64_MAX, 4));
	assert(!liaison_sizes_valid(1, UINT64_MAX));
	assert(!liaison_sizes_valid(1048576, 512));
	assert(liaison_sizes_valid(1024, 64));
	assert(serial_crc((const uint8_t *) "123456789", 9) == 0xcbf43926);
	uint8_t *data = malloc(65536), *wire = malloc(2 * SERIAL_RAW_MAX + 2);
	struct serial_parser *p = calloc(1, sizeof(*p));
	assert(data && wire && p);
	for(size_t i = 0; i < 65536; ++i) {
		data[i] = (uint8_t) i;
	}
	assert(serial_encode(wire, data, 65536) == 0);
	size_t n = serial_encode(wire, data, 65535);
	int complete = 0;
	for(size_t i = 0; i < n; ++i) {
		if(serial_feed(p, wire[i], 65535) == 1) {
			++complete;
		}
	}
	assert(complete == 1 && !memcmp(p->raw + 3, data, 65535));
	n = serial_encode(wire, data, 128);
	/* Mutate every encoded byte independently, then require resynchronization. */
	for(size_t bad = 0; bad < n; ++bad) {
		memset(p, 0, sizeof(*p));
		wire[bad] ^= 1;
		for(size_t i = 0; i < n; ++i) {
			assert(serial_feed(p, wire[i], 128) != 1);
		}
		wire[bad] ^= 1;
		complete = 0;
		for(size_t i = 0; i < n; ++i) {
			if(serial_feed(p, wire[i], 128) == 1) {
				++complete;
			}
		}
		assert(complete == 1);
	}
	memset(p, 0, sizeof(*p));
	for(size_t i = 0; i < n; ++i) {
		assert(serial_feed(p, wire[i], 127) != 1);
	}
	/* PTY no-data read is not EOF when VMIN/VTIME are zero. */
	int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
	assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
	int slave = serial_open(ptsname(master), 115200);
	assert(slave >= 0);
	ssize_t empty = read(slave, data, 1);
	assert(empty == 0 || (empty < 0 && errno == EAGAIN));
	assert(serial_open("/dev/null", 115200) < 0);
	assert(serial_open(ptsname(master), 12345) < 0);
	close(slave);
	close(master);
	int pair[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	struct sigaction sa = {.sa_handler = nothing};
	sigemptyset(&sa.sa_mask);
	assert(sigaction(SIGUSR1, &sa, NULL) == 0);
	struct writer w = {.fd = pair[1], .target = pthread_self()};
	pthread_t thread;
	assert(pthread_create(&thread, NULL, fragmented, &w) == 0);
	assert(liaison_socket_io(pair[0], data, 32, 0, rose_now()) == 0);
	for(int i = 0; i < 32; ++i) {
		assert(data[i] == i);
	}
	assert(pthread_join(thread, NULL) == 0);
	u64 start = rose_now();
	assert(liaison_socket_io(pair[0], data, 1, 0, start) == -1 && errno == ETIMEDOUT);
	assert(rose_elapsed_seconds(start) < 1.5);
	uint8_t *large = calloc(1, LIAISON_MAX_MESSAGE);
	assert(large);
	start = rose_now();
	assert(liaison_socket_io(pair[0], large, LIAISON_MAX_MESSAGE, 1, start) == -1 && errno == ETIMEDOUT);
	assert(rose_elapsed_seconds(start) < 1.5);
	free(large);
	close(pair[0]);
	close(pair[1]);
	free(data);
	free(wire);
	free(p);
	puts("liaison framing, PTY setup, interrupted/partial I/O and deadlines: PASS");
}
