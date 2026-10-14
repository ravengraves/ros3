/* Uno/ATmega328P, 16 MHz, 115200 baud.
 * make BUILD_DIR=build-embedded-serial embedded-avr
 * Sends List["Hello world", count] once a second; reset[] restarts at zero.
 * Timer1 schedules greetings only. RX framing has no timeout.
 */
#ifndef F_CPU
#define F_CPU 16000000UL
#endif
#include "../embedded_serial_message.h"
#include <avr/io.h>

static uint8_t tx[128], rx[128];
static mirage_msg message;
static ros3_writer writer;
static ros3_frame frame;

int main(void) {
	UBRR0L = 16;
	UCSR0A = _BV(U2X0);
	UCSR0B = _BV(TXEN0) | _BV(RXEN0);
	UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
	TCCR1B = _BV(CS12) | _BV(CS10);
	mirage_init(&message, tx, sizeof(tx));
	ros3_read_init(&frame, rx, sizeof(rx));
	uint16_t last = TCNT1 - 15625U;
	int16_t count = 0;
	for(;;) {
		if(UCSR0A & _BV(RXC0)) {
			uint8_t flags = UCSR0A, byte = UDR0;
			if(flags & (_BV(FE0) | _BV(DOR0) | _BV(UPE0))) {
				ros3_read_init(&frame, rx, sizeof(rx));
			} else if(ros3_read_byte(&frame, byte) == 0 && demo_reset(&frame)) {
				count = 0;
			}
		}
		if((uint16_t) (TCNT1 - last) >= 15625U) {
			last = TCNT1;
			if(demo_reply(&message, count) == 0
			   && ros3_write_init(&writer, message.data, message.length) == 0) {
				while(ros3_write_status(&writer)) {
					while(!(UCSR0A & _BV(UDRE0))) {}
					UDR0 = ros3_write_byte(&writer);
				}
			}
			count = count == INT16_MAX ? 0 : count + 1;
		}
	}
}
