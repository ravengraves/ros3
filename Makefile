CC := clang
AR := llvm-ar
GCC ?= gcc
GCC_AR ?= gcc-ar
CXX ?= g++

BUILD_DIR := build
INSTALL_DIR ?= install

CFLAGS_BASE := -Wall -Wextra -Wpedantic -Werror -std=c11 -fPIC \
	-ffunction-sections -fdata-sections \
	-fno-stack-protector -D_FORTIFY_SOURCE=0 \
	-Iinclude -O3 -mtune=native
CFLAGS_LTO := $(CFLAGS_BASE) -flto
CFLAGS_GCC_LTO := $(CFLAGS_BASE) -flto -Wno-pedantic
CFLAGS_SO := $(CFLAGS_BASE)

CEXE := -fuse-ld=lld -Wl,-gc-sections,-icf=safe,-s
LDFLAGS := -fuse-ld=lld
CXXFLAGS := -Wall -Wextra -Wpedantic -Werror -std=c++17 -Iinclude -O3 -mtune=native

SOURCE := src/ros3.c
HEADER := include/ros3.h
COMPLETION_BASH := completions/ros3.bash

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

all: $(BUILD_DIR)/libros3.so $(BUILD_DIR)/libros3.a $(BUILD_DIR)/libros3_gcc_lto.a

$(BUILD_DIR)/libros3.so: $(BUILD_DIR)/ros3_so.o
	$(CC) -shared -o $@ $< $(LDFLAGS) -L/usr/local/lib -lmirage -lquicksand

$(BUILD_DIR)/libros3.a: $(BUILD_DIR)/ros3_lto.o
	$(AR) rcs $@ $<

$(BUILD_DIR)/libros3_gcc_lto.a: $(BUILD_DIR)/ros3_gcc_lto.o
	$(GCC_AR) rcs $@ $<

$(BUILD_DIR)/ros3_so.o: $(SOURCE)
	mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_SO) $< -o $@

$(BUILD_DIR)/ros3_lto.o: $(SOURCE)
	mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_LTO) $< -o $@

$(BUILD_DIR)/ros3_gcc_lto.o: $(SOURCE)
	mkdir -p $(BUILD_DIR)
	$(GCC) -c $(CFLAGS_GCC_LTO) $< -o $@

$(BUILD_DIR)/test: cmd/test/main.c $(BUILD_DIR)/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o $@ $< $(BUILD_DIR)/libros3.a -L/usr/local/lib -lmirage -lquicksand

$(BUILD_DIR)/rosemaster: cmd/rosemaster/main.c $(BUILD_DIR)/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o $@ $< $(BUILD_DIR)/libros3.a -L/usr/local/lib -lmirage -lquicksand

$(BUILD_DIR)/rosenode: cmd/rosenode/main.c $(BUILD_DIR)/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o $@ $< $(BUILD_DIR)/libros3.a -L/usr/local/lib -lmirage -lquicksand

$(BUILD_DIR)/rosetopic: cmd/rosetopic/main.c $(BUILD_DIR)/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o $@ $< $(BUILD_DIR)/libros3.a -L/usr/local/lib -lmirage -lquicksand

$(BUILD_DIR)/rosenet_tx: cmd/rosenet_tx/main.c $(BUILD_DIR)/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o $@ $< $(BUILD_DIR)/libros3.a -L/usr/local/lib -lmirage -lquicksand

$(BUILD_DIR)/rosenet_rx: cmd/rosenet_rx/main.c $(BUILD_DIR)/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o $@ $< $(BUILD_DIR)/libros3.a -L/usr/local/lib -lmirage -lquicksand

$(BUILD_DIR)/roseserial: cmd/roseserial/main.c $(BUILD_DIR)/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o $@ $< $(BUILD_DIR)/libros3.a -L/usr/local/lib -lmirage -lquicksand

