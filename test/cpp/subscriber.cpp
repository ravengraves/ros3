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

bool decode_payload(mirage_msg *msg, std::string &label_out, int64_t &value_out) {
	int64_t fn_len = 0;
	int64_t fn_argc = 0;
	char label[128];
	int64_t label_len = 0;
	int ret = 0;

	ret |= mirage_read_start(msg);
	ret |= mirage_read_fn(msg, nullptr, &fn_len, MIRAGE_SKIP_COPY, &fn_argc);
	ret |= mirage_read_string(msg, label, &label_len, (int64_t) sizeof(label) - 1);
	ret |= mirage_read_i64(msg, &value_out);

	if(ret < 0 || label_len < 0 || label_len >= (int64_t) sizeof(label)) {
		return false;
	}

	label[label_len] = '\0';
	label_out.assign(label, (size_t) label_len);
	return true;
}

} // namespace

int main(int argc, char **argv) {
	std::string topic = (argc > 1) ? argv[1] : "chatter";
	int64_t max_messages = (argc > 2) ? parse_i64(argv[2], -1) : -1;

	char node_name[] = "cpp_subscriber";
	rose_node *node = rose_init(argc, argv, node_name, nullptr, nullptr);
	if(!node) {
		std::fprintf(stderr, "Failed to initialize node\n");
		return 1;
	}

	rose_subscriber *sub = nullptr;
	for(int i = 0; i < 2000 && !sub; ++i) {
		sub = rose_create_sub(node,
					     const_cast<char *>(topic.c_str()),
					     -1,
					     0,
					     nullptr);
		if(!sub) {
			rose_sleep(1e-3);
		}
	}
	if(!sub) {
		std::fprintf(stderr, "Failed to create subscriber for topic '%s'\n", topic.c_str());
		rose_shutdown(node);
		return 2;
	}

	mirage_msg *msg = mirage_create(1024, nullptr);
	if(!msg) {
		std::fprintf(stderr, "Failed to allocate message buffer\n");
		rose_shutdown(node);
		return 3;
	}

	int64_t received = 0;
	while(rose_ok(node) && (max_messages < 0 || received < max_messages)) {
		if(rose_read(sub, msg) >= 0) {
			std::string label;
			int64_t value = 0;
			if(decode_payload(msg, label, value)) {
				std::printf("Received: ['%s', %lld]\n", label.c_str(), (long long) value);
				received += 1;
			} else {
				std::fprintf(stderr, "Received malformed message\n");
			}
		} else {
			rose_sleep(1e-3);
		}
	}

	mirage_destroy(&msg, nullptr);
	rose_shutdown(node);
	return 0;
}
