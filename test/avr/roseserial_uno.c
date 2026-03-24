// Build:
//
// avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os -std=c11 -Wall -Wextra \
//   -I/home/alec/code/rose/mirage/include \
//   -o /tmp/roseserial_uno.elf \
//   test/avr/roseserial_uno.c \
//   /home/alec/code/rose/mirage/src/mirage_embedded.c
//
// avr-objcopy -O ihex -R .eeprom /tmp/roseserial_uno.elf /tmp/roseserial_uno.hex
//
// Upload to an Uno on /dev/ttyUSB0:
//
// avrdude -p m328p -c arduino -P /dev/ttyUSB0 -b 115200 \
//   -U flash:w:/tmp/roseserial_uno.hex:i
//
// Verify:
// 
// roseserial /dev/ttyUSB0 /mcu/rx /mcu/tx 115200 1024 256
//
// rosetopic echo /mcu/rx
//
#include <avr/io.h>
#include <util/delay.h>

#include <stdint.h>

#include "mirage_embedded.h"

#ifndef F_CPU
#define F_CPU 16000000UL
#endif

#define BAUD 115200UL
#define UBRR_VALUE 16U

static void uart_init(void) {
	UBRR0H = (uint8_t) (UBRR_VALUE >> 8);
	UBRR0L = (uint8_t) UBRR_VALUE;
	UCSR0A = _BV(U2X0);
	UCSR0B = _BV(TXEN0);
	UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
}

static void uart_write_byte(uint8_t value) {
	while((UCSR0A & _BV(UDRE0)) == 0) {
	}
	UDR0 = value;
}

static void uart_write_buf(const uint8_t *data, uint16_t len) {
	for(uint16_t i = 0; i < len; ++i) {
		uart_write_byte(data[i]);
	}
}

int main(void) {
	static uint8_t frame[128];
	static mirage_msg msg;
	int16_t count = 0;

	uart_init();
	mirage_init(&msg, frame, (int16_t) sizeof(frame));

	for(;;) {
		int16_t ret = 0;

		ret |= mirage_write_start(&msg);
		ret |= mirage_write_fn(&msg, "List", MIRAGE_STRLEN, 2);
		ret |= mirage_write_string(&msg, "Hello world", MIRAGE_STRLEN);
		ret |= mirage_write_i16(&msg, count);

		if(ret >= 0) {
			uint16_t len = (uint16_t) msg.length;
			uart_write_byte((uint8_t) (len & 0xFF));
			uart_write_byte((uint8_t) ((len >> 8) & 0xFF));
			uart_write_buf(msg.data, len);
			count = (int16_t) ((count + 1) % 4096);
		}

		_delay_ms(1000);
	}
}
