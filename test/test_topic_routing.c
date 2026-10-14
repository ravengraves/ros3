/* Intercept transport allocation to check the actual endpoint names without
 * reserving the large internal rings. Resolver and public constructors run
 * unchanged, including argv parsing, registration and channel construction. */
#define quicksand_connect routing_connect
#define quicksand_disconnect routing_disconnect
#define quicksand_write routing_write
#define quicksand_delete routing_delete
#include "../src/ros3.c"
#include <assert.h>

int64_t routing_connect(quicksand_connection **connection, char *topic,
			int64_t topic_length, int64_t size, int64_t rate, void *alloc) {
	(void) topic_length;
	(void) rate;
	(void) alloc;
	*connection = calloc(1, sizeof(**connection));
	assert(*connection);
	(*connection)->buffer = calloc(1, sizeof(quicksand_ringbuffer));
	assert((*connection)->buffer);
	(*connection)->buffer->length = 16;
	(*connection)->buffer->message_size = size > 0 ? (uint64_t) size : 1024;
	assert(strlen(topic) < sizeof((*connection)->name));
	strcpy((char *) (*connection)->name, topic);
	return 0;
}

void routing_disconnect(quicksand_connection **connection, void *dealloc) {
	(void) dealloc;
	if(!*connection) {
		return;
	}
	free((*connection)->buffer);
	free(*connection);
	*connection = NULL;
}

int64_t routing_write(quicksand_connection *connection, uint8_t *data, int64_t size) {
	(void) connection;
	(void) data;
	(void) size;
	return 0;
}

void routing_delete(char *topic, int64_t length) {
	(void) topic;
	(void) length;
}

static void expect_topic(rose_node *node, char *input, const char *expected) {
	rose_publisher *pub = rose_create_pub(node, input, -1, 64, 16);
	rose_subscriber *sub = rose_create_sub(node, input, -1, 0, NULL);
	assert(pub && sub);
	assert(strcmp((char *) pub->pub->name, expected) == 0);
	assert(strcmp((char *) sub->sub->name, expected) == 0);
}

static void test_configuration(void) {
	char long_source[254];
	memset(long_source, 'x', 250);
	memcpy(long_source + 250, ":=y", 4);
	char *unused[] = {"test", long_source};
	assert(setenv("ROSE_BASE", "review", 1) == 0);
	assert(!rose_init(2, unused, "node", NULL, NULL));
	assert(rose_last_error_code() == ROSE_ERR_BADINPUT);
	char long_destination[246];
	memcpy(long_destination, "cmd:=", 5);
	memset(long_destination + 5, 'x', 240);
	long_destination[245] = '\0';
	char *shadowed[] = {"test", "__ns:=namespace", long_destination, "cmd:=good"};
	assert(!rose_init(4, shadowed, "node", NULL, NULL));
	// Direct construction rejects the same invalid rules without taking ownership.
	char *rules = malloc(260);
	u64 *lengths = malloc(4 * sizeof(*lengths));
	assert(rules && lengths);
	memset(rules, 'x', 250);
	memcpy(rules + 250, "\0y\0", 3);
	lengths[0] = 250;
	lengths[1] = 1;
	assert(!rose_create_node("node", -1, NULL, -1, rules, lengths, 1, NULL, NULL));
	memcpy(rules, "cmd\0", 4);
	memset(rules + 4, 'x', 240);
	memcpy(rules + 244, "\0cmd\0good\0", 10);
	lengths[0] = 3;
	lengths[1] = 240;
	lengths[2] = 3;
	lengths[3] = 4;
	assert(!rose_create_node("node", -1, "namespace", -1, rules, lengths, 2, NULL, NULL));
	free(rules);
	free(lengths);
	char *argv[] = {"test", "__ns:=namespace", "/source:=/target", "local:=/absolute"};
	rose_node *a = rose_init(4, argv, "same", NULL, NULL);
	assert(a);
	assert(setenv("ROSE_BASE", "changed", 1) == 0);
	rose_node *b = rose_init(4, argv, "same", NULL, NULL);
	assert(b);
	assert(setenv("ROSE_BASE", "invalid/base", 1) == 0);
	expect_topic(a, "topic", "/review%namespace%topic");
	expect_topic(a, "/topic", "/review%topic");
	expect_topic(a, "/source", "/review%target");
	expect_topic(a, "local", "/review%absolute");
	expect_topic(b, "/source", "/changed%target");
	expect_topic(b, "topic", "/changed%namespace%topic");
	rose_shutdown(a);
	rose_shutdown(b);
}

