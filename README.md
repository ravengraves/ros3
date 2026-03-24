# ROS3 🌹

ros3 is a lightweight and high-performance pub/sub middleware for robotic systems.

It is a ROS-like robotics runtime layer with lightning-fast system introspection tools and optional network and serial transports. ROS3 uses [quicksand 🏜️](https://git.sr.ht/~alecgraves/quicksand) for shared-memory IPC and [mirage 🔮](https://git.sr.ht/~alecgraves/mirage) for message encoding.

Build status:
* Ubuntu: [![builds.sr.ht status](https://builds.sr.ht/~alecgraves/ros3/commits/master/ubuntu22.yml.svg)](https://builds.sr.ht/~alecgraves/ros3/commits/master/ubuntu22.yml?)
* Debian: [![builds.sr.ht status](https://builds.sr.ht/~alecgraves/ros3/commits/master/debian.yml.svg)](https://builds.sr.ht/~alecgraves/ros3/commits/master/debian.yml?)
* aarch64: [![builds.sr.ht status](https://builds.sr.ht/~alecgraves/ros3/commits/master/aarch64.yml.svg)](https://builds.sr.ht/~alecgraves/ros3/commits/master/aarch64.yml?)


## Performance
In `rosbench`, comparing ROS3 shared-memory against ROS2 single-process component runs over different communication topologies (averaged across Fast DDS, CycloneDDS, ConnextDDS, and Zenoh middleware on ROS2 Jazzy, with message sizes ranging from 32B to 1KiB):

- Unicast (1:1): ros3 achieves **124.0x** higher throughput and **275,597.8x** lower latency
- Pipeline (1:a:b:c): ros3 achieves **118.1x** higher throughput and **28.8x** lower latency
- Sequencer (many:1): ros3 achieves **407.3x** higher throughput and **44.2x** lower latency
- Multicast (1:many): ros3 achieves **148.1x** higher throughput and **167,314.6x** lower latency
- Diamond (1:2:1): ros3 achieves **1,137.9x** higher throughput and **41.2x** lower latency

(and ROS3 is only hitting ~3.47% of theoretical RAM bandwidth for these message sizes. This could be further optimized with batched message reading.)

See [rosbench](https://git.sr.ht/~alecgraves/rosbench/) for the full benchmark tables and plots.

**Build Speed**
> AMD Ryzen 5 7600X 6-Core Processor

```
$ time bash -c "make -C quicksand -j4 && make -C mirage -j4 && make -C ros3 -j4"
...
real	0m1.034s
user	0m2.540s
sys	0m0.729s
```

ros2 (manually trimmed build, no tests):
- rclc-based minimal build: 371.43s wall clock, 109 total built packages
- rclcpp-based minimal build: 453.77s wall clock, 108 total built packages

This makes ROS3 builds about ~350x-450x faster than ROS2.

**Installation Size**
d
ROS3:

```
$ ls -sh build/
1.4M    build
```

ROS2:
- rclcpp: 68.6M
- rclc: 45.6M

ROS3 is only ~45-60x smaller for a minimal install (though this was with `-O3` for ROS3, vs MinSizeRel / `-O2`).


## Usage - C 🚀

Start `rosecore` in one terminal before running nodes:

```bash
./build/rosecore
```

Publisher - C
```c
#include <mirage.h>
#include <ros3.h>

int main(int argc, char **argv) {
	rose_node *node = rose_init((char)argc, argv, "c_pub", NULL, NULL);
	rose_publisher *pub = rose_create_pub(node, "/chatter", -1, 1024, 128);
	mirage_msg *msg = mirage_create(1024, NULL);

	while(rose_ok(node)) {
		mirage_write_start(msg);
		mirage_write_string(msg, "hello from c", -1);
		rose_publish(pub, msg);
		rose_sleep(0.1);
	}

	return 0;
}
```

Subscriber - C
```c
#include <mirage.h>
#include <ros3.h>
#include <stdio.h>

int main(int argc, char **argv) {
	rose_node *node = rose_init((char)argc, argv, "c_sub", NULL, NULL);
	rose_subscriber *sub = rose_create_sub(node, "/chatter", -1, 1, NULL);
	mirage_msg *msg = mirage_create(1024, NULL);
	char text[256];
	i64 len = 0;

	while(rose_ok(node)) {
		if(rose_read(sub, msg) >= 0) {
			mirage_read_start(msg);
			mirage_read_string(msg, text, &len, sizeof(text) - 1);
			text[len] = '\0';
			printf("%s\n", text);
		}
		rose_sleep(0.01);
	}

	return 0;
}
```

Typical compile lines:
```bash
cc -O2 -I/usr/local/include pub.c -L/usr/local/lib -lros3 -lmirage -lquicksand -o pub
cc -O2 -I/usr/local/include sub.c -L/usr/local/lib -lros3 -lmirage -lquicksand -o sub
```

Run with a shared namespace:
```bash
export ROSE_BASE=robot1
./build/rosecore
./sub
./pub
```

## Usage - Python 🐍

Install Python bindings after the C library is built:
```bash
make python
```

Publisher - Python
```python
import ros3 as rose

node = rose.Node("py_publisher")
publisher = node.publisher("chatter", message_size=1024, rate=128)

while node.ok():
    publisher.publish(["hello from python"])
    rose.sleep(0.1)
```

Subscriber - Python
```python
import ros3 as rose

node = rose.Node("py_subscriber")
subscriber = node.subscriber("chatter")

while node.ok():
    for message in subscriber:
        print(f"Received: {message}")
    rose.sleep(0.01)
```

`publisher.publish(...)` encodes with mirage 🔮 automatically, and `subscriber` yields decoded Python objects.

## Build and Install 📖

Install dependencies first:

- `scdoc`
- `mirage` 🔮
- `quicksand` 🏜️

Example Linux flow:
```bash
sudo apt install scdoc

git clone https://git.sr.ht/~alecgraves/mirage
cd mirage
make
sudo make install
cd ..

git clone https://git.sr.ht/~alecgraves/quicksand
cd quicksand
make
sudo make install
cd ..

git clone https://git.sr.ht/~alecgraves/ros3
cd ros3
make
sudo make install
```

Optional targets:
```bash
make check
make python
make docs
sudo make install-docs
```

On macOS, use `Makefile.darwin` for `ros3` and `quicksand`.

Windows support is in progress. For now, use WSL.

## Tools 🛠

- `rosecore` - central node/topic registry and discovery daemon. Example: `rosecore`
- `rosenode` - inspect or kill running nodes. Example: `rosenode list`
- `rosenode info` - show metadata for one node. Example: `rosenode info demo`
- `rosenode kill` - stop a node by name. Example: `rosenode kill demo`
- `rosetopic` - inspect topic state and traffic. Example: `rosetopic list`
- `rosetopic info` - show publishers and subscribers for a topic. Example: `rosetopic info /chatter`
- `rosetopic echo` - stream topic payload bytes. Example: `rosetopic echo /chatter`
- `rosetopic hz` - measure message rate. Example: `rosetopic hz /chatter`
- `rosetopic rm` - remove an old topic shared-memory segment. Example: `rosetopic rm /chatter`
- `rosenet_tx` - bridge a local topic to TCP or UDP. Example: `rosenet_tx --topic /cam --host 10.0.0.2 --port 19999`
- `rosenet_rx` - receive TCP or UDP and publish into ROS3. Example: `rosenet_rx --topic /cam_remote --port 19999 --message_size 65536 --message_rate 64`
- `roseserial` - bridge a serial device and ROS3 topics. Example: `roseserial /dev/ttyUSB0 /mcu/rx /mcu/tx 115200 1024 256`

Shared environment:

- `ROSE_BASE` - namespace prefix for one robot or environment
- `ROSE_QUERY_TIMEOUT_MS` - query timeout for CLI tools

## Web Docs 📚

- [Build and Installation](docs/web/pages/00-build-install.md)
- [Project Setup](docs/web/pages/01-project-setup.md)
- [Pub/Sub in C](docs/web/pages/02-pubsub-c.md)
- [Pub/Sub in C++](docs/web/pages/03-pubsub-cpp.md)
- [Pub/Sub in Python](docs/web/pages/04-pubsub-python.md)
- [ROS3 Tools](docs/web/pages/05-tools.md)
- [roseserial](docs/web/pages/06-roseserial.md)
- [Multi-machine Usage](docs/web/pages/07-multi-machine.md)
- [Mirage Datatypes Used by ROS3](docs/web/pages/08-mirage-datatypes.md)
- [Package Control (Git Subtree)](docs/web/pages/09-package-control-subtree.md)

## Analysis

Static analysis and sanitizer targets are available from the repo root:
```bash
make compile_commands.json
make tidy
make clang-analyze
make fanalyzer
make sanitize-check
make analysis
```

## Name / Etymology / etc.
ROS3 (pronounced "Rose" 🌹) is the third version of a shared memory message passing system that was developed in 2023 at the Univeristy of Texas at San Antonio for an operating systems class final project. ROS3 is an acronym for Roadrunner/Robotics Object System version 3 (with the other versions being [featherring](https://git.sr.ht/~alecgraves/featherring/) and [ravenring](https://git.sr.ht/~alecgraves/ravenring/)). The ending "3" of ROS3 is also [1337-speak](https://en.wikipedia.org/wiki/Leet) for "E", meaning "Efficient", "Easy to use", and "Easy to install"

ROS3 is based loosely on the [LMAX disruptor](https://lmax-exchange.github.io/disruptor/) (a pun on the weaker 'phaser'), but with protocol modifications to enable less-restrictive time-based coordination between publishers and subscribers as well as read-last behavior for low-latency. ROS3 is also written in C (instead of Java) for better portability and performance on constrained embedded systems.

## License ⚖

MIT License.
