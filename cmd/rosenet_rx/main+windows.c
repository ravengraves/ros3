#include <stdio.h>
#include <string.h>

static int is_help_arg(const char *arg) {
	return arg && (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0 || strcmp(arg, "/h") == 0 || strcmp(arg, "/help") == 0 || strcmp(arg, "/?") == 0);
}

int main(int argc, char **argv) {
	if(argc > 1 && is_help_arg(argv[1])) {
		printf("%s: Windows tool entrypoint (MinGW build).\n", argv[0]);
		return 0;
	}
	fprintf(stderr, "%s: native Windows implementation pending; use Linux/macOS binary for now.\n", argv[0]);
	return 1;
}
