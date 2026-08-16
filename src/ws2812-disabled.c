#include <stdint.h>

#include "ws2812.h"

void ws2812_init(unsigned int pin) {
  (void)pin;
}

void ws2812_put_rgb(uint8_t red, uint8_t green, uint8_t blue) {
  (void)red;
  (void)green;
  (void)blue;
}

void ws2812_put_pixel(uint32_t pixel_grb) {
  (void)pixel_grb;
}
