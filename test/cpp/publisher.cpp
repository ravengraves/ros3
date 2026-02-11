#include "ros3.h"

#include <cstdlib>
#include <cstdio>
#include <string>

namespace {

int64_t parse_i64(const char *s, int64_t fallback) {
	if(!s) {
		return fallback;
	}
	char *end = nullptr;
	long long v = std::strtoll(s, &end, 10);
	if(end == s || (end && *end != '\0')) {
		return fallback;
	}
	return (int64_t) v;
}

double parse_f64(const char *s, double fallback) {
	if(!s) {
		return fallback;
	}
	char *end = nullptr;
	double v = std::strtod(s, &end);
	if(end == s || (end && *end != '\0')) {
		return fallback;
	}
	return v;
}

} // namespace

int main(int argc, char **argv) {
	std::string topic = (argc > 1) ? argv[1] : "chatter";
	int64_t max_messages = (argc > 2) ? parse_i64(argv[2], -1) : -1;
	double hz = (argc > 3) ? parse_f64(argv[3], 1.0) : 1.0;
	if(hz <= 0.0) {
		hz = 1.0;
	}

	char node_name[] = "cpp_publisher";
	rose_node *node = rose_init((char) argc, argv, node_name, nullptr, nullptr);
	if(!node) {
		std::fprintf(stderr, "Failed to initialize node\n");
		return 1;
	}

	rose_publisher *pub = rose_create_pub(node,
					      const_cast<char *>(topic.c_str()),
					      -1,
					      1024,
					      128);
	if(!pub) {
		std::fprintf(stderr, "Failed to create publisher for topic '%s'\n", topic.c_str());
		rose_shutdown(node);
		return 2;
	}

	mirage_msg *msg = mirage_create(1024, nullptr);
	if(!msg) {
		std::fprintf(stderr, "Failed to allocate message buffer\n");
		rose_shutdown(node);
		return 3;
	}

	int64_t count = 0;
	while(rose_ok(node) && (max_messages < 0 || count < max_messages)) {
		mirage_write_start(msg);
		mirage_write_fn(msg, "List", -1, 2);
		mirage_write_string(msg, "Hello", -1);
		mirage_write_i64(msg, count);

		if(rose_publish(pub, msg) < 0) {
			std::fprintf(stderr, "Failed to publish message %lld\n", (long long) count);
			mirage_destroy(&msg, nullptr);
			rose_shutdown(node);
			return 4;
		}

		std::printf("Published: ['Hello', %lld]\n", (long long) count);
		count += 1;
		rose_sleep_rate(hz);
	}

	mirage_destroy(&msg, nullptr);
	rose_shutdown(node);
	return 0;
}
