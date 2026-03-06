ROSENET_RX(1)

# NAME

rosenet_rx - receive TCP or UDP frames and publish to ROS3 topic

# SYNOPSIS

*rosenet_rx* <topic> <port> <msg_size> <slots> [--udp]

*rosenet_rx* --topic <topic> --port <port> [--udp]++
  [--message_size N] [--queue_depth N]

# DESCRIPTION

Listens on a network socket and republishes frames into a local ROS3 topic.

# EXAMPLES

## Receive TCP frames and publish to a topic

$ rosenet_rx /camera 9000 65536 1024

## Receive UDP frames

$ rosenet_rx --topic /camera --port 9000 --udp

## Use explicit message size and queue depth

$ rosenet_rx --topic /camera --port 9000 --message_size 1048576 --queue_depth 512
