// rosetopic.c - CLI tool for querying and monitoring ROS3 topics
//
// Usage:
//   rosetopic list              - List all topics
//   rosetopic info <topic>      - Show publishers/subscribers for a topic
//   rosetopic echo <topic>      - Subscribe and dump messages as hex
//   rosetopic hz <topic>        - Measure message frequency
//   rosetopic rm <topic>        - Unlink a topic shared-memory segment

#define _POSIX_C_SOURCE 200809L

#include "core.h"
#include "../common/process.h"
#include "../common/query.h"
#include "ros3.h"

#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_help_arg(const char *arg) {
	return strcmp(arg, "-h") == 0
			|| strcmp(arg, "--help") == 0
			|| strcmp(arg, "/h") == 0
			|| strcmp(arg, "/help") == 0
			|| strcmp(arg, "/?") == 0;
}

static volatile int running = 1;

static void handle_signal(int sig) {
	(void) sig;
	running = 0;
}

static void print_usage(void) {
	fprintf(stderr, "Usage: rosetopic <command> [args...]\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Commands:\n");
	fprintf(stderr, "  list              - List all topics\n");
	fprintf(stderr, "  info <topic>      - Show publishers/subscribers\n");
	fprintf(stderr, "  echo <topic>      - Subscribe and dump hex\n");
	fprintf(stderr, "  hz <topic>        - Measure message frequency\n");
	fprintf(stderr, "  rm <topic>        - Unlink a topic shared-memory segment\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Environment:\n");
	fprintf(stderr, "  ROSE_BASE  Namespace (default: 'rose')\n");
	fprintf(stderr, "  ROSE_QUERY_TIMEOUT_MS  Query timeout in milliseconds (default: 1000)\n");
}

static int get_query_timeout_ms(void) {
	const char *s = getenv("ROSE_QUERY_TIMEOUT_MS");
	if(!s || !s[0]) {
		return 1000;
	}

	char *end = NULL;
	long v = strtol(s, &end, 10);
	if(end == s || *end != '\0' || v <= 0 || v > 60000) {
		return 1000;
	}

	return (int) v;
}

static int get_query_poll_step_ms(int timeout_ms) {
	if(timeout_ms <= 20) {
		return 1;
	}
	if(timeout_ms <= 200) {
		return 2;
	}
	return 10;
}

static void build_shm_base_prefix(char *out, size_t out_size) {
	if(!rose_topic_to_shm(out, out_size, "x")) {
		out[0] = '\0';
		return;
	}
	out[strlen(out) - 1] = '\0';
}

// Convert shm topic name to display format, stripping the ROSE_BASE prefix.
static void topic_to_display(char *out, size_t out_size, const char *shm_name) {
	if(!out || out_size == 0) {
		return;
	}
	out[0] = '\0';
	if(!shm_name || !shm_name[0]) {
		return;
	}

	char base_prefix[256];
	build_shm_base_prefix(base_prefix, sizeof(base_prefix));

	const char *src = shm_name;
	size_t base_prefix_len = strlen(base_prefix);
	if(strncmp(shm_name, base_prefix, base_prefix_len) == 0) {
		src = shm_name + base_prefix_len - 1;
	}

	size_t i = 0;
	while(*src && i < out_size - 1) {
		out[i++] = (*src == '%') ? '/' : *src;
		src++;
	}
	out[i] = '\0';
}

// Build query topic name using rose_topic_to_shm helper
static void build_topic_name(char *buf, size_t buf_size, const char *suffix) {
	char topic[256];
	snprintf(topic, sizeof(topic), "rosecore_%s", suffix);
	rose_topic_to_shm(buf, buf_size, topic);
}

static uint8_t *alloc_topic_buffer(quicksand_connection *conn, i64 *buffer_size) {
	if(!conn || !conn->buffer || conn->buffer->message_size == 0 || !buffer_size) {
		return NULL;
	}

	*buffer_size = (i64) conn->buffer->message_size;
	return (uint8_t *) malloc((size_t) *buffer_size);
}

