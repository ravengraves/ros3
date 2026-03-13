## roseserial

`roseserial` bridges serial frames and ROS3 topics.

### Start bridge

```bash
roseserial /dev/ttyUSB0 /mcu/rx /mcu/tx 115200 1024 256
```

### Frame format

- 2-byte little-endian payload length
- payload bytes (Mirage-encoded message)

### Arduino sender using Mirage embedded API

```c
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
	ret |= mirage_write_start(&msg);
	ret |= mirage_write_fn(&msg, "String", (int16_t)(sizeof("String") - 1), 1);
	ret |= mirage_write_string(&msg, "hello world", (int16_t)(sizeof("hello world") - 1));
	if (ret < 0) return;

	const uint16_t len = (uint16_t)msg.length;
	Serial.write((uint8_t)(len & 0xFF));
	Serial.write((uint8_t)((len >> 8) & 0xFF));
	Serial.write(msg.data, len);

	delay(1000);
}
```

Use `mirage_embedded.h` on Arduino so payloads match ROS3 Mirage decoding.
