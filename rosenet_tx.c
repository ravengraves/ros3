// rosenet_tx.c - Network transmitter: subscribes to local topic, sends over network
//
// Usage: rosenet_tx <topic> <host> <port> [--udp]

#include "ros3.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
	if(argc < 4) {
		fprintf(stderr, "Usage: %s <topic> <host> <port> [--udp]\n", argv[0]);
		return 1;
	}

	const char *topic = argv[1];
	const char *host = argv[2];
	int port = atoi(argv[3]);
	int use_udp = (argc > 4 && strcmp(argv[4], "--udp") == 0);

	// Init as rose node
	rose_node *node = rose_init(argc, argv, "rosenet_tx", NULL, NULL);

	// Subscribe to source topic
	rose_subscriber *sub = rose_create_sub(node, (char *) topic, -1, 0, NULL);
	if(!sub) {
		fprintf(stderr, "Failed to subscribe to %s\n", topic);
		return 1;
	}

	// Setup network socket
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
		return 1;
	}

	if(!use_udp) {
		rose_info("Connecting to %s:%d (TCP)...", host, port);
		while(rose_ok(node)) {
			if(connect(sock, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
				break;
			}
			rose_sleep(0.1);
			// Recreate socket after failed connect
			close(sock);
			sock = socket(AF_INET, SOCK_STREAM, 0);
		}
		if(!rose_ok(node)) {
			return 1;
		}
		rose_info("Connected");
	} else {
		rose_info("Sending to %s:%d (UDP)", host, port);
	}

	// Main loop
	mirage_msg *msg = mirage_create(65536, NULL);
	u64 count = 0;

	while(rose_ok(node)) {
		msg->length = msg->max_length;
		if(rose_read(sub, msg) >= 0) {
			if(use_udp) {
				sendto(sock, msg->data, msg->length, 0,
				       (struct sockaddr *) &addr, sizeof(addr));
			} else {
				uint32_t len = htonl((uint32_t) msg->length);
				send(sock, &len, 4, 0);
				send(sock, msg->data, msg->length, 0);
			}
			if(++count % 1000 == 0) {
				rose_info("Sent %lu messages", (unsigned long) count);
			}
		}
		rose_sleep(1e-3);
	}

	close(sock);
	return 0;
}