// Skip to latest position in ring buffer before sending request
static void skip_to_latest(quicksand_connection *conn) {
	if(!conn || !conn->buffer) {
		return;
	}

	if(conn->buffer->message_size == 0) {
		conn->read_index = conn->buffer->index;
		conn->read_stamp = quicksand_now();
		return;
	}

	i64 buf_size = 0;
	uint8_t *buf = alloc_topic_buffer(conn, &buf_size);
	if(!buf) {
		conn->read_index = conn->buffer->index;
		conn->read_stamp = quicksand_now();
		return;
	}

	quicksand_read_latest(conn, buf, &buf_size);
	free(buf);
}

// Add topics from CSV to a list (avoiding duplicates)
static int add_topics_from_csv(char topics[][256], int *count, int max,
			       const char *csv) {
	if(!csv || !csv[0]) {
		return 0;
	}

	char buf[256];
	const char *p = csv;

	while(*p) {
		// Skip leading comma
		if(*p == ',') {
			p++;
			continue;
		}

		// Copy topic name
		int i = 0;
		while(*p && *p != ',' && i < 255) {
			buf[i++] = *p++;
		}
		if(*p && *p != ',') {
			while(*p && *p != ',') {
				p++;
			}
			continue;
		}
		buf[i] = '\0';

		if(i == 0) {
			continue;
		}

		// Check for duplicate
		int found = 0;
		for(int j = 0; j < *count; j++) {
			if(strcmp(topics[j], buf) == 0) {
				found = 1;
				break;
			}
		}

		if(!found && *count < max) {
			memcpy(topics[*count], buf, (size_t) i + 1);
			(*count)++;
		}
	}

	return 0;
}

// Compare for qsort
static int topic_cmp(const void *a, const void *b) {
	return strcmp((const char *) a, (const char *) b);
}

static int csv_contains_topic(const char *csv, const char *topic) {
	if(!csv || !topic || !topic[0]) {
		return 0;
	}

	size_t topic_len = strlen(topic);
	const char *p = csv;
	while(*p) {
		const char *start = p;
		while(*p && *p != ',') {
			p++;
		}
		if((size_t) (p - start) == topic_len
		   && strncmp(start, topic, topic_len) == 0) {
			return 1;
		}
		if(*p == ',') {
			p++;
		}
	}

	return 0;
}

// Read responses and collect all unique topics
static int read_and_print_topics(quicksand_connection *conn, rose_query *query) {
	mirage_msg *msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!msg) {
		fprintf(stderr, "Failed to allocate message buffer\n");
		return 1;
	}

	static char name[256];
	static char pubs[ROSE_TOPIC_CSV_SIZE];
	static char subs[ROSE_TOPIC_CSV_SIZE];
	i64 pid, last_heartbeat;

	// Collect unique topics
	static char topics[1024][256];
	int topic_count = 0;

	int timeout_ms = get_query_timeout_ms();
	int poll_ms = get_query_poll_step_ms(timeout_ms);
	u64 wait_start = rose_query_clock();

	while(rose_query_clock() - wait_start < (u64) timeout_ms * 1000000ULL) {
		i64 sz = msg->max_length;
		i64 r = quicksand_read(conn, msg->data, &sz);
		if(r >= 0) {
			msg->length = sz;
			int res = rose_query_parse(query, msg, name, sizeof(name),
						   &pid, &last_heartbeat,
						   pubs, sizeof(pubs),
						   subs, sizeof(subs));
			if(res == 0) {
				break;
			} else if(res == 1) {
				add_topics_from_csv(topics, &topic_count, 1024, pubs);
				add_topics_from_csv(topics, &topic_count, 1024, subs);
			}
		} else {
			quicksand_sleep((u64) poll_ms * 1000000ULL);
		}
	}

	if(!query->complete) {
		fprintf(stderr, "Query timed out before Done\n");
		mirage_destroy(&msg, NULL);
		return 1;
	}

	// Sort and print with display formatting
	qsort(topics, (size_t) topic_count, sizeof(topics[0]), topic_cmp);
	for(int i = 0; i < topic_count; i++) {
		char display[256];
		topic_to_display(display, sizeof(display), topics[i]);
		printf("%s\n", display);
	}

	mirage_destroy(&msg, NULL);
	if(!query->complete) {
		fprintf(stderr, "Query timed out before Done\n");
	}
	return query->complete ? 0 : 1;
}