static void test_base_boundaries(void) {
	char base[240], output[257];
	char pid_suffix[32];
	int pid_length = snprintf(pid_suffix, sizeof(pid_suffix), "-%lld",
				  (long long) rose_platform_getpid());
	assert(pid_length > 0 && (size_t) pid_length < sizeof(pid_suffix));
	const char *suffixes[] = {"rosecore", "roselog", "param_query", ROSE_TOPIC_REQUEST,
				  ROSE_TOPIC_DUMP, ROSE_TOPIC_LOOKUP, ROSE_TOPIC_HEARTBEAT};
	char *argv[] = {"test"};
	// Canonical length excludes one optional leading and trailing slash.
	for(size_t length = 235; length <= 237; ++length) {
		for(size_t slashes = 0; slashes < 4; ++slashes) {
			size_t offset = slashes & 1;
			base[0] = '/';
			memset(base + offset, 'b', length);
			size_t end = offset + length;
			if(slashes & 2) {
				base[end++] = '/';
			}
			base[end] = '\0';
			assert(setenv("ROSE_BASE", base, 1) == 0);
			for(size_t i = 0; i < sizeof(suffixes) / sizeof(*suffixes); ++i) {
				memset(output, '?', sizeof(output));
				if(length == 235) {
					assert(rose_topic_to_shm(output, 256, suffixes[i]));
					assert(strlen(output) == 237 + strlen(suffixes[i]));
					assert(output[0] == '/' && output[236] == '%');
					assert(strcmp(output + 237, suffixes[i]) == 0);
					assert(output[256] == '?');
				} else {
					assert(!rose_topic_to_shm(output, 256, suffixes[i]));
					assert(rose_last_error_code() == ROSE_ERR_BADINPUT);
					for(size_t j = 0; j < sizeof(output); ++j) {
						assert(output[j] == '?');
					}
				}
			}
			if(length == 235) {
				// A 255-byte transport name still needs a separate NUL byte.
				for(size_t capacity = 0; capacity <= 255; capacity += 255) {
					memset(output, '?', sizeof(output));
					assert(!rose_topic_to_shm(output, capacity, ROSE_TOPIC_HEARTBEAT));
					for(size_t j = 0; j < sizeof(output); ++j) {
						assert(output[j] == '?');
					}
				}
				// Global base validity and node-private capacity are separate.
				int node_fits = 237 + 1 + (size_t) pid_length + strlen("%control") <= 255;
				rose_node *node = rose_create_node("n", -1, NULL, -1, NULL, NULL, 0, NULL, NULL);
				assert((node != NULL) == node_fits);
				if(node) {
					rose_shutdown(node);
				}
				node = rose_init(1, argv, "n", NULL, NULL);
				assert((node != NULL) == node_fits);
				if(node) {
					rose_shutdown(node);
				}
			} else {
				assert(!rose_topic_to_shm(output, sizeof(output), "short"));
				assert(!rose_create_node("n", -1, NULL, -1, NULL, NULL, 0, NULL, NULL));
				assert(!rose_init(1, argv, "n", NULL, NULL));
			}
		}
	}
	// Private channels include namespace, node name and the current PID.
	// Derive the name length so these bounds remain exact for every PID width.
	memset(base, 'b', 220);
	base[220] = '\0';
	assert(setenv("ROSE_BASE", base, 1) == 0);
	char name[256];
	size_t name_length = 255 - (220 + 2 + strlen("ns/")) - (size_t) pid_length - strlen("%control");
	assert(name_length > 0 && name_length + 1 < sizeof(name));
	memset(name, 'n', name_length);
	name[name_length] = '\0';
	rose_node *node = rose_create_node(name, -1, "ns", -1, NULL, NULL, 0, NULL, NULL);
	assert(node);
	assert(strlen((char *) node->control_sub.sub->name) == 255);
	assert(strlen((char *) node->param_sub.sub->name) == 254);
	rose_shutdown(node);
	char *namespaced[] = {"test", "__ns:=ns"};
	node = rose_init(2, namespaced, name, NULL, NULL);
	assert(node && strlen((char *) node->control_sub.sub->name) == 255);
	rose_shutdown(node);
	name[name_length] = 'n';
	name[name_length + 1] = '\0';
	assert(!rose_create_node(name, -1, "ns", -1, NULL, NULL, 0, NULL, NULL));
	assert(!rose_init(2, namespaced, name, NULL, NULL));
}

