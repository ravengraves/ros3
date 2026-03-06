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

```bash
cc -O2 -I/usr/local/include src/pub.c -L/usr/local/lib -lros3 -lmirage -lquicksand -o pub
cc -O2 -I/usr/local/include src/sub.c -L/usr/local/lib -lros3 -lmirage -lquicksand -o sub
```

### Runtime namespace

```bash
export ROSE_BASE=robot1
```

### Startup workflow

Terminal 1 (master):

```bash
./build/rosemaster
```

Terminal 2 (subscriber):

```bash
ROSE_BASE=robot1 ./sub
```

Terminal 3 (publisher):

```bash
ROSE_BASE=robot1 ./pub
```

Use one namespace per robot or environment to avoid topic collisions.
