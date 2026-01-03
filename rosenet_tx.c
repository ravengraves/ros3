// rose_liaison_out.c - Broadcaster: reads from local ring buffer, sends to remote host
//
// Usage: rose_liaison_out <topic> <host> <port> [--udp]

#define _POSIX_C_SOURCE 200809L

#include "ros3.h"

#include <arpa/inet.h>
#include <netdb.h>
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
	fprintf(stderr, "Usage: %s <topic> <host> <port> [--udp]\n", prog);
	fprintf(stderr, "\n");
	fprintf(stderr, "Reads messages from a local shared memory topic and sends\n");
	fprintf(stderr, "them to a remote host over TCP (default) or UDP.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  --udp  Use UDP instead of TCP\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Environment:\n");
	fprintf(stderr, "  ROSE_BASE  Namespace (default: 'rose')\n");
}

int main(int argc, char **argv) {
	if(argc < 4) {
		print_usage(argv[0]);
		return 1;
	}

	const char *topic = argv[1];
	const char *host = argv[2];
	int port = atoi(argv[3]);
	int use_udp = (argc > 4 && strcmp(argv[4], "--udp") == 0);

	// Build shm name with ROSE_BASE prefix
	char shm_name[512];
	rose_topic_to_shm(shm_name, sizeof(shm_name), topic);

	// Connect to local ring buffer as subscriber
	quicksand_connection *sub = NULL;
	if(quicksand_connect(&sub, shm_name, -1, 0, 0, NULL) != 0) {
		fprintf(stderr, "Failed to connect to topic %s (shm: %s)\n", topic, shm_name);
		return 1;
	}

	printf("Connected to local topic: %s\n", topic);

	// Create socket
	int sock = socket(AF_INET, use_udp ? SOCK_DGRAM : SOCK_STREAM, 0);
	if(sock < 0) {
		perror("socket");
		return 1;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t) port);

	if(inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
		fprintf(stderr, "Invalid address: %s\n", host);
		close(sock);
		return 1;
	}

	if(!use_udp) {
		printf("Connecting to %s:%d (TCP)...\n", host, port);
		if(connect(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
			perror("connect");
			close(sock);
			return 1;
		}
		printf("Connected\n");
	} else {
		printf("Sending to %s:%d (UDP)\n", host, port);
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	uint8_t buf[65536];
	u64 msg_count = 0;

	while(running) {
		i64 sz = sizeof(buf);
		if(quicksand_read(sub, buf, &sz) >= 0) {
			ssize_t sent;
			if(use_udp) {
				sent = sendto(sock, buf, (size_t) sz, 0,
					      (struct sockaddr *) &addr, sizeof(addr));
			} else {
				// TCP: send 4-byte length prefix + data
				uint32_t len = htonl((uint32_t) sz);
				ssize_t hdr_sent = send(sock, &len, 4, 0);
				if(hdr_sent != 4) {
					fprintf(stderr, "Failed to send header\n");
					break;
				}
				sent = send(sock, buf, (size_t) sz, 0);
			}

			if(sent < 0) {
				perror("send");
				break;
			}

			msg_count++;
			if(msg_count % 1000 == 0) {
				printf("Sent %lu messages\n", (unsigned long) msg_count);
			}
		} else {
			quicksand_sleep(1e6); // 1ms
		}
	}

	printf("\nStopped. Total messages sent: %lu\n", (unsigned long) msg_count);
	close(sock);
	return 0;
}
