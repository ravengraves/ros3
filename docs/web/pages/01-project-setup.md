## Project Setup

### Minimal layout

```text
my_ros3_app/
  src/
    pub.c
    sub.c
  Makefile
```

### Compile against ros3

Have your makefile essentially execute the following:
```bash
cc -O2 -I/usr/local/include src/pub.c -L/usr/local/lib -lros3 -lmirage -lquicksand -o pub
cc -O2 -I/usr/local/include src/sub.c -L/usr/local/lib -lros3 -lmirage -lquicksand -o sub
```

Expose the following options for install in your build file:
```
PREFIX ?= /usr/local
BIN_INSTALL_DIR := $(DESTDIR)$(PREFIX)/bin
LIB_INSTALL_DIR := $(DESTDIR)$(PREFIX)/lib
INCLUDE_INSTALL_DIR := $(DESTDIR)$(PREFIX)/include
```

Then, install with Makefile targets similar to the following:
```
install-bin: $(BIN_TARGETS)
  mkdir -p $(BIN_INSTALL_DIR)
  install -m 0755 $(BIN_TARGETS) $(BIN_INSTALL_DIR)/

install-lib: $(LIB_TARGETS) $(PC_FILE)
  mkdir -p $(LIB_INSTALL_DIR) $(INCLUDE_INSTALL_DIR)
  install -m 0644 $(BUILD_DIR)/libros3.dylib $(BUILD_DIR)/libros3.a $(BUILD_DIR)/libros3_gcc_lto.a $(LIB_INSTALL_DIR)/
  install -m 0644 $(HEADER) $(INCLUDE_INSTALL_DIR)/ros3.h
```

These will be set to a ros3 workspace (ros3/bin ros3/lib ros3/include) during development.


### Runtime namespace

```bash
export ROSE_BASE=robot1
```

### Startup workflow

Terminal 1 (master):

```bash
./build/rosecore
```

Terminal 2 (publisher):

```bash
ROSE_BASE=robot1 ./pub
```

Terminal 3 (subscriber):

```bash
ROSE_BASE=robot1 ./sub
```

Use one namespace per robot or environment to avoid topic collisions.
