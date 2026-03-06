## ROS3 Tools

### Overview

- `rosemaster`: node/topic registry and query endpoint
- `rosenode`: inspect/kill nodes
- `rosetopic`: list/inspect/echo/remove topics
- `rosenet_tx`: bridge local topic to TCP/UDP
- `rosenet_rx`: bridge TCP/UDP to local topic
- `roseserial`: serial device bridge for microcontrollers

### rosemaster

Topic: master registry and discovery service.

Commands and options:

- `rosemaster`
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
- `rosetopic rm <topic>`

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

### roseserial

Topic: serial microcontroller bridge to ROS3 topics.

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
