## Multi-machine Usage

### Pattern

- Run one `rosemaster` per machine namespace.
- Use `rosenet_tx` and `rosenet_rx` to bridge topics over TCP.

### Example

Machine A:

```bash
export ROSE_BASE=robot_a
./build/rosenet_tx --topic /cam --host 10.0.0.2 --port 19999
```

Machine B:

```bash
export ROSE_BASE=robot_b
./build/rosenet_rx --topic /cam_remote --port 19999 --message_size 65536 --message_rate 64
```

### Notes

- Keep message sizes conservative for latency stability.
- Use dedicated bridge topics for high-rate streams.
- Keep clocks reasonably synchronized when debugging timestamps.
- Use SSH tunneling or a VPN when bridging across untrusted networks.
