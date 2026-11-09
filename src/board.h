#pragma once

#include <stdint.h>

/**
 * Hardware layer for the CrowPanel 1.28" rotary display:
 * GC9A01 LCD over SPI (LovyanGFX), backlight PWM, rotary encoder and its push button.
 */
namespace board {

constexpr int kScreenWidth = 240;
constexpr int kScreenHeight = 240;

enum class Button : uint8_t { None, Click, DoubleClick, LongPress };

void init();
void setBacklight(uint8_t level);

/** Push RGB565 pixels that are already byte-swapped (big endian) to the LCD via DMA. */
void lcdPush(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *pixels);

/** Encoder detents accumulated since the last call (positive = clockwise). */
int32_t takeDetents();

/**
 * Poll the encoder push button. Call every loop iteration.
 * When doubleClickEnabled is false a single click is reported immediately on release;
 * otherwise it is held back for the double-click window.
 */
Button pollButton(bool doubleClickEnabled);

}  // namespace board
