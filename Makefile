CC := clang
AR := llvm-ar
PREFIX ?= /usr/local

CFLAGS := -Wall -Wextra -Wpedantic -Werror -std=c11 -fPIC -flto \
	-ffunction-sections -fdata-sections \
	-fno-stack-protector -D_FORTIFY_SOURCE=0 \
	-I . -O3 -mtune=native
CEXE := -fuse-ld=lld -Wl,-gc-sections,-icf=safe,-s
LDFLAGS := -fuse-ld=lld

# arduino version: TARGET=embedded
SOURCE := ros3.c

ifeq ($(ARCH),x86_64)
	CFLAGS += -march=native
endif

ifeq ($(ARCH),aarch64)
	CFLAGS += -march=armv8-a
endif

all: build/libros3.so build/libros3.a

build/libros3.so: build/ros3.o
	$(CC) -shared -o build/libros3.so build/ros3.o $(LDFLAGS)

build/libros3.a: build/ros3.o
	$(AR) rcs build/libros3.a build/ros3.o

build/ros3.o: $(SOURCE)
	mkdir -p build
	$(CC) -c $(CFLAGS) $(SOURCE) -o build/ros3.o

build/test: test.c build/libros3.a
	$(CC) $(CFLAGS) $(CEXE) -o build/test test.c build/libros3.a -L/usr/local/lib -l mirage -l quicksand # -fsanitize=address,undefined

build/rosemaster: rosemaster.c build/libros3.a
	$(CC) $(CFLAGS) $(CEXE) -o build/rosemaster rosemaster.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/rosenode: rosenode.c build/libros3.a
	$(CC) $(CFLAGS) $(CEXE) -o build/rosenode rosenode.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/rosetopic: rosetopic.c build/libros3.a
	$(CC) $(CFLAGS) $(CEXE) -o build/rosetopic rosetopic.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/rosenet_tx: rosenet_tx.c build/libros3.a
	$(CC) $(CFLAGS) $(CEXE) -o build/rosenet_tx rosenet_tx.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/rosenet_rx: rosenet_rx.c build/libros3.a
	$(CC) $(CFLAGS) $(CEXE) -o build/rosenet_rx rosenet_rx.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/test_net_pub: test_net_pub.c build/libros3.a
	$(CC) $(CFLAGS) $(CEXE) -o build/test_net_pub test_net_pub.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/test_net_sub: test_net_sub.c build/libros3.a
	$(CC) $(CFLAGS) $(CEXE) -o build/test_net_sub test_net_sub.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

build/roseserial: roseserial.c build/libros3.a
	$(CC) $(CFLAGS) $(CEXE) -o build/roseserial roseserial.c build/libros3.a -L/usr/local/lib -lmirage -lquicksand

compile_commands.json: Makefile
	@echo '[\n' \
	'{"directory":"$(PWD)","command":"$(CC) -c $(CFLAGS) $(SOURCE) -o build/ros3.o","file":"$(SOURCE)"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -shared -o build/libros3.so build/ros3.o $(LDFLAGS)","file":"build/libros3.so"},\n' \
	'{"directory":"$(PWD)","command":"$(AR) rcs build/libros3.a build/ros3.o","file":"build/libros3.a"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) $(CFLAGS) $(CEXE) -o build/test test.c build/libros3.a -fuse-ld=lld -Wl,-gc-sections,-icf=safe","file":"test.c"}\n]' \
	> $@

install:
	mkdir -p $(PREFIX)/lib
	mkdir -p $(PREFIX)/include
	install -m 0644 build/libros3.so $(PREFIX)/lib/
	install -m 0644 build/libros3.a $(PREFIX)/lib/
	install -m 0644 mirage.h $(PREFIX)/include/
	sudo ldconfig

uninstall:
	rm $(PREFIX)/lib/libros3.so
	rm $(PREFIX)/lib/libros3.a
	rm $(PREFIX)/include/mirage.h
	sudo ldconfig

format:
	clang-format -style=file -i $$(find . | grep -e '\.c$$' -e '\.h$$')

clean:
	rm -rf build

check: build/rosemaster build/rosenode build/rosetopic build/test build/rosenet_tx \
		build/rosenet_rx build/test_net_pub build/test_net_sub
	./test_utils.sh
	./test_net.sh

.PHONY: format clean all check check-net install uninstall