$(BUILD_DIR)/test_net_pub: cmd/test_net_pub/main.c $(BUILD_DIR)/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o $@ $< $(BUILD_DIR)/libros3.a -L/usr/local/lib -lmirage -lquicksand

$(BUILD_DIR)/test_net_sub: cmd/test_net_sub/main.c $(BUILD_DIR)/libros3.a
	$(CC) $(CFLAGS_LTO) $(CEXE) -o $@ $< $(BUILD_DIR)/libros3.a -L/usr/local/lib -lmirage -lquicksand

$(BUILD_DIR)/test_cpp_publisher: test/cpp/publisher.cpp $(BUILD_DIR)/libros3_gcc_lto.a
	$(CXX) $(CXXFLAGS) -o $@ $< $(BUILD_DIR)/libros3_gcc_lto.a -L/usr/local/lib -lmirage -lquicksand

$(BUILD_DIR)/test_cpp_subscriber: test/cpp/subscriber.cpp $(BUILD_DIR)/libros3_gcc_lto.a
	$(CXX) $(CXXFLAGS) -o $@ $< $(BUILD_DIR)/libros3_gcc_lto.a -L/usr/local/lib -lmirage -lquicksand

install: all $(BUILD_DIR)/rosemaster $(BUILD_DIR)/rosenode $(BUILD_DIR)/rosetopic $(BUILD_DIR)/rosenet_tx $(BUILD_DIR)/rosenet_rx $(BUILD_DIR)/roseserial
	mkdir -p $(INSTALL_DIR)/bin $(INSTALL_DIR)/lib $(INSTALL_DIR)/include $(INSTALL_DIR)/completions/bash
	install -m 0755 $(BUILD_DIR)/rosemaster $(INSTALL_DIR)/bin/
	install -m 0755 $(BUILD_DIR)/rosenode $(INSTALL_DIR)/bin/
	install -m 0755 $(BUILD_DIR)/rosetopic $(INSTALL_DIR)/bin/
	install -m 0755 $(BUILD_DIR)/rosenet_tx $(INSTALL_DIR)/bin/
	install -m 0755 $(BUILD_DIR)/rosenet_rx $(INSTALL_DIR)/bin/
	install -m 0755 $(BUILD_DIR)/roseserial $(INSTALL_DIR)/bin/
	install -m 0644 $(BUILD_DIR)/libros3.so $(INSTALL_DIR)/lib/
	install -m 0644 $(BUILD_DIR)/libros3.a $(INSTALL_DIR)/lib/
	install -m 0644 $(HEADER) $(INSTALL_DIR)/include/
	install -m 0644 $(COMPLETION_BASH) $(INSTALL_DIR)/completions/bash/

format:
	clang-format -style=file -i $$(find . | grep -e '\.c$$' -e '\.h$$')

clean:
	rm -rf $(BUILD_DIR)

clean-install:
	rm -rf $(INSTALL_DIR)/bin $(INSTALL_DIR)/lib $(INSTALL_DIR)/include $(INSTALL_DIR)/completions

check: $(BUILD_DIR)/rosemaster $(BUILD_DIR)/rosenode $(BUILD_DIR)/rosetopic $(BUILD_DIR)/test $(BUILD_DIR)/rosenet_tx \
		$(BUILD_DIR)/rosenet_rx $(BUILD_DIR)/test_net_pub $(BUILD_DIR)/test_net_sub
	./test_utils.sh
	./test_net.sh

python: $(BUILD_DIR)/libros3.so
	cd lang/python && pip install .

check-python: $(BUILD_DIR)/libros3.so
	cd lang/python && pip install --break-system-packages -e . && cd ../.. && python3 test/python/test_pubsub.py

check-cpp: $(BUILD_DIR)/rosemaster $(BUILD_DIR)/test_cpp_publisher $(BUILD_DIR)/test_cpp_subscriber
	./test_cpp.sh

.PHONY: format clean clean-install all check install python check-python check-cpp
