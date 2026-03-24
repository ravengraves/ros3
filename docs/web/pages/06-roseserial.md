## roseserial

`roseserial` bridges serial frames and ROS3 topics.

### Start bridge

```bash
roseserial /dev/ttyUSB0 /mcu/rx /mcu/tx 115200 1024 256
```

If `tx_topic` does not exist yet, `roseserial` keeps running and retries that
subscription once per second. The serial -> `rx_topic` path continues working
immediately.

### Frame format

- 2-byte little-endian payload length
- payload bytes (Mirage-encoded message)

### Arduino sender using Mirage embedded API

```c
#include <Arduino.h>
#include <mirage_embedded.h>

static uint8_t frame[128];
static mirage_msg msg;
static int16_t count = 0;

void setup() {
	Serial.begin(115200);
	mirage_init(&msg, frame, (int16_t)sizeof(frame));
}

void loop() {
	// Canonical Mirage/WXF encoding: List["Hello world", count]
	int16_t ret = 0;
	ret |= mirage_write_start(&msg);
	ret |= mirage_write_fn(&msg, "List", (int16_t)(sizeof("List") - 1), 2);
	ret |= mirage_write_string(&msg, "Hello world", (int16_t)(sizeof("Hello world") - 1));
	ret |= mirage_write_i16(&msg, count);
	if (ret < 0) return;

	const uint16_t len = (uint16_t)msg.length;
	Serial.write((uint8_t)(len & 0xFF));
	Serial.write((uint8_t)((len >> 8) & 0xFF));
	Serial.write(msg.data, len);
	count = (count + 1) % 4096;

	delay(1000);
}
```

Use `mirage_embedded.h` on Arduino so payloads match ROS3 Mirage decoding.
