#pragma once

#include <lvgl.h>
#include <stdint.h>
#include <string>
#include <vector>

/** Icon keys with a special meaning (see docs/protocol.md). */
constexpr uint32_t kIconNone = 0;
constexpr uint32_t kIconSystem = 1;

/** Arduino's constrain() is a macro that evaluates its argument repeatedly; use this instead. */
inline int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct AppInfo {
  int id = 0;
  uint32_t iconKey = kIconNone;
  std::string name;
  int volume = 0;
  bool muted = false;
  uint32_t localChangeMs = 0;  // last time the user changed this app on the knob
  bool stale = false;          // used during a full sync
};

/** Mirror of the PC's volume mixer. Only touched from the Arduino loop task. */
struct Model {
  int masterVolume = 0;
  bool masterMuted = false;
  bool masterFixed = false;  // output device ignores Windows volume (hardware-controlled)
  uint32_t masterLocalChangeMs = 0;
  std::string outputName;
  bool online = false;

  std::vector<AppInfo> apps;  // in PC mixer order

  bool structureDirty = true;  // app added/removed/renamed or an icon arrived
  bool valuesDirty = true;     // volumes, mute, online state, output name
};

extern Model g_model;

AppInfo *modelFindApp(int id);
int modelIndexOf(int id);

/** Returns true when the app's icon is neither cached nor already requested; it is then marked requested. */
bool modelUpsertApp(int id, uint32_t iconKey, int volume, bool muted, const char *name);
void modelRemoveApp(int id);
/** Marks all apps stale and forgets outstanding icon requests (they are re-requested). */
void modelSyncBegin();
void modelSyncEnd();

/** Icon cache (PSRAM). */
const lv_image_dsc_t *iconGet(uint32_t key);
bool iconWanted(uint32_t key);
void iconRequestFinished(uint32_t key);
/** Takes ownership of `pixels` (heap_caps_malloc'd, w*h*4 bytes ARGB8888). */
void iconStore(uint32_t key, int w, int h, uint8_t *pixels);
/** Free icons no app references. Call only after the UI dropped widgets of removed apps. */
void iconGarbageCollect();
size_t iconCount();
