CC := clang
AR := llvm-ar
GCC ?= gcc
GCC_AR ?= gcc-ar
CXX ?= g++
PREFIX ?= /usr/local

# Base flags without LTO
CFLAGS_BASE := -Wall -Wextra -Wpedantic -Werror -std=c11 -fPIC \
	-ffunction-sections -fdata-sections \
	-fno-stack-protector -D_FORTIFY_SOURCE=0 \
	-I . -O3 -mtune=native

# LTO flags for static library only (executable linking)
CFLAGS_LTO := $(CFLAGS_BASE) -flto

# GCC LTO flags for a g++-linkable static archive
CFLAGS_GCC_LTO := $(CFLAGS_BASE) -flto -Wno-pedantic

# Non-LTO for shared library (Python/dlopen compatibility)
CFLAGS_SO := $(CFLAGS_BASE)

CEXE := -fuse-ld=lld -Wl,-gc-sections,-icf=safe,-s
LDFLAGS := -fuse-ld=lld
CXXFLAGS := -Wall -Wextra -Wpedantic -Werror -std=c++17 -I . -O3 -mtune=native

# arduino version: TARGET=embedded
SOURCE := ros3.c

ifeq ($(ARCH),x86_64)
	CFLAGS_BASE += -march=native
	CFLAGS_LTO += -march=native
	CFLAGS_GCC_LTO += -march=native
	CFLAGS_SO += -march=native
endif

ifeq ($(ARCH),aarch64)
	CFLAGS_BASE += -march=armv8-a
	CFLAGS_LTO += -march=armv8-a
	CFLAGS_GCC_LTO += -march=armv8-a
	CFLAGS_SO += -march=armv8-a
endif

all: build/libros3.so build/libros3.a

# Shared library - no LTO for Python/dlopen compatibility
build/libros3.so: build/ros3_so.o
	$(CC) -shared -o build/libros3.so build/ros3_so.o $(LDFLAGS) -L$(PREFIX)/lib -lmirage -lquicksand

# Static library - with LTO for best performance in C programs
build/libros3.a: build/ros3_lto.o
	$(AR) rcs build/libros3.a build/ros3_lto.o

# Static library - GCC LTO object for g++/gcc link compatibility
build/libros3_gcc_lto.a: build/ros3_gcc_lto.o
	$(GCC_AR) rcs build/libros3_gcc_lto.a build/ros3_gcc_lto.o

build/ros3_so.o: $(SOURCE)
	mkdir -p build
	$(CC) -c $(CFLAGS_SO) $(SOURCE) -o build/ros3_so.o

build/ros3_lto.o: $(SOURCE)
	mkdir -p build
	$(CC) -c $(CFLAGS_LTO) $(SOURCE) -o build/ros3_lto.o

build/ros3_gcc_lto.o: $(SOURCE)
	mkdir -p build
	$(GCC) -c $(CFLAGS_GCC_LTO) $(SOURCE) -o build/ros3_gcc_lto.o

build/test: test.c build/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o build/test test.c build/libros3.a -L/usr/local/lib -l mirage -l quicksand

build/rosemaster: rosemaster.c build/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o build/rosemaster rosemaster.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/rosenode: rosenode.c build/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o build/rosenode rosenode.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/rosetopic: rosetopic.c build/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o build/rosetopic rosetopic.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/rosenet_tx: rosenet_tx.c build/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o build/rosenet_tx rosenet_tx.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/rosenet_rx: rosenet_rx.c build/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o build/rosenet_rx rosenet_rx.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/test_net_pub: test_net_pub.c build/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o build/test_net_pub test_net_pub.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/test_net_sub: test_net_sub.c build/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o build/test_net_sub test_net_sub.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/test_cpp_publisher: test/cpp/publisher.cpp build/libros3_gcc_lto.a
	$(CXX) $(CXXFLAGS) -o build/test_cpp_publisher test/cpp/publisher.cpp build/libros3_gcc_lto.a -L/usr/local/lib -lmirage -lquicksand

build/test_cpp_subscriber: test/cpp/subscriber.cpp build/libros3_gcc_lto.a
	$(CXX) $(CXXFLAGS) -o build/test_cpp_subscriber test/cpp/subscriber.cpp build/libros3_gcc_lto.a -L/usr/local/lib -lmirage -lquicksand

build/roseserial: roseserial.c build/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o build/roseserial roseserial.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

compile_commands.json: Makefile
	@echo '[\n' \
	'{"directory":"$(PWD)","command":"$(CC) -c $(CFLAGS) $(SOURCE) -o build/ros3.o","file":"$(SOURCE)"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -shared -o build/libros3.so build/ros3.o $(LDFLAGS)","file":"build/libros3.so"},\n' \
	'{"directory":"$(PWD)","command":"$(AR) rcs build/libros3.a build/ros3.o","file":"build/libros3.a"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) $(CFLAGS) $(CEXE) -o build/test test.c build/libros3.a -fuse-ld=lld -Wl,-gc-sections,-icf=safe","file":"test.c"}\n]' \
	> $@

install: build/libros3.so build/libros3.a
	mkdir -p $(PREFIX)/lib
	mkdir -p $(PREFIX)/include
	install -m 0644 build/libros3.so $(PREFIX)/lib/
	install -m 0644 build/libros3.a $(PREFIX)/lib/
	install -m 0644 ros3.h $(PREFIX)/include/
	sudo ldconfig

uninstall:
	rm -f $(PREFIX)/lib/libros3.so
	rm -f $(PREFIX)/lib/libros3.a
	rm -f $(PREFIX)/include/ros3.h
	sudo ldconfig

format:
	clang-format -style=file -i $$(find . | grep -e '\.c$$' -e '\.h$$')

clean:
	rm -rf build

check: build/rosemaster build/rosenode build/rosetopic build/test build/rosenet_tx \
		build/rosenet_rx build/test_net_pub build/test_net_sub
	./test_utils.sh
	./test_net.sh

# =============================================================================
# Python bindings
# =============================================================================

python: build/libros3.so
	cd lang/python && pip install .

check-python: build/libros3.so
	cd lang/python && pip install --break-system-packages -e . && cd ../.. && python3 test/python/test_pubsub.py

check-cpp: build/rosemaster build/test_cpp_publisher build/test_cpp_subscriber
	./test_cpp.sh

.PHONY: format clean all check check-net install uninstall python check-python check-cpp
