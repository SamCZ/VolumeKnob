#pragma once

#include <stdint.h>

/**
 * Three views on one LVGL screen:
 *  - Master:  rest state, master volume arc. Rotate = volume, click = app list,
 *             double click = mute.
 *  - List:    carousel of mixer apps. Rotate = select, click = open app,
 *             hold = back to master.
 *  - App:     one app's volume. Rotate = volume, double click = mute,
 *             click = back to list, hold = back to master.
 * Any non-master view returns to master after a period without input.
 */
void uiInit();
/** Apply model changes to the widgets. Call every loop iteration before lv_timer_handler(). */
void uiUpdate();

void uiRotate(int32_t detents);
void uiClick();
void uiDoubleClick();
void uiLongPress();
/** Whether the current view uses double click (single clicks are then delayed). */
bool uiWantsDoubleClick();
