ROSESERIAL(1)

# NAME

roseserial - bidirectional serial bridge for ROS3 topics

# SYNOPSIS

*roseserial* <device> <rx_topic> <tx_topic> <baud> <msg_size> <slots>

# DESCRIPTION

Bridges a serial device and ROS3 topics using a length-prefixed frame format.

# EXAMPLES

## Bridge a microcontroller at 115200 baud

$ roseserial /dev/ttyUSB0 /mcu/rx /mcu/tx 115200 1024 256

## Use a different namespace for isolated testing

$ export ROSE_BASE=robot2
$ roseserial /dev/ttyACM0 /mcu/rx /mcu/tx 921600 4096 512
