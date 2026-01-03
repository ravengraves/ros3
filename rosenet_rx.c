// rose_liaison_in.c - Listener: receives from network, writes to local ring buffer
//
// Usage: rose_liaison_in <topic> <port> <msg_size> <ring_slots> [--udp]

#define _POSIX_C_SOURCE 200809L

#include "ros3.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile int running = 1;

static void handle_signal(int sig) {
	(void) sig;
	running = 0;
}

static void print_usage(const char *prog) {
	fprintf(stderr, "Usage: %s <topic> <port> <msg_size> <ring_slots> [--udp]\n", prog);
	fprintf(stderr, "\n");
	fprintf(stderr, "Receives messages from network and writes them to a local\n");
	fprintf(stderr, "shared memory topic. Creates the ring buffer.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Arguments:\n");
	fprintf(stderr, "  topic       Topic name (e.g., /sensor/data)\n");
	fprintf(stderr, "  port        Port to listen on\n");
	fprintf(stderr, "  msg_size    Maximum message size in bytes\n");
	fprintf(stderr, "  ring_slots  Number of slots in ring buffer\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  --udp  Use UDP instead of TCP\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Environment:\n");
	fprintf(stderr, "  ROSE_BASE  Namespace (default: 'rose')\n");
}

// Read exactly n bytes from socket (TCP)
static ssize_t recv_exact(int sock, void *buf, size_t n) {
	size_t total = 0;
	uint8_t *p = buf;

	while(total < n && running) {
		ssize_t r = recv(sock, p + total, n - total, 0);
		if(r <= 0) {
			return r;
		}
		total += (size_t) r;
	}

	return (ssize_t) total;
}

int main(int argc, char **argv) {
	if(argc < 5) {
		print_usage(argv[0]);
		return 1;
	}

	const char *topic = argv[1];
	int port = atoi(argv[2]);
	int msg_size = atoi(argv[3]);
	int ring_slots = atoi(argv[4]);
	int use_udp = (argc > 5 && strcmp(argv[5], "--udp") == 0);

	if(msg_size <= 0 || ring_slots <= 0) {
		fprintf(stderr, "msg_size and ring_slots must be positive\n");
		return 1;
	}

	// Build shm name with ROSE_BASE prefix
	char shm_name[512];
	rose_topic_to_shm(shm_name, sizeof(shm_name), topic);

	// Create local ring buffer as publisher
	quicksand_connection *pub = NULL;
	if(quicksand_connect(&pub, shm_name, -1, msg_size, ring_slots, NULL) != 0) {
		fprintf(stderr, "Failed to create topic %s (shm: %s)\n", topic, shm_name);
		return 1;
	}

	printf("Created local topic: %s (msg_size=%d, slots=%d)\n",
	       topic, msg_size, ring_slots);

	// Create and bind socket
	int sock = socket(AF_INET, use_udp ? SOCK_DGRAM : SOCK_STREAM, 0);
	if(sock < 0) {
		perror("socket");
		return 1;
	}

	int opt = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t) port);
	addr.sin_addr.s_addr = INADDR_ANY;

	if(bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("bind");
		close(sock);
		return 1;
	}

	int client_sock = sock;
	if(!use_udp) {
		if(listen(sock, 1) < 0) {
			perror("listen");
			close(sock);
			return 1;
		}
		printf("Waiting for TCP connection on port %d...\n", port);
		client_sock = accept(sock, NULL, NULL);
		if(client_sock < 0) {
			perror("accept");
			close(sock);
			return 1;
		}
		printf("Connected\n");
	} else {
		printf("Listening for UDP on port %d\n", port);
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	uint8_t *buf = malloc((size_t) msg_size);
	if(!buf) {
		fprintf(stderr, "Failed to allocate receive buffer\n");
		close(client_sock);
		if(!use_udp) {
			close(sock);
		}
		return 1;
	}

	u64 msg_count = 0;

	while(running) {
		ssize_t n;

		if(use_udp) {
			n = recvfrom(client_sock, buf, (size_t) msg_size, 0, NULL, NULL);
		} else {
			// TCP: read 4-byte length prefix, then data
			uint32_t len;
			ssize_t hdr_recv = recv_exact(client_sock, &len, 4);
			if(hdr_recv != 4) {
				if(hdr_recv == 0) {
					printf("Connection closed by remote\n");
				} else if(running) {
					perror("recv header");
				}
				break;
			}

			len = ntohl(len);
			if(len > (uint32_t) msg_size) {
				fprintf(stderr, "Message too large: %u > %d\n", len, msg_size);
				continue;
			}

			n = recv_exact(client_sock, buf, len);
			if(n != (ssize_t) len) {
				if(n == 0) {
					printf("Connection closed by remote\n");
				} else if(running) {
					perror("recv data");
				}
				break;
			}
		}

		if(n > 0) {
			quicksand_write(pub, buf, n);
			msg_count++;
			if(msg_count % 1000 == 0) {
				printf("Received %lu messages\n", (unsigned long) msg_count);
			}
		} else if(n < 0 && running) {
			perror("recv");
			break;
		}
	}

	printf("\nStopped. Total messages received: %lu\n", (unsigned long) msg_count);

	free(buf);
	close(client_sock);
	if(!use_udp) {
		close(sock);
	}
	return 0;
}
