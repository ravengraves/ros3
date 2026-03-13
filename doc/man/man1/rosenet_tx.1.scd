ROSENET_TX(1)

# NAME

rosenet_tx - forward ROS3 topic traffic to TCP or UDP

# SYNOPSIS

*rosenet_tx* <topic> <host> <port> [--udp]

*rosenet_tx* --topic <topic> --host <host> --port <port> [--udp]++
  [--message_size N] [--read_last 0|1]

# DESCRIPTION

Subscribes to a local ROS3 topic and forwards frames over the network.

# EXAMPLES

## Forward a topic over TCP

$ rosenet_tx /camera 127.0.0.1 9000

## Forward a topic over UDP

$ rosenet_tx --topic /camera --host 192.168.1.40 --port 9000 --udp

## Forward only the latest message with explicit payload sizing

$ rosenet_tx --topic /camera --host 127.0.0.1 --port 9000 --message_size 1048576 --read_last 1
