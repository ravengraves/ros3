/* Actual Quicksand transport; the Python harness checks discovery while alive. */
#define _POSIX_C_SOURCE 200809L
#include "ros3.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check_registration_envelope(void) {
	// Smallest accepted base is one byte: /b% + identity + %control <=255.
	char name[245], topic[256];
	memset(name, 'n', 244);
	name[244] = '\0';
	memset(topic, 't', 255);
	topic[255] = '\0';
	mirage_msg *message = mirage_create(ROSE_REGISTRATION_MSG_SIZE, NULL);
	assert(message);
	assert(mirage_write_start(message) >= 0);
	assert(mirage_write_fn(message, ROSE_MSG_PUB_REGISTER, -1, 2) >= 0);
	assert(mirage_write_string(message, name, -1) >= 0);
	assert(mirage_write_string(message, topic, -1) >= 0);
	assert(message->length == 522 && message->length <= ROSE_REGISTRATION_MSG_SIZE);
	mirage_destroy(&message, NULL);
}

int main(void) {
	check_registration_envelope();
	char *argv[130] = {"fixture", "__ns:=one", "source:=early", "/mapped:=/recursive"};
	for(int i = 4; i < 129; ++i) {
		argv[i] = "ordinary";
	}
	argv[4] = "relative:=relative_target";
	argv[5] = "relative_target:=chained";
	argv[129] = "source:=/mapped";
	rose_node *one = rose_init(130, argv, "same", NULL, NULL);
	// Direct construction accepts packed, NUL-separated pairs and takes ownership
	// only on success. Exercise subscriber remapping, including last-match wins.
	const char subscriber_rules[] = "input\0early\0/two/input\0/mapped\0/mapped\0/recursive\0";
	char *rules = malloc(sizeof(subscriber_rules));
	u64 *lengths = malloc(6 * sizeof(*lengths));
	assert(rules && lengths);
	memcpy(rules, subscriber_rules, sizeof(subscriber_rules));
	const u64 rule_lengths[] = {5, 5, 10, 7, 7, 10};
	memcpy(lengths, rule_lengths, sizeof(rule_lengths));
	rose_node *two = rose_create_node("same", -1, "two", -1, rules, lengths, 3, NULL, NULL);
	assert(one && two && strcmp(one->name, two->name));
	// Exercise the exact private-channel and public-topic transport boundaries.
	char long_name[256], long_topic[256];
	int pid_length = snprintf(long_name, sizeof(long_name), "-%ld", (long) getpid());
	size_t base_prefix_length = (size_t) (strchr(one->prefix, '%') - one->prefix) + 1;
	size_t name_length = 255 - base_prefix_length - (size_t) pid_length - strlen("%control");
	memset(long_name, 'n', name_length);
	long_name[name_length] = '\0';
	rose_node *long_node = rose_create_node(long_name, -1, NULL, -1, NULL, NULL, 0, NULL, NULL);
	assert(long_node && strlen((char *) long_node->control_sub.sub->name) == 255);
	size_t topic_length = 255 - long_node->prefix_length;
	memset(long_topic, 't', topic_length);
	long_topic[topic_length] = '\0';
	assert(setenv("ROSE_BASE", "invalid/base", 1) == 0);
	// These spans deliberately have no NUL terminator.
	char source[] = {'s', 'o', 'u', 'r', 'c', 'e'};
	char input[] = {'i', 'n', 'p', 'u', 't'};
	rose_publisher *pub = rose_create_pub(one, source, sizeof(source), 128, 16);
	rose_subscriber *sub = rose_create_sub(two, input, sizeof(input), 0, NULL);
	rose_subscriber *absolute_sub = rose_create_sub(two, "/two/input", -1, 0, NULL);
	rose_publisher *long_pub = rose_create_pub(long_node, long_topic, -1, 128, 16);
	assert(pub && sub && absolute_sub && long_pub);
	assert(strcmp((char *) pub->pub->name, (char *) sub->sub->name) == 0);
	assert(strcmp((char *) pub->pub->name, (char *) absolute_sub->sub->name) == 0);
	rose_publisher *relative_pub = rose_create_pub(one, "/one/relative", -1, 128, 16);
	rose_subscriber *relative_sub = rose_create_sub(two, "/one/relative_target", -1, 0, NULL);
	assert(relative_pub && relative_sub);
	assert(strcmp((char *) relative_pub->pub->name, (char *) relative_sub->sub->name) == 0);
	// Relative destinations stay in their node namespace. Identical unmapped
	// topic text in another namespace must attach to a different ring.
	rose_publisher *isolated_one = rose_create_pub(one, "isolated", -1, 128, 16);
	rose_publisher *isolated_two = rose_create_pub(two, "isolated", -1, 128, 16);
	rose_subscriber *isolated_sub = rose_create_sub(two, "isolated", -1, 0, NULL);
	assert(isolated_one && isolated_two && isolated_sub);
	assert(strcmp((char *) isolated_one->pub->name, (char *) isolated_two->pub->name));
	assert(strcmp((char *) isolated_two->pub->name, (char *) isolated_sub->sub->name) == 0);
	assert(strlen((char *) long_pub->pub->name) == 255);
	mirage_msg *sent = mirage_create(128, NULL), *received = mirage_create(128, NULL);
	assert(sent && received);
	assert(mirage_write_start(sent) >= 0 && mirage_write_string(sent, "delivered", -1) >= 0);
	assert(rose_publish(pub, sent) == ROSE_OK);
	assert(rose_read(sub, received) == ROSE_OK);
	assert(sent->length == received->length && !memcmp(sent->data, received->data, sent->length));
	assert(rose_read(absolute_sub, received) == ROSE_OK);
	assert(sent->length == received->length && !memcmp(sent->data, received->data, sent->length));
	assert(rose_publish(relative_pub, sent) == ROSE_OK);
	assert(rose_read(relative_sub, received) == ROSE_OK);
	assert(sent->length == received->length && !memcmp(sent->data, received->data, sent->length));
	assert(rose_publish(isolated_one, sent) == ROSE_OK);
	assert(rose_read(isolated_sub, received) != ROSE_OK);
	assert(rose_publish(isolated_two, sent) == ROSE_OK);
	assert(rose_read(isolated_sub, received) == ROSE_OK);
	assert(sent->length == received->length && !memcmp(sent->data, received->data, sent->length));
	printf("READY\n%s\n%s\n%s\n/%s\n", one->name, two->name, long_node->name, long_topic);
	assert(long_node->registration_scratch->length <= ROSE_REGISTRATION_MSG_SIZE);
	printf("Registration: %llu bytes; declared capacity %d; physical payload capacity %llu\n",
	       (unsigned long long) long_node->registration_scratch->length,
	       ROSE_REGISTRATION_MSG_SIZE,
	       (unsigned long long) (long_node->rosecore_pub.pub->buffer->message_size - 20));
	fflush(stdout);
	(void) getchar();
	mirage_destroy(&sent, NULL);
	mirage_destroy(&received, NULL);
	rose_shutdown(one);
	rose_shutdown(two);
	rose_shutdown(long_node);
	return 0;
}
