/**
 * Volume Knob firmware: mirrors the Windows volume mixer (master + per-app volume)
 * received from the companion app over USB serial. See docs/protocol.md.
 */
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "board.h"
#include "link.h"
#include "ui.h"

namespace {

constexpr int kBufferLines = 48;

void flushCb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
  const uint32_t w = lv_area_get_width(area);
  const uint32_t h = lv_area_get_height(area);
  lv_draw_sw_rgb565_swap(pixels, w * h);  // the panel wants big-endian RGB565
  board::lcdPush(area->x1, area->y1, w, h, reinterpret_cast<const uint16_t *>(pixels));
  // Safe to release now: LVGL renders the next area into the other buffer, and
  // lcdPush() waits for this transfer before reusing it.
  lv_display_flush_ready(display);
}

uint32_t tickCb() { return millis(); }

}  // namespace

void setup() {
  board::init();
  linkInit();

  lv_init();
  lv_tick_set_cb(tickCb);

  const size_t bytes = board::kScreenWidth * kBufferLines * sizeof(uint16_t);
  void *buf1 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  void *buf2 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

  lv_display_t *display = lv_display_create(board::kScreenWidth, board::kScreenHeight);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(display, flushCb);
  lv_display_set_buffers(display, buf1, buf2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

  uiInit();
  lv_timer_handler();
  board::setBacklight(220);
}

void loop() {
  linkPoll();

  int32_t detents = board::takeDetents();
  if (detents != 0) uiRotate(detents);

  switch (board::pollButton(uiWantsDoubleClick())) {
    case board::Button::Click:
      uiClick();
      break;
    case board::Button::DoubleClick:
      uiDoubleClick();
      break;
    case board::Button::LongPress:
      uiLongPress();
      break;
    case board::Button::None:
      break;
  }

  uiUpdate();
  lv_timer_handler();
  delay(1);
}
