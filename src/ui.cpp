#include "ui.h"

#include <Arduino.h>
#include <lvgl.h>

#include <map>
#include <string>
#include <vector>

#include "link.h"
#include "model.h"

namespace {

enum class View : uint8_t { Master, List, App };

constexpr uint32_t kAccentMaster = 0x2D9CFF;
constexpr uint32_t kAccentApp = 0xFF9F0A;
constexpr uint32_t kArcTrack = 0x1C1C1E;
constexpr uint32_t kArcMuted = 0x48484A;
constexpr uint32_t kRed = 0xFF453A;
constexpr uint32_t kAmber = 0xFFD60A;
constexpr uint32_t kTextDim = 0x8E8E93;
constexpr uint32_t kBadgeSystem = 0x3A3A3C;

constexpr uint32_t kIdleTimeoutMs = 15000;
constexpr int kIconSize = 64;
constexpr int kItemSpacing = 84;
constexpr int kListIconY = -16;
constexpr int kAppIconY = -46;
constexpr uint32_t kAnimMs = 160;

View view = View::Master;
uint32_t lastInputMs = 0;
uint32_t lastRotateMs = 0;

lv_obj_t *viewMaster;
lv_obj_t *viewList;
lv_obj_t *viewApp;

lv_obj_t *mArc;
lv_obj_t *mValue;
lv_obj_t *mFooter;

lv_obj_t *lName;
lv_obj_t *lValue;
lv_obj_t *lIndex;
lv_obj_t *lEmpty;
std::vector<lv_obj_t *> lItems;
std::vector<int> lItemD;  // each item's slot relative to the selection (0 = centre, -1 = left, ...)
int listSel = 0;
int listSelId = -1;

lv_obj_t *aArc;
lv_obj_t *aValue;
lv_obj_t *aName;
lv_obj_t *aIcon = nullptr;
int appId = -1;

/*---------------------------------------------------------------- helpers */

void animXCb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }
// Carousel items are plain lv_image widgets scaled by the image's own transform. Style transforms
// (transform_scale on a container) render through an offscreen layer, and LVGL 9.3 stopped drawing
// every later sibling once such a layer sat fully left of the screen.
void animScaleCb(void *obj, int32_t v) { lv_image_set_scale((lv_obj_t *)obj, (uint32_t)v); }
void animOpaCb(void *obj, int32_t v) { lv_obj_set_style_image_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }
void animArcCb(void *obj, int32_t v) { lv_arc_set_value((lv_obj_t *)obj, v); }

void animate(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to) {
  if (from == to) {
    lv_anim_delete(obj, cb);
    cb(obj, to);
    return;
  }
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, kAnimMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

lv_obj_t *makeView(lv_obj_t *parent) {
  lv_obj_t *v = lv_obj_create(parent);
  lv_obj_remove_style_all(v);
  lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
  lv_obj_center(v);
  lv_obj_remove_flag(v, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  return v;
}

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int y, int width = 0) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  if (width > 0) {
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  }
  lv_label_set_text(l, "");
  lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
  return l;
}

lv_obj_t *makeArc(lv_obj_t *parent) {
  lv_obj_t *arc = lv_arc_create(parent);
  lv_obj_set_size(arc, 226, 226);
  lv_obj_center(arc);
  lv_arc_set_rotation(arc, 135);
  lv_arc_set_bg_angles(arc, 0, 270);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(arc, 0, 0);
  lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_hex(kArcTrack), LV_PART_MAIN);
  return arc;
}

void setArc(lv_obj_t *arc, int volume, bool muted, uint32_t accent, bool smooth) {
  lv_obj_set_style_arc_color(arc, lv_color_hex(muted ? kArcMuted : accent), LV_PART_INDICATOR);
  if (smooth) {
    animate(arc, animArcCb, lv_arc_get_value(arc), volume);
  } else {
    lv_anim_delete(arc, animArcCb);
    lv_arc_set_value(arc, volume);
  }
}

void setValueLabel(lv_obj_t *label, int volume, bool muted) {
  if (muted) {
    lv_label_set_text(label, LV_SYMBOL_MUTE);
    lv_obj_set_style_text_color(label, lv_color_hex(kRed), 0);
  } else {
    lv_label_set_text_fmt(label, "%d", volume);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
  }
}

