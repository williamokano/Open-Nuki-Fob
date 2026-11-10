// M5Stack Atom Lite: button on GPIO39 (active low, external pull-up), SK6812 RGB LED on GPIO27.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void board_init(void);
bool board_button_pressed(void);
void board_led(uint8_t r, uint8_t g, uint8_t b);
