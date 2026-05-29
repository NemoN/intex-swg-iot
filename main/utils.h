#ifndef utils_h
#define utils_h

#include <stdint.h>
#include "driver/gpio.h"

static inline void GPIO_Set(gpio_num_t pin) {
	gpio_set_level(pin, 1);
}

static inline void GPIO_Clear(gpio_num_t pin) {
	gpio_set_level(pin, 0);
}

static inline uint8_t GPIO_IN_Get(gpio_num_t pin) {
	return gpio_get_level(pin) ? 1 : 0;
}

static inline void pinMode(gpio_num_t pin, gpio_mode_t mode) {
	gpio_set_direction(pin, mode);
}

static inline uint8_t digitalRead(gpio_num_t pin) {
	return GPIO_IN_Get(pin);
}

#define INPUT GPIO_MODE_INPUT
#define OUTPUT GPIO_MODE_OUTPUT
#define LOW 0x00
#define HIGH 0x01

uint64_t millis();
uint64_t micros();
void delayMicroseconds(uint32_t us);
void digitalWrite(gpio_num_t pin, uint8_t state);
//void pinMode(gpio_num_t pin, gpio_mode_t mode);
//String checkLedStatus(byte statusDigit, byte ledMask);
//bool isDisplayBlinking(byte displayDigit);

#endif