lv_color_t badgeColor(const std::string &name) {
  uint32_t h = 2166136261u;
  for (char c : name) h = (h ^ (uint8_t)c) * 16777619u;
  return lv_color_hsv_to_rgb((uint16_t)(h % 360), 55, 70);
}

/*
 * Badges for apps without an icon (letter in a coloured circle, or a speaker for System Sounds),
 * drawn once into an ARGB8888 buffer so they are ordinary images like real icons.
 */
std::map<std::string, lv_draw_buf_t *> badges;

std::string badgeKey(const AppInfo &app) { return app.iconKey == kIconSystem ? std::string("\x01system") : app.name; }

const lv_draw_buf_t *badgeFor(const AppInfo &app) {
  std::string key = badgeKey(app);
  auto it = badges.find(key);
  if (it != badges.end()) return it->second;

  lv_draw_buf_t *buf = lv_draw_buf_create(kIconSize, kIconSize, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
  if (!buf) return nullptr;
  lv_draw_buf_clear(buf, nullptr);

  lv_obj_t *canvas = lv_canvas_create(lv_layer_sys());
  lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
  lv_canvas_set_draw_buf(canvas, buf);
  lv_layer_t layer;
  lv_canvas_init_layer(canvas, &layer);

  lv_draw_rect_dsc_t rect;
  lv_draw_rect_dsc_init(&rect);
  rect.radius = LV_RADIUS_CIRCLE;
  rect.bg_opa = LV_OPA_COVER;
  rect.bg_color = app.iconKey == kIconSystem ? lv_color_hex(kBadgeSystem) : badgeColor(app.name);
  lv_area_t full = {0, 0, kIconSize - 1, kIconSize - 1};
  lv_draw_rect(&layer, &rect, &full);

  char letter[2] = {app.name.empty() ? '?' : (char)toupper((unsigned char)app.name[0]), '\0'};
  lv_draw_label_dsc_t label;
  lv_draw_label_dsc_init(&label);
  label.font = &lv_font_montserrat_28;
  label.color = lv_color_white();
  label.align = LV_TEXT_ALIGN_CENTER;
  label.text = app.iconKey == kIconSystem ? LV_SYMBOL_VOLUME_MAX : letter;
  int32_t lineH = lv_font_get_line_height(label.font);
  lv_area_t textArea = {0, (kIconSize - lineH) / 2, kIconSize - 1, (kIconSize - lineH) / 2 + lineH - 1};
  lv_draw_label(&layer, &label, &textArea);

  lv_canvas_finish_layer(canvas, &layer);
  lv_obj_delete(canvas);

  badges[key] = buf;
  return buf;
}

/** Free badges no app needs any more. Call only after the views were rebuilt. */
void badgeGarbageCollect() {
  for (auto it = badges.begin(); it != badges.end();) {
    bool used = false;
    for (const AppInfo &app : g_model.apps) {
      if (!iconGet(app.iconKey) && badgeKey(app) == it->first) {
        used = true;
        break;
      }
    }
    if (used) {
      ++it;
      continue;
    }
    lv_image_cache_drop(it->second);
    lv_draw_buf_destroy(it->second);
    it = badges.erase(it);
  }
}

/** App icon (cached image) or its badge as a plain image, kIconSize square, scaled around its center. */
lv_obj_t *makeIcon(lv_obj_t *parent, const AppInfo &app) {
  lv_obj_t *img = lv_image_create(parent);
  lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
  if (const lv_image_dsc_t *dsc = iconGet(app.iconKey)) {
    lv_image_set_src(img, dsc);
  } else if (const lv_draw_buf_t *badge = badgeFor(app)) {
    lv_image_set_src(img, badge);
  }
  lv_obj_set_size(img, kIconSize, kIconSize);
  lv_image_set_pivot(img, kIconSize / 2, kIconSize / 2);
  return img;
}

int stepFor(int32_t detents) {
  uint32_t now = millis();
  uint32_t dt = now - lastRotateMs;
  lastRotateMs = now;
  int perDetent = (dt < 45 || detents > 1 || detents < -1) ? 4 : 2;  // accelerate fast turns
  return (int)detents * perDetent;
}

/*---------------------------------------------------------------- master view */

void refreshMaster(bool smooth) {
  const Model &m = g_model;
  setArc(mArc, m.masterVolume, m.masterMuted || m.masterFixed, kAccentMaster, smooth);
  setValueLabel(mValue, m.masterVolume, m.masterMuted);
  if (m.online && m.masterFixed) {
    // The output device ignores Windows volume (e.g. headsets with a hardware dial).
    lv_label_set_text(mFooter, "Fixed by device");
    lv_obj_set_style_text_color(mFooter, lv_color_hex(kAmber), 0);
    return;
  }
  if (!m.online) {
    lv_label_set_text(mFooter, "PC offline");
    lv_obj_set_style_text_color(mFooter, lv_color_hex(kRed), 0);
  } else {
    lv_label_set_text(mFooter, m.outputName.c_str());
    lv_obj_set_style_text_color(mFooter, lv_color_hex(kTextDim), 0);
  }
}

/*---------------------------------------------------------------- list view */

void clearList() {
  for (lv_obj_t *item : lItems) lv_obj_delete(item);
  lItems.clear();
  lItemD.clear();
}

/**
 * Slot of item i on the circular carousel: the offset from the selection, wrapped into
 * [-n/2, n/2]. When two slots are equally far (even n), keep the one that continues the
 * item's movement so it does not jump across the screen.
 */
int wrappedSlot(int i, int n, int expected) {
  int d = ((i - listSel) % n + n) % n;
  if (d > n / 2) d -= n;
  if (n % 2 == 0 && abs(d) == n / 2 && expected == -d) d = -d;
  return d;
}

/** Place the carousel items. `moved` is how far the selection just moved (0 = rebuild, no wrap). */
void layoutList(bool smooth, int moved = 0) {
  const int n = (int)lItems.size();
  for (int i = 0; i < n; i++) {
    lv_obj_t *item = lItems[i];
    int expected = lItemD[i] - moved;
    int d = wrappedSlot(i, n, expected);
    // An item that went around the end enters from the far side instead of sliding across.
    bool wrapped = smooth && moved != 0 && d != expected;
    lItemD[i] = d;
    int ad = abs(d);
    int x = d * kItemSpacing;
    int scale = ad == 0 ? 256 : (ad == 1 ? 150 : 90);
    int opa = ad == 0 ? 255 : (ad == 1 ? 140 : 0);
    if (wrapped && ad <= 2) {
      lv_anim_delete(item, nullptr);
      lv_obj_set_x(item, x + (moved > 0 ? kItemSpacing : -kItemSpacing));
      lv_image_set_scale(item, 90);
      lv_obj_set_style_image_opa(item, 0, 0);
    }
    if (ad > 2) {
      lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);
      lv_anim_delete(item, nullptr);
      lv_obj_set_x(item, x);
      lv_image_set_scale(item, scale);
      lv_obj_set_style_image_opa(item, 0, 0);
      continue;
    }
    lv_obj_remove_flag(item, LV_OBJ_FLAG_HIDDEN);
    if (smooth) {
      animate(item, animXCb, lv_obj_get_style_x(item, 0), x);
      animate(item, animScaleCb, (int32_t)lv_image_get_scale(item), scale);
      animate(item, animOpaCb, lv_obj_get_style_image_opa(item, 0), opa);
    } else {
      lv_obj_set_x(item, x);
      lv_image_set_scale(item, scale);
      lv_obj_set_style_image_opa(item, opa, 0);
    }
  }
}

