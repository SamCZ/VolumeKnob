#include "model.h"

#include <esp_heap_caps.h>
#include <string.h>

#include <map>
#include <set>

Model g_model;

namespace {

struct IconEntry {
  lv_image_dsc_t dsc;
  uint8_t *pixels;
};

// std::map nodes never move, so &entry.dsc stays valid while LVGL references it.
std::map<uint32_t, IconEntry> icons;
std::set<uint32_t> requested;  // asked the PC for these, not arrived yet

bool keyNeedsData(uint32_t key) { return key != kIconNone && key != kIconSystem; }

bool keyInUse(uint32_t key) {
  for (const AppInfo &app : g_model.apps) {
    if (app.iconKey == key) return true;
  }
  return false;
}

}  // namespace

AppInfo *modelFindApp(int id) {
  for (AppInfo &app : g_model.apps) {
    if (app.id == id) return &app;
  }
  return nullptr;
}

int modelIndexOf(int id) {
  for (size_t i = 0; i < g_model.apps.size(); i++) {
    if (g_model.apps[i].id == id) return (int)i;
  }
  return -1;
}

bool modelUpsertApp(int id, uint32_t iconKey, int volume, bool muted, const char *name) {
  AppInfo *app = modelFindApp(id);
  if (!app) {
    g_model.apps.emplace_back();
    app = &g_model.apps.back();
    app->id = id;
    g_model.structureDirty = true;
  }
  app->stale = false;
  if (app->iconKey != iconKey || app->name != name) {
    app->iconKey = iconKey;
    app->name = name;
    g_model.structureDirty = true;
  }
  if (app->volume != volume || app->muted != muted) {
    app->volume = volume;
    app->muted = muted;
    g_model.valuesDirty = true;
  }
  if (!keyNeedsData(iconKey) || icons.count(iconKey) || requested.count(iconKey)) return false;
  requested.insert(iconKey);
  return true;
}

void modelRemoveApp(int id) {
  int index = modelIndexOf(id);
  if (index < 0) return;
  g_model.apps.erase(g_model.apps.begin() + index);
  g_model.structureDirty = true;
}

void modelSyncBegin() {
  for (AppInfo &app : g_model.apps) app.stale = true;
  requested.clear();
}

void modelSyncEnd() {
  size_t before = g_model.apps.size();
  std::vector<AppInfo> kept;
  kept.reserve(before);
  for (AppInfo &app : g_model.apps) {
    if (!app.stale) kept.push_back(std::move(app));
  }
  g_model.apps.swap(kept);
  if (g_model.apps.size() != before) g_model.structureDirty = true;
}

const lv_image_dsc_t *iconGet(uint32_t key) {
  auto it = icons.find(key);
  return it == icons.end() ? nullptr : &it->second.dsc;
}

bool iconWanted(uint32_t key) { return keyNeedsData(key) && keyInUse(key) && icons.find(key) == icons.end(); }

void iconRequestFinished(uint32_t key) { requested.erase(key); }

void iconStore(uint32_t key, int w, int h, uint8_t *pixels) {
  if (!iconWanted(key)) {
    heap_caps_free(pixels);
    return;
  }
  IconEntry &entry = icons[key];
  memset(&entry.dsc, 0, sizeof(entry.dsc));
  entry.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  entry.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
  entry.dsc.header.w = w;
  entry.dsc.header.h = h;
  entry.dsc.header.stride = w * 4;
  entry.dsc.data_size = (uint32_t)(w * h * 4);
  entry.dsc.data = pixels;
  entry.pixels = pixels;
  g_model.structureDirty = true;  // swap placeholder badges for the real icon
}

void iconGarbageCollect() {
  for (auto it = icons.begin(); it != icons.end();) {
    if (keyInUse(it->first)) {
      ++it;
      continue;
    }
    lv_image_cache_drop(&it->second.dsc);
    heap_caps_free(it->second.pixels);
    it = icons.erase(it);
  }
}

size_t iconCount() { return icons.size(); }
