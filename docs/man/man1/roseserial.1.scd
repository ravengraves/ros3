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

## Arduino hello world sender (serial -> _rx_topic_, Mirage payload)

Run _roseserial_ at matching baud (example uses 115200):

$ roseserial /dev/ttyUSB0 /mcu/rx /mcu/tx 115200 1024 256

Arduino sketch example:

	#include <Arduino.h>
	#include <mirage_embedded.h>
	static uint8_t frame[128];
	static mirage_msg msg;
	void setup() {
	Serial.begin(115200);
	mirage_init(&msg, frame, (int16_t)sizeof(frame));
	}
	void loop() {
	int16_t ret = 0;
	// Encode: String["hello world"] using Mirage embedded API.
	ret |= mirage_write_start(&msg);
	ret |= mirage_write_fn(&msg, "String", MIRAGE_STRLEN, 1);
	ret |= mirage_write_string(&msg, "hello world", MIRAGE_STRLEN);
	if (ret < 0) return;
	const uint16_t len = (uint16_t)msg.length;
	// Frame format: [2-byte little-endian length][payload bytes]
	Serial.write((uint8_t)(len & 0xFF));
	Serial.write((uint8_t)((len >> 8) & 0xFF));
	Serial.write(msg.data, len);
	delay(1000);
	}