void refreshListLabels() {
  const auto &apps = g_model.apps;
  if (apps.empty()) {
    lv_obj_remove_flag(lEmpty, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lEmpty, g_model.online ? "No apps\nplaying audio" : "PC offline");
    lv_label_set_text(lName, "");
    lv_label_set_text(lValue, "");
    lv_label_set_text(lIndex, "");
    return;
  }
  lv_obj_add_flag(lEmpty, LV_OBJ_FLAG_HIDDEN);
  const AppInfo &app = apps[listSel];
  lv_label_set_text(lName, app.name.c_str());
  if (app.muted) {
    lv_label_set_text(lValue, LV_SYMBOL_MUTE " Muted");
    lv_obj_set_style_text_color(lValue, lv_color_hex(kRed), 0);
  } else {
    lv_label_set_text_fmt(lValue, "%d%%", app.volume);
    lv_obj_set_style_text_color(lValue, lv_color_hex(kAccentApp), 0);
  }
  lv_label_set_text_fmt(lIndex, "%d / %d", listSel + 1, (int)apps.size());
}

void buildList() {
  clearList();
  const auto &apps = g_model.apps;
  int idx = modelIndexOf(listSelId);
  if (idx >= 0) {
    listSel = idx;
  } else {
    listSel = apps.empty() ? 0 : clampInt(listSel, 0, (int)apps.size() - 1);
  }
  listSelId = apps.empty() ? -1 : apps[listSel].id;

  for (const AppInfo &app : apps) {
    lv_obj_t *item = makeIcon(viewList, app);
    lv_obj_align(item, LV_ALIGN_CENTER, 0, kListIconY);
    lItems.push_back(item);
    lItemD.push_back(0);
  }
  layoutList(false);
  refreshListLabels();
}

