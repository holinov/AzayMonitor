#include <Arduino.h>
#include <util/delay.h>

// Define LED pin - using PB0 (physical pin 12 on ATtiny2313A)
#define LED_PIN 0  // PB0 is bit 0

void setup() {
  // Set LED pin as output
  DDRB |= (1 << LED_PIN);
  PORTB &= ~(1 << LED_PIN);  // Start with LED off
}

void loop() {
  // Toggle LED
  PORTB ^= (1 << LED_PIN);
  _delay_ms(500);  // Wait for 0.5 second
  PORTB ^= (1 << LED_PIN);
  _delay_ms(500);  // Wait for 0.5 second
}