// Read responses and print info for a specific topic
static int read_and_print_topic_info(quicksand_connection *conn, rose_query *query,
				     const char *topic_filter) {
	mirage_msg *msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!msg) {
		fprintf(stderr, "Failed to allocate message buffer\n");
		return 1;
	}

	static char name[256];
	static char pubs[ROSE_TOPIC_CSV_SIZE];
	static char subs[ROSE_TOPIC_CSV_SIZE];
	i64 pid, last_heartbeat;

	// Collect publishers and subscribers
	static char publishers[256][ROSE_MAX_NAME_LEN + 32];
	static char subscribers[256][ROSE_MAX_NAME_LEN + 32];
	int pub_count = 0, sub_count = 0;

	int timeout_ms = get_query_timeout_ms();
	int poll_ms = get_query_poll_step_ms(timeout_ms);
	u64 wait_start = rose_query_clock();

	while(rose_query_clock() - wait_start < (u64) timeout_ms * 1000000ULL) {
		i64 sz = msg->max_length;
		i64 r = quicksand_read(conn, msg->data, &sz);
		if(r >= 0) {
			msg->length = sz;
			int res = rose_query_parse(query, msg, name, sizeof(name),
						   &pid, &last_heartbeat,
						   pubs, sizeof(pubs),
						   subs, sizeof(subs));
			if(res == 0) {
				break;
			} else if(res == 1) {
				// Check if this node publishes to topic
				if(csv_contains_topic(pubs, topic_filter)) {
					if(pub_count < 256) {
						snprintf(publishers[pub_count], sizeof(publishers[pub_count]), "%s (pid %ld)",
							 name, (long) pid);
						pub_count++;
					}
				}
				// Check if this node subscribes to topic
				if(csv_contains_topic(subs, topic_filter)) {
					if(sub_count < 256) {
						snprintf(subscribers[sub_count], sizeof(subscribers[sub_count]), "%s (pid %ld)",
							 name, (long) pid);
						sub_count++;
					}
				}
			}
		} else {
			quicksand_sleep((u64) poll_ms * 1000000ULL);
		}
	}

	if(!query->complete) {
		fprintf(stderr, "Query timed out before Done\n");
		mirage_destroy(&msg, NULL);
		return 1;
	}

	char display[256];
	topic_to_display(display, sizeof(display), topic_filter);
	printf("Topic: %s\n\n", display);
	printf("Publishers:\n");
	if(pub_count == 0) {
		printf("  (none)\n");
	} else {
		for(int i = 0; i < pub_count; i++) {
			printf("  %s\n", publishers[i]);
		}
	}

	printf("\nSubscribers:\n");
	if(sub_count == 0) {
		printf("  (none)\n");
	} else {
		for(int i = 0; i < sub_count; i++) {
			printf("  %s\n", subscribers[i]);
		}
	}

	mirage_destroy(&msg, NULL);
	if(!query->complete) {
		fprintf(stderr, "Query timed out before Done\n");
	}
	return query->complete ? 0 : 1;
}

