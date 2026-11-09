#include "board.h"

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <LovyanGFX.hpp>

namespace board {
namespace {

// Pin map (from Elecrow's factory firmware).
constexpr int kPinRail1 = 1;  // must be high while the display runs
constexpr int kPinRail2 = 2;
constexpr int kPinPowerLed = 40;
constexpr int kPinRgb = 48;
constexpr int kRgbCount = 5;
constexpr int kPinEncA = 45;
constexpr int kPinEncB = 42;
constexpr int kPinEncSwitch = 41;
constexpr int kPinBacklight = 46;

// Mounting: the 45-degree stand holds the knob upside down relative to the factory
// orientation, so the panel is rotated 180 degrees. (The wheel needs no change: turning the
// whole knob over keeps clockwise clockwise.)
constexpr bool kFlip180 = true;

constexpr uint32_t kDebounceMs = 15;
constexpr uint32_t kDoubleClickMs = 280;
constexpr uint32_t kLongPressMs = 650;

class Lcd : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 panel_;
  lgfx::Bus_SPI bus_;

 public:
  Lcd() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 80000000;
      cfg.freq_read = 20000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 10;
      cfg.pin_mosi = 11;
      cfg.pin_miso = -1;
      cfg.pin_dc = 3;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = 9;
      cfg.pin_rst = 14;
      cfg.pin_busy = -1;
      cfg.memory_width = kScreenWidth;
      cfg.memory_height = kScreenHeight;
      cfg.panel_width = kScreenWidth;
      cfg.panel_height = kScreenHeight;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = kFlip180 ? 2 : 0;  // 2 = 180 degrees, done by the panel (MADCTL)
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }
    setPanel(&panel_);
  }
};

Lcd lcd;
Adafruit_NeoPixel rgb(kRgbCount, kPinRgb, NEO_GRB + NEO_KHZ800);

// Quadrature decoder: state transition table indexed by (prev << 2) | current,
// with A = bit1 and B = bit0. Four valid transitions make one detent.
DRAM_ATTR const int8_t kEncTable[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};
portMUX_TYPE encMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t encState = 0;
volatile int8_t encQuarter = 0;
volatile int32_t encDetents = 0;

void IRAM_ATTR encoderIsr() {
  uint8_t current = (uint8_t)((digitalRead(kPinEncA) << 1) | digitalRead(kPinEncB));
  portENTER_CRITICAL_ISR(&encMux);
  int8_t move = kEncTable[(encState << 2) | current];
  encState = current;
  if (move != 0) {
    int8_t q = encQuarter + move;
    if (q >= 4) {
      encDetents = encDetents + 1;
      q -= 4;
    } else if (q <= -4) {
      encDetents = encDetents - 1;
      q += 4;
    }
    encQuarter = q;
  }
  portEXIT_CRITICAL_ISR(&encMux);
}

}  // namespace

void init() {
  pinMode(kPinPowerLed, OUTPUT);
  digitalWrite(kPinPowerLed, LOW);
  pinMode(kPinRail1, OUTPUT);
  digitalWrite(kPinRail1, HIGH);
  pinMode(kPinRail2, OUTPUT);
  digitalWrite(kPinRail2, HIGH);

  ledcAttach(kPinBacklight, 5000, 8);
  ledcWrite(kPinBacklight, 0);

  rgb.begin();
  rgb.clear();
  rgb.show();

  lcd.init();
  lcd.initDMA();
  lcd.fillScreen(TFT_BLACK);
  // Keep the SPI transaction open so pushImageDMA() returns without waiting.
  lcd.startWrite();

  pinMode(kPinEncA, INPUT);
  pinMode(kPinEncB, INPUT);
  pinMode(kPinEncSwitch, INPUT_PULLUP);
  encState = (uint8_t)((digitalRead(kPinEncA) << 1) | digitalRead(kPinEncB));
  attachInterrupt(digitalPinToInterrupt(kPinEncA), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(kPinEncB), encoderIsr, CHANGE);
}

void setBacklight(uint8_t level) { ledcWrite(kPinBacklight, level); }

void lcdPush(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *pixels) {
  // Wait for the previous transfer: LVGL is double buffered, so the buffer we are
  // about to hand out was the one being sent two flushes ago.
  lcd.waitDMA();
  lcd.pushImageDMA(x, y, w, h, reinterpret_cast<const lgfx::swap565_t *>(pixels));
}

int32_t takeDetents() {
  portENTER_CRITICAL(&encMux);
  int32_t d = encDetents;
  encDetents = 0;
  portEXIT_CRITICAL(&encMux);
  return d;
}

Button pollButton(bool doubleClickEnabled) {
  static bool lastRaw = false;
  static bool pressed = false;
  static uint32_t rawChangedMs = 0;
  static uint32_t pressMs = 0;
  static uint32_t releaseMs = 0;
  static bool longFired = false;
  static uint8_t clicks = 0;

  const uint32_t now = millis();
  const bool raw = digitalRead(kPinEncSwitch) == LOW;
  if (raw != lastRaw) {
    lastRaw = raw;
    rawChangedMs = now;
  }

  if (raw != pressed && now - rawChangedMs >= kDebounceMs) {
    pressed = raw;
    if (pressed) {
      pressMs = now;
      longFired = false;
    } else if (!longFired) {
      if (!doubleClickEnabled) {
        clicks = 0;
        return Button::Click;
      }
      if (++clicks >= 2) {
        clicks = 0;
        return Button::DoubleClick;
      }
      releaseMs = now;
    }
  }

  if (pressed && !longFired && now - pressMs >= kLongPressMs) {
    longFired = true;
    clicks = 0;
    return Button::LongPress;
  }

  if (!pressed && clicks == 1 && now - releaseMs > kDoubleClickMs) {
    clicks = 0;
    return Button::Click;
  }
  return Button::None;
}

}  // namespace board
