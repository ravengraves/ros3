## ROS3 Tools

### Overview

- `rosecore`: node/topic registry and query endpoint
- `rosenode`: inspect/kill nodes
- `rosetopic`: list/inspect/echo/measure/remove topics
- `rosenet_tx`: bridge local topic to TCP/UDP
- `rosenet_rx`: bridge TCP/UDP to local topic
- `roseserial`: serial device bridge for microcontrollers

### rosecore

Topic: master registry and discovery service.

Commands and options:

- `rosecore`
- No CLI flags.

### rosenode

Topic: node lifecycle and node metadata.

Commands and options:

- `rosenode list`
- `rosenode info <name>`
- `rosenode info --pid <pid>`
- `rosenode kill <name>`
- `rosenode kill -9 <name>`

### rosetopic

Topic: topic discovery, inspection, and message streaming.

Commands and options:

- `rosetopic list`
- `rosetopic info <topic>`
- `rosetopic echo <topic>`
- `rosetopic hz <topic>`
- `rosetopic rm <topic>` - delete an old topic (e.g. if you changed the size)

### rosenet_tx

Topic: outbound network bridge from ROS3 topic to TCP/UDP.

Commands and options:

- Positional form:
  `rosenet_tx <topic> <host> <port> [--udp]`
- Flag form:
  `rosenet_tx --topic <topic> --host <host> --port <port> [--udp] [--message_size N] [--read_last 0|1]`
- Options:
  `--topic`, `--host`, `--port`, `--udp`, `--message_size`, `--read_last`

### rosenet_rx

Topic: inbound network bridge from TCP/UDP to ROS3 topic.

Commands and options:

- Positional form:
  `rosenet_rx <topic> <port> <msg_size> <message_rate> [--udp]`
- Flag form:
  `rosenet_rx --topic <topic> --port <port> [--udp] [--message_size N] [--message_rate N]`
- Options:
  `--topic`, `--port`, `--udp`, `--message_size`, `--message_rate`

Network `--message_size` is the exact encoded payload cap (default 65536,
maximum 16MiB), with no benchmark inflation. RX `--message_rate` is a slot
count (default 64); its aligned ring allocation must fit 64MiB. UDP payloads
are capped at 65507 bytes; truncated datagrams are dropped. Source samples
over the configured/transport limit are consumed and dropped, allowing later
valid samples through. TCP frames retain a little-endian u32 length and have
a one-second whole-frame deadline. I/O/publication failures return nonzero.

### roseserial

Topic: serial microcontroller bridge to ROS3 topics.
If the TX topic is missing at startup, `roseserial` keeps running and retries
the subscriber connection once per second.

Commands and options:

- `roseserial <device> <rx_topic> <tx_topic> <baud> <msg_size> <slots>`
- No optional flags.

### Common workflow

```bash
# list nodes and topics
rosenode list
rosetopic list

# inspect one node/topic
rosenode info demo
rosetopic info /camera

# stream payload bytes from a topic
rosetopic echo /camera

# measure topic rate
rosetopic hz /camera

# unlink a topic shared-memory segment
rosetopic rm /camera
```

### Networking examples

```bash
rosenet_tx --topic /cam --host 10.0.0.2 --port 19999 --message_size 65536 --read_last 1
rosenet_rx --topic /cam_remote --port 19999 --message_size 65536 --message_rate 64
```

### Shared environment

- `ROSE_BASE`: namespace prefix (default `rose`)
- `ROSE_QUERY_TIMEOUT_MS`: query timeout in milliseconds
