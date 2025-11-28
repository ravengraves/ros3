CC := clang
AR := llvm-ar
TARGET := normal
PREFIX ?= /usr/local

CFLAGS := -Wall -Wextra -Wpedantic -Werror -std=c11 -fPIC -flto \
	-ffunction-sections -fdata-sections \
	-fno-stack-protector -D_FORTIFY_SOURCE=0 \
	-I . -O3 -mtune=native
CEXE := -fuse-ld=lld -Wl,-gc-sections,-icf=safe,-s
LDFLAGS := -fuse-ld=lld

# arduino version: TARGET=embedded
ifeq ($(TARGET),embedded)
SOURCE := mirage+embedded.c
else
SOURCE := mirage.c
endif

ifeq ($(ARCH),x86_64)
	CFLAGS += -march=native
endif

ifeq ($(ARCH),aarch64)
	CFLAGS += -march=armv8-a
endif

all: build/libmirage.so build/libmirage.a

build/libmirage.so: build/mirage.o
	$(CC) -shared -o build/libmirage.so build/mirage.o $(LDFLAGS)

build/libmirage.a: build/mirage.o
	$(AR) rcs build/libmirage.a build/mirage.o

build/mirage.o: $(SOURCE)
	mkdir -p build
	$(CC) -c $(CFLAGS) $(SOURCE) -o build/mirage.o

build/test: test.c build/libmirage.a
	$(CC) $(CFLAGS) $(CEXE) -o build/test test.c build/libmirage.a # -fsanitize=address,undefined

build/test_all: test_all.c build/libmirage.a
	$(CC) $(CFLAGS) $(CEXE) -o build/test_all test_all.c build/libmirage.a # -fsanitize=address,undefined

compile_commands.json: Makefile
	@echo '[\n' \
	'{"directory":"$(PWD)","command":"$(CC) -c $(CFLAGS) $(SOURCE) -o build/mirage.o","file":"$(SOURCE)"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -shared -o build/libmirage.so build/mirage.o $(LDFLAGS)","file":"build/libmirage.so"},\n' \
	'{"directory":"$(PWD)","command":"$(AR) rcs build/libmirage.a build/mirage.o","file":"build/libmirage.a"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) $(CFLAGS) $(CEXE) -o build/test test.c build/libmirage.a -fuse-ld=lld -Wl,-gc-sections,-icf=safe","file":"test.c"}\n]' \
	> $@

install:
	mkdir -p $(PREFIX)/lib
	mkdir -p $(PREFIX)/include
	install -m 0644 build/libmirage.so $(PREFIX)/lib/
	install -m 0644 build/libmirage.a $(PREFIX)/lib/
	install -m 0644 mirage.h $(PREFIX)/include/
	sudo ldconfig

uninstall:
	rm $(PREFIX)/lib/libmirage.so
	rm $(PREFIX)/lib/libmirage.a
	rm $(PREFIX)/include/mirage.h
	sudo ldconfig

format:
	clang-format -style=file -i $$(find . | grep -e '\.c$$' -e '\.h$$')

clean:
	rm -rf build

check: build/test build/test_all
	./build/test
	./build/test_all

.PHONY: format clean all check install uninstall