/*---------------------------------------------------------------- app view */

void showView(View v);

void refreshApp(bool smooth) {
  const AppInfo *app = modelFindApp(appId);
  if (!app) return;
  setArc(aArc, app->volume, app->muted, kAccentApp, smooth);
  setValueLabel(aValue, app->volume, app->muted);
}

void buildApp() {
  if (aIcon) {
    lv_obj_delete(aIcon);
    aIcon = nullptr;
  }
  const AppInfo *app = modelFindApp(appId);
  if (!app) {
    showView(View::List);  // the app closed while it was open
    return;
  }
  aIcon = makeIcon(viewApp, *app);
  lv_obj_align(aIcon, LV_ALIGN_CENTER, 0, kAppIconY);
  lv_label_set_text(aName, app->name.c_str());
  refreshApp(false);
}

/*---------------------------------------------------------------- navigation */

/**
 * Where hold and the idle timeout go. When Windows cannot change the output device's volume,
 * the master screen has nothing to offer, so the app list takes its place.
 */
View homeView() { return g_model.online && g_model.masterFixed ? View::List : View::Master; }

void showView(View v) {
  if (view == View::List && v != View::List) clearList();
  if (view == View::App && v != View::App && aIcon) {
    lv_obj_delete(aIcon);
    aIcon = nullptr;
  }
  view = v;
  lv_obj_set_flag(viewMaster, LV_OBJ_FLAG_HIDDEN, v != View::Master);
  lv_obj_set_flag(viewList, LV_OBJ_FLAG_HIDDEN, v != View::List);
  lv_obj_set_flag(viewApp, LV_OBJ_FLAG_HIDDEN, v != View::App);

  switch (v) {
    case View::Master:
      refreshMaster(false);
      break;
    case View::List:
      buildList();
      break;
    case View::App:
      buildApp();
      break;
  }
  // Views were rebuilt without widgets for removed apps, so their icons can go.
  iconGarbageCollect();
  badgeGarbageCollect();
}

}  // namespace

/*---------------------------------------------------------------- public API */

