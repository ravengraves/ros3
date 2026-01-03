// rosenet_rx.c - Network receiver: listens on port, publishes to local topic
//
// Usage: rosenet_rx <topic> <port> <msg_size> <ring_slots> [--udp]

#include "ros3.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Read exactly n bytes from socket
static ssize_t recv_exact(int sock, void *buf, size_t n) {
	uint8_t *p = buf;
	size_t total = 0;
	while(total < n) {
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
		fprintf(stderr, "Usage: %s <topic> <port> <msg_size> <slots> [--udp]\n", argv[0]);
		return 1;
	}

	const char *topic = argv[1];
	int port = atoi(argv[2]);
	int msg_size = atoi(argv[3]);
	int slots = atoi(argv[4]);
	int use_udp = (argc > 5 && strcmp(argv[5], "--udp") == 0);

	// Init as rose node
	rose_node *node = rose_init(argc, argv, "rosenet_rx", NULL, NULL);

	// Create publisher for destination topic
	rose_publisher *pub = rose_create_pub(node, (char *) topic, -1, msg_size, slots);
	if(!pub) {
		fprintf(stderr, "Failed to create topic %s\n", topic);
		return 1;
	}

	// Setup network socket
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
		return 1;
	}

	int client = sock;
	if(!use_udp) {
		listen(sock, 1);
		rose_info("Waiting for TCP on port %d...", port);
		client = accept(sock, NULL, NULL);
		if(client < 0) {
			perror("accept");
			return 1;
		}
		rose_info("Connected");
	} else {
		rose_info("Listening for UDP on port %d", port);
	}

	// Main loop
	mirage_msg *msg = mirage_create(msg_size, NULL);
	u64 count = 0;

	while(rose_ok(node)) {
		ssize_t n;
		if(use_udp) {
			n = recvfrom(client, msg->data, msg_size, 0, NULL, NULL);
		} else {
			uint32_t len;
			if(recv_exact(client, &len, 4) != 4) {
				break;
			}
			len = ntohl(len);
			if(len > (uint32_t) msg_size) {
				continue;
			}
			n = recv_exact(client, msg->data, len);
			if(n != (ssize_t) len) {
				break;
			}
		}

		if(n > 0) {
			msg->cursor = n;
			rose_publish(pub, msg);
			if(++count % 1000 == 0) {
				rose_info("Received %lu messages", (unsigned long) count);
			}
		}
	}

	close(client);
	if(!use_udp) {
		close(sock);
	}
	return 0;
}