int main(void) {
	test_configuration();
	test_base_boundaries();
	assert(setenv("ROSE_BASE", "/routing_test/", 1) == 0);
	char *argv[130];
	argv[0] = "test";
	for(int i = 1; i < 130; ++i) {
		argv[i] = "ordinary_argument";
	}
	argv[1] = "__ns:=/robot/front/";
	argv[2] = "local:=early";
	argv[3] = "local:=mapped";
	argv[4] = "mapped:=chained";
	argv[5] = "/absolute:=/global/destination";
	argv[6] = "/robot/front/equivalent:=/outside";
	argv[129] = "late:=/last_argument";
	rose_node *node = rose_init(130, argv, "router", NULL, NULL);
	assert(node && strcmp(node->prefix, "/routing_test%robot%front%") == 0);
	assert(strncmp(node->name, "robot/front/router-", 19) == 0);
	expect_topic(node, "local", "/routing_test%robot%front%mapped");
	expect_topic(node, "/robot/front/local", "/routing_test%robot%front%mapped");
	expect_topic(node, "/absolute", "/routing_test%global%destination");
	expect_topic(node, "equivalent", "/routing_test%outside");
	expect_topic(node, "late", "/routing_test%last_argument");
	expect_topic(node, "unmapped", "/routing_test%robot%front%unmapped");
	char expected[256];
	snprintf(expected, sizeof(expected), "%s%s%%params", node->prefix, strrchr(node->name, '/') + 1);
	assert(strcmp((char *) node->param_sub.sub->name, expected) == 0);
	snprintf(expected, sizeof(expected), "%s%s%%control", node->prefix, strrchr(node->name, '/') + 1);
	assert(strcmp((char *) node->control_sub.sub->name, expected) == 0);
	char shm[256];
	assert(rose_topic_to_shm(shm, sizeof(shm), "/robot/front/unmapped"));
	assert(strcmp(shm, (char *) node->pubs[node->pubs_length - 1].pub->name) == 0);
	const char *bad_topics[] = {"a%b", "a,b", "a//b", "a/", "/", "a b", "a:b", "", "*", "a/*"};
	for(size_t i = 0; i < sizeof(bad_topics) / sizeof(*bad_topics); ++i) {
		assert(!rose_topic_to_shm(shm, sizeof(shm), bad_topics[i]));
		assert(!rose_create_pub(node, (char *) bad_topics[i], -1, 64, 16));
		assert(!rose_create_sub(node, (char *) bad_topics[i], -1, 0, NULL));
	}
	rose_shutdown(node);
	char *root[] = {"test", "__ns:=/"};
	node = rose_init(2, root, "root", NULL, NULL);
	assert(node && strcmp(node->prefix, "/routing_test%") == 0);
	rose_shutdown(node);
	char *invalid_rule[] = {"test", "a:=b,c"};
	assert(!rose_init(2, invalid_rule, "bad", NULL, NULL));
	char *wildcard_rule[] = {"test", "a/*:=b"};
	assert(!rose_init(2, wildcard_rule, "bad", NULL, NULL));
	assert(!rose_init(-1, NULL, "bad", NULL, NULL));
	assert(!rose_init(1, NULL, "bad", NULL, NULL));
	const char *bad_bases[] = {"", "/", "a/b", "a%b", "a,b", "//base", "base//"};
	for(size_t i = 0; i < sizeof(bad_bases) / sizeof(*bad_bases); ++i) {
		assert(setenv("ROSE_BASE", bad_bases[i], 1) == 0);
		assert(!rose_topic_to_shm(shm, sizeof(shm), "topic"));
		assert(!rose_create_node("node", -1, NULL, -1, NULL, NULL, 0, NULL, NULL));
	}
	puts("topic routing: remaps, argv[129], namespace channels and canonical names passed");
	return 0;
}