void uiInit() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Master
  viewMaster = makeView(scr);
  mArc = makeArc(viewMaster);
  lv_obj_t *caption = makeLabel(viewMaster, &lv_font_montserrat_20, kTextDim, -50);
  lv_label_set_text(caption, LV_SYMBOL_VOLUME_MAX);
  mValue = makeLabel(viewMaster, &lv_font_montserrat_48, 0xFFFFFF, 2);
  mFooter = makeLabel(viewMaster, &lv_font_montserrat_14, kTextDim, 50, 150);

  // List
  viewList = makeView(scr);
  lv_obj_t *title = makeLabel(viewList, &lv_font_montserrat_14, kTextDim, -88);
  lv_label_set_text(title, "APPS");
  lName = makeLabel(viewList, &lv_font_montserrat_20, 0xFFFFFF, 38, 180);
  lValue = makeLabel(viewList, &lv_font_montserrat_16, kAccentApp, 64);
  lIndex = makeLabel(viewList, &lv_font_montserrat_14, kTextDim, 92);
  lEmpty = makeLabel(viewList, &lv_font_montserrat_20, kTextDim, 0);

  // App
  viewApp = makeView(scr);
  aArc = makeArc(viewApp);
  aValue = makeLabel(viewApp, &lv_font_montserrat_48, 0xFFFFFF, 20);
  aName = makeLabel(viewApp, &lv_font_montserrat_16, kTextDim, 62, 140);

  view = View::App;  // force showView to apply all flags
  showView(View::Master);
  g_model.structureDirty = false;
  g_model.valuesDirty = false;
}

void uiUpdate() {
  Model &m = g_model;
  if (m.structureDirty) {
    m.structureDirty = false;
    m.valuesDirty = false;
    switch (view) {
      case View::Master:
        refreshMaster(true);
        break;
      case View::List:
        buildList();
        break;
      case View::App:
        buildApp();
        break;
    }
    iconGarbageCollect();
    badgeGarbageCollect();
  } else if (m.valuesDirty) {
    m.valuesDirty = false;
    switch (view) {
      case View::Master:
        refreshMaster(true);
        break;
      case View::List:
        refreshListLabels();
        break;
      case View::App:
        refreshApp(true);
        break;
    }
  }

  View home = homeView();
  if (view == View::Master && home == View::List) {
    showView(View::List);  // the output device just became fixed-volume (or the PC told us at sync)
  } else if (view != home && millis() - lastInputMs > kIdleTimeoutMs) {
    showView(home);
  }
}

void uiRotate(int32_t detents) {
  if (detents == 0) return;
  lastInputMs = millis();
  Model &m = g_model;

  switch (view) {
    case View::Master: {
      if (m.masterFixed) break;  // Windows cannot change this device's volume
      int vol = clampInt(m.masterVolume + stepFor(detents), 0, 100);
      m.masterVolume = vol;
      m.masterMuted = false;  // like the Windows volume keys
      m.masterLocalChangeMs = millis();
      linkSendMaster(vol, false);
      refreshMaster(true);
      break;
    }
    case View::List: {
      const int n = (int)m.apps.size();
      if (n < 2) break;
      // Circular: past the last app comes the first, and back from the first comes the last.
      int moved = clampInt((int)detents, -(n - 1), n - 1);
      listSel = ((listSel + moved) % n + n) % n;
      listSelId = m.apps[listSel].id;
      layoutList(true, moved);
      refreshListLabels();
      break;
    }
    case View::App: {
      AppInfo *app = modelFindApp(appId);
      if (!app) break;
      app->volume = clampInt(app->volume + stepFor(detents), 0, 100);
      app->muted = false;
      app->localChangeMs = millis();
      linkSendApp(app->id, app->volume, false);
      refreshApp(true);
      break;
    }
  }
}

void uiClick() {
  lastInputMs = millis();
  switch (view) {
    case View::Master:
      showView(View::List);
      break;
    case View::List:
      if (!g_model.apps.empty()) {
        appId = g_model.apps[listSel].id;
        showView(View::App);
      }
      break;
    case View::App:
      showView(View::List);
      break;
  }
}

void uiDoubleClick() {
  lastInputMs = millis();
  Model &m = g_model;
  if (view == View::Master) {
    m.masterMuted = !m.masterMuted;
    m.masterLocalChangeMs = millis();
    linkSendMaster(m.masterVolume, m.masterMuted);
    refreshMaster(true);
  } else if (view == View::App) {
    AppInfo *app = modelFindApp(appId);
    if (!app) return;
    app->muted = !app->muted;
    app->localChangeMs = millis();
    linkSendApp(app->id, app->volume, app->muted);
    refreshApp(true);
  }
}

void uiLongPress() {
  lastInputMs = millis();
  if (view != homeView()) showView(homeView());
}

bool uiWantsDoubleClick() { return view != View::List; }