// Print data dump: ASCII, decimal, hex (80 chars wide, 8 bytes per line)
static void hex_dump(const uint8_t *data, size_t len) {
	const int bpl = 8; // bytes per line

	for(size_t offset = 0; offset < len; offset += bpl) {
		size_t line_len = (offset + bpl <= len) ? (size_t) bpl : len - offset;

		// Offset (4 hex digits)
		printf("%04zx  ", offset);

		// ASCII representation (the star)
		printf("|");
		for(size_t i = 0; i < (size_t) bpl; i++) {
			if(i < line_len) {
				uint8_t c = data[offset + i];
				putchar(isprint(c) ? c : '.');
			} else {
				putchar(' ');
			}
		}
		printf("|  ");

		// Decimal representation
		for(size_t i = 0; i < (size_t) bpl; i++) {
			if(i < line_len) {
				printf("%3u ", data[offset + i]);
			} else {
				printf("    ");
			}
		}
		printf(" ");

		// Hex representation
		for(size_t i = 0; i < (size_t) bpl; i++) {
			if(i < line_len) {
				printf("%02x ", data[offset + i]);
			} else {
				printf("   ");
			}
		}

		printf("\n");
	}
	printf("\n");
}

// Subscribe to topic and dump messages as hex
static int echo_topic(const char *topic_name) {
	// Convert topic name to shared memory format with ROSE_BASE
	char shm_name[512];
	if(!rose_topic_to_shm(shm_name, sizeof(shm_name), topic_name)) {
		fprintf(stderr, "%s\n", rose_last_error());
		return 1;
	}

	quicksand_connection *sub = NULL;
	int64_t ret = quicksand_connect(&sub, shm_name, -1, 0, 0, NULL);
	if(ret != 0) {
		fprintf(stderr, "Failed to subscribe to %s (shm: %s)\n",
			topic_name, shm_name);
		return 1;
	}

	skip_to_latest(sub);

	printf("Subscribed to %s (Ctrl+C to stop)\n\n", topic_name);

	i64 buf_size = 0;
	uint8_t *buf = alloc_topic_buffer(sub, &buf_size);
	if(!buf) {
		fprintf(stderr, "Failed to allocate topic buffer\n");
		quicksand_disconnect(&sub, NULL);
		return 1;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	running = 1;

	u64 msg_num = 0;
	while(running) {
		i64 sz = buf_size;
		i64 r = quicksand_read(sub, buf, &sz);
		if(r >= 0) {
			msg_num++;
			printf("--- Message %lu (%ld bytes) ---\n",
			       (unsigned long) msg_num, (long) sz);
			hex_dump(buf, (size_t) sz);
		} else {
			rose_nanosleep(10e6); // 10ms poll
		}
	}

	printf("\nStopped.\n");
	free(buf);
	quicksand_disconnect(&sub, NULL);
	return 0;
}

static void print_hz_stats(const char *topic_name, u64 interval_count,
			   f64 total_period, f64 total_period_sq,
			   f64 min_period, f64 max_period) {
	if(!topic_name || interval_count == 0 || total_period <= 0.0) {
		return;
	}

	f64 mean_period = total_period / (f64) interval_count;
	f64 variance = 0.0;
	f64 stddev = 0.0;
	if(interval_count > 1) {
		f64 centered_sum = total_period_sq - ((f64) interval_count) * mean_period * mean_period;
		if(centered_sum < 0.0) {
			centered_sum = 0.0;
		}
		variance = centered_sum / (f64) (interval_count - 1);
		stddev = sqrt(variance);
	}

	printf("%s: %.3f Hz, period avg %.6fs, std %.6fs, min %.6fs, max %.6fs, samples %lu\n",
	       topic_name,
	       (f64) interval_count / total_period,
	       mean_period,
	       stddev,
	       min_period,
	       max_period,
	       (unsigned long) interval_count);
	fflush(stdout);
}

static u64 hz_ring_data_offset(void) {
	u64 offset = (u64) sizeof(quicksand_ringbuffer);
	u64 rem = offset & 63u;
	if(rem == 0u) {
		return offset;
	}
	return offset + (64u - rem);
}

static void hz_record_period(u64 msg_time,
			     u64 *prev_msg_time,
			     u64 *interval_count,
			     f64 *total_period,
			     f64 *total_period_sq,
			     f64 *min_period,
			     f64 *max_period) {
	if(!prev_msg_time || !interval_count || !total_period
	   || !total_period_sq || !min_period || !max_period) {
		return;
	}

	if(*prev_msg_time != 0 && msg_time > *prev_msg_time) {
		f64 period = rose_seconds(msg_time, *prev_msg_time);
		*total_period += period;
		*total_period_sq += period * period;
		if(*interval_count == 0 || period < *min_period) {
			*min_period = period;
		}
		if(*interval_count == 0 || period > *max_period) {
			*max_period = period;
		}
		*interval_count += 1;
	}
	*prev_msg_time = msg_time;
}

static void hz_sample_ring(quicksand_connection *conn,
			   u64 *last_seen_index,
			   u64 *prev_msg_time,
			   u64 *interval_count,
			   f64 *total_period,
			   f64 *total_period_sq,
			   f64 *min_period,
			   f64 *max_period) {
	u64 write_cursor = 0;
	u64 ring_length = 0;
	u64 slot_size = 0;
	u64 data_offset = 0;
	u64 start_index = 0;
	u64 cursor = 0;
	const u8 *base = NULL;

	if(!conn || !conn->buffer || !last_seen_index) {
		return;
	}

	write_cursor = atomic_load_explicit(&conn->buffer->index, memory_order_acquire);
	if(write_cursor == *last_seen_index) {
		return;
	}

	ring_length = conn->buffer->length;
	slot_size = conn->buffer->message_size;
	if(ring_length == 0 || slot_size < 16) {
		*last_seen_index = write_cursor;
		return;
	}

	start_index = *last_seen_index;
	if(start_index > write_cursor) {
		start_index = write_cursor;
	}
	if(write_cursor - start_index > ring_length) {
		start_index = write_cursor - ring_length;
	}

	data_offset = hz_ring_data_offset();
	base = (const u8 *) conn->buffer;
	for(cursor = start_index; cursor < write_cursor; cursor += 1) {
		u64 slot = cursor & (ring_length - 1u);
		const u8 *slot_ptr = base + data_offset + slot * slot_size;
		u64 msg_time = 0;
		memcpy(&msg_time, slot_ptr, sizeof(msg_time));
		if(msg_time == 0) {
			continue;
		}
		hz_record_period(msg_time, prev_msg_time, interval_count,
				 total_period, total_period_sq,
				 min_period, max_period);
	}
	*last_seen_index = write_cursor;
}

static int hz_topic(const char *topic_name) {
	char shm_name[512];
	if(!rose_topic_to_shm(shm_name, sizeof(shm_name), topic_name)) {
		fprintf(stderr, "%s\n", rose_last_error());
		return 1;
	}

	quicksand_connection *sub = NULL;
	int64_t ret = quicksand_connect(&sub, shm_name, -1, 0, 0, NULL);
	if(ret != 0) {
		fprintf(stderr, "Failed to subscribe to %s (shm: %s)\n",
			topic_name, shm_name);
		return 1;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	running = 1;

	printf("Measuring %s (Ctrl+C to stop)\n", topic_name);
	printf("Waiting for messages...\n");

	u64 prev_msg_time = 0;
	u64 last_report = rose_now();
	u64 last_seen_index = 0;
	u64 interval_count = 0;
	f64 total_period = 0.0;
	f64 total_period_sq = 0.0;
	f64 min_period = 0.0;
	f64 max_period = 0.0;

	if(sub->buffer) {
		last_seen_index = atomic_load_explicit(&sub->buffer->index,
						       memory_order_acquire);
	}

	while(running) {
		u64 before = last_seen_index;
		hz_sample_ring(sub, &last_seen_index, &prev_msg_time,
			       &interval_count, &total_period, &total_period_sq,
			       &min_period, &max_period);
		if(last_seen_index == before) {
			rose_nanosleep(10e6); // 10ms poll
		}

		if(interval_count > 0 && rose_elapsed_seconds(last_report) >= 1.0) {
			print_hz_stats(topic_name, interval_count, total_period, total_period_sq,
				       min_period, max_period);
			last_report = rose_now();
			interval_count = 0;
			total_period = 0.0;
			total_period_sq = 0.0;
			min_period = 0.0;
			max_period = 0.0;
		}
	}

	print_hz_stats(topic_name, interval_count, total_period, total_period_sq,
		       min_period, max_period);
	printf("Stopped.\n");

	quicksand_disconnect(&sub, NULL);
	return 0;
}

static int remove_topic(const char *topic_name) {
	char shm_name[512];
	if(!rose_topic_to_shm(shm_name, sizeof(shm_name), topic_name)) {
		fprintf(stderr, "%s\n", rose_last_error());
		return 1;
	}
	quicksand_delete(shm_name, -1);

	printf("Unlinked %s\n", topic_name);
	return 0;
}

int rosetopic_main(int argc, char **argv) {
	int exit_code = 0;
	rose_query query = {0};
	quicksand_connection *conn = NULL;

	if(argc < 2) {
		print_usage();
		return 1;
	}
	if(is_help_arg(argv[1])) {
		print_usage();
		return 0;
	}

	(void) rose_set_pdeathsig(SIGTERM);
	char namespace_probe[256];
	if(!rose_topic_to_shm(namespace_probe, sizeof(namespace_probe), "rosecore_dump")) {
		fprintf(stderr, "%s\n", rose_last_error());
		return 1;
	}

	const char *cmd = argv[1][0] == '/' ? argv[1] + 1 : argv[1];

	if(strcmp(cmd, "list") == 0) {
		char topic[512];
		build_topic_name(topic, sizeof(topic), "dump");

		int64_t ret = quicksand_connect(&conn, topic, -1,
						ROSE_QUERY_MSG_SIZE,
						ROSE_DUMP_RING_SIZE, NULL);
		if(ret != 0) {
			fprintf(stderr, "Failed to connect to %s\n", topic);
			exit_code = 1;
			goto cleanup;
		}

		skip_to_latest(conn);
		if(rose_query_send(&query, ROSE_MSG_DUMP_ALL, NULL, 0) < 0) {
			fprintf(stderr, "Failed to send dump request\n");
			exit_code = 1;
			goto cleanup;
		}
		exit_code = read_and_print_topics(conn, &query);

	} else if(strcmp(cmd, "info") == 0) {
		if(argc < 3) {
			print_usage();
			exit_code = 1;
			goto cleanup;
		}

		char topic[512];
		build_topic_name(topic, sizeof(topic), "dump");

		int64_t ret = quicksand_connect(&conn, topic, -1,
						ROSE_QUERY_MSG_SIZE,
						ROSE_DUMP_RING_SIZE, NULL);
		if(ret != 0) {
			fprintf(stderr, "Failed to connect to %s\n", topic);
			exit_code = 1;
			goto cleanup;
		}

		char topic_filter[256];
		if(!rose_topic_to_shm(topic_filter, sizeof(topic_filter), argv[2])) {
			fprintf(stderr, "Failed to normalize topic %s\n", argv[2]);
			exit_code = 1;
			goto cleanup;
		}

		skip_to_latest(conn);
		if(rose_query_send(&query, ROSE_MSG_DUMP_ALL, NULL, 0) < 0) {
			fprintf(stderr, "Failed to send dump request\n");
			exit_code = 1;
			goto cleanup;
		}
		exit_code = read_and_print_topic_info(conn, &query, topic_filter);

	} else if(strcmp(cmd, "echo") == 0) {
		if(argc < 3) {
			print_usage();
			exit_code = 1;
			goto cleanup;
		}

		return echo_topic(argv[2]);

	} else if(strcmp(cmd, "hz") == 0) {
		if(argc < 3) {
			print_usage();
			exit_code = 1;
			goto cleanup;
		}

		return hz_topic(argv[2]);

	} else if(strcmp(cmd, "rm") == 0) {
		if(argc < 3) {
			print_usage();
			exit_code = 1;
			goto cleanup;
		}

		return remove_topic(argv[2]);

	} else {
		print_usage();
		exit_code = 1;
	}

cleanup:
	quicksand_disconnect(&conn, NULL);
	return exit_code;
}
