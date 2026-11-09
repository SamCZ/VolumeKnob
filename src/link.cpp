#include "link.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"
#include "ui.h"

namespace {

constexpr size_t kLineMax = 32 * 1024;      // fits a 64x64 ARGB8888 icon in base64
constexpr uint32_t kOfflineAfterMs = 3500;  // companion pings every second
constexpr uint32_t kEchoWindowMs = 400;     // ignore PC values right after a local change
constexpr int kMaxIconSize = 96;
constexpr uint32_t kBeaconMs = 1000;  // announce interval while no companion is talking to us

char *line = nullptr;
size_t lineLen = 0;
bool lineOverflow = false;

uint32_t lastRxMs = 0;
bool sawCompanion = false;
bool companionLeft = false;
uint32_t refreshDueMs = 0;  // when to ask the PC to resend values we ignored
bool refreshPending = false;
uint32_t lastBeaconMs = 0;

// Chip MAC, formatted like the USB serial number Windows shows for this port (e.g. CC:68:C7:8C:3C:3C).
char deviceId[18] = "";

void sendf(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
  Serial.write(reinterpret_cast<const uint8_t *>(buf), n);
}

bool inEchoWindow(uint32_t changeMs) {
  if (changeMs == 0) return false;
  uint32_t now = millis();
  if (now - changeMs < kEchoWindowMs) {
    refreshPending = true;
    refreshDueMs = changeMs + kEchoWindowMs;
    return true;
  }
  return false;
}

char *skipSpace(char *p) {
  while (*p == ' ') p++;
  return p;
}

void handleIconData(uint32_t key, char *end);

/** Icon line; always acknowledged with `k <key>` so the PC sends the next one only after this was consumed. */
void handleIcon(char *p) {
  char *end;
  uint32_t key = strtoul(p, &end, 16);
  handleIconData(key, end);
  iconRequestFinished(key);  // even on a decode error, so a later sync can ask again
  sendf("k %08lx\n", (unsigned long)key);
}

void handleIconData(uint32_t key, char *end) {
  int w = strtol(end, &end, 10);
  int h = strtol(end, &end, 10);
  end = skipSpace(end);
  size_t b64Len = strlen(end);
  if (w <= 0 || h <= 0 || w > kMaxIconSize || h > kMaxIconSize) return;
  if (!iconWanted(key)) return;

  size_t bytes = (size_t)w * h * 4;
  uint8_t *pixels = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!pixels) return;
  size_t decoded = 0;
  int rc = mbedtls_base64_decode(pixels, bytes, &decoded, reinterpret_cast<const unsigned char *>(end), b64Len);
  if (rc != 0 || decoded != bytes) {
    heap_caps_free(pixels);
    return;
  }
  iconStore(key, w, h, pixels);
}

void dumpState() {
  sendf("#online %d master %d mute %d apps %u icons %u\n", g_model.online, g_model.masterVolume,
        g_model.masterMuted, (unsigned)g_model.apps.size(), (unsigned)iconCount());
  sendf("#heap internal %u psram %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  for (const AppInfo &app : g_model.apps) {
    sendf("#app %d key %08lx cached %d vol %d mute %d %s\n", app.id, (unsigned long)app.iconKey,
          iconGet(app.iconKey) != nullptr, app.volume, app.muted, app.name.c_str());
  }
}

/** Debug: stream the current screen as base64 RGB565 rows ("#shot w h", "#row y data", "#endshot"). */
void sendScreenshot() {
  lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
  if (!snap) {
    sendf("#shot failed\n");
    return;
  }
  const uint32_t w = snap->header.w, h = snap->header.h, stride = snap->header.stride;
  Serial.setTxTimeoutMs(500);
  sendf("#shot %u %u\n", (unsigned)w, (unsigned)h);
  static char b64[1024];
  for (uint32_t y = 0; y < h; y++) {
    size_t outLen = 0;
    mbedtls_base64_encode(reinterpret_cast<unsigned char *>(b64), sizeof(b64), &outLen, snap->data + y * stride, w * 2);
    sendf("#row %u ", (unsigned)y);
    Serial.write(reinterpret_cast<const uint8_t *>(b64), outLen);
    Serial.write('\n');
  }
  sendf("#endshot\n");
  Serial.setTxTimeoutMs(20);
  lv_draw_buf_destroy(snap);
}

/** Debug: simulated knob input, "J r <detents>", "J c", "J d", "J l". */
void injectInput(char *p) {
  p = skipSpace(p);
  switch (*p) {
    case 'r':
      uiRotate(strtol(p + 1, nullptr, 10));
      break;
    case 'c':
      uiClick();
      break;
    case 'd':
      uiDoubleClick();
      break;
    case 'l':
      uiLongPress();
      break;
  }
}

void handleLine(char *s, size_t len) {
  if (len == 0) return;
  char *p = s + 1;
  char *end;

  switch (s[0]) {
    case '?':
      sendf("!VOLKNOB 1 %s\n", deviceId);
      break;
    case 'P':
      break;
    case 'X':
      companionLeft = true;
      break;
    case 'S':
      modelSyncBegin();
      break;
    case 'E':
      modelSyncEnd();
      break;
    case 'O': {
      std::string name(skipSpace(p));
      if (name != g_model.outputName) {
        g_model.outputName = name;
        g_model.valuesDirty = true;
      }
      break;
    }
    case 'G': {
      int vol = clampInt(strtol(p, &end, 10), 0, 100);
      bool mute = strtol(end, &end, 10) != 0;
      bool fixed = strtol(end, &end, 10) != 0;
      if (fixed != g_model.masterFixed) {
        g_model.masterFixed = fixed;
        g_model.valuesDirty = true;
      }
      if (inEchoWindow(g_model.masterLocalChangeMs)) break;
      if (vol != g_model.masterVolume || mute != g_model.masterMuted) {
        g_model.masterVolume = vol;
        g_model.masterMuted = mute;
        g_model.valuesDirty = true;
      }
      break;
    }
    case 'A': {
      int id = strtol(p, &end, 10);
      uint32_t key = strtoul(end, &end, 16);
      int vol = clampInt(strtol(end, &end, 10), 0, 100);
      bool mute = strtol(end, &end, 10) != 0;
      const char *name = skipSpace(end);
      AppInfo *existing = modelFindApp(id);
      if (existing && inEchoWindow(existing->localChangeMs)) {
        vol = existing->volume;
        mute = existing->muted;
      }
      if (modelUpsertApp(id, key, vol, mute, name)) {
        sendf("q %08lx\n", (unsigned long)key);
      }
      break;
    }
    case 'V': {
      int id = strtol(p, &end, 10);
      int vol = clampInt(strtol(end, &end, 10), 0, 100);
      bool mute = strtol(end, &end, 10) != 0;
      AppInfo *app = modelFindApp(id);
      if (!app || inEchoWindow(app->localChangeMs)) break;
      if (app->volume != vol || app->muted != mute) {
        app->volume = vol;
        app->muted = mute;
        g_model.valuesDirty = true;
      }
      break;
    }
    case 'R':
      modelRemoveApp(strtol(p, &end, 10));
      break;
    case 'I':
      handleIcon(p);
      break;
    case 'D':
      dumpState();
      break;
    case 'J':
      injectInput(p);
      break;
    case 'W':
      sendScreenshot();
      break;
    default:
      break;
  }
}

}  // namespace

void linkInit() {
  // The HWCDC ISR drops bytes when this queue is full (no USB flow control), so it must
  // hold one whole icon line plus some small lines; the PC sends one icon at a time.
  Serial.setRxBufferSize(32 * 1024);
  Serial.setTxTimeoutMs(20);  // never stall the UI when no host is reading
  Serial.begin(115200);
  line = (char *)heap_caps_malloc(kLineMax, MALLOC_CAP_SPIRAM);

  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  snprintf(deviceId, sizeof(deviceId), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5]);
  sendf("!HELLO\n");
}

void linkPoll() {
  uint8_t chunk[512];
  size_t budget = 48 * 1024;

  while (budget > 0) {
    int avail = Serial.available();
    if (avail <= 0) break;
    size_t n = Serial.read(chunk, min((size_t)avail, sizeof(chunk)));
    if (n == 0) break;
    budget -= min(budget, n);
    lastRxMs = millis();
    sawCompanion = true;
    companionLeft = false;

    for (size_t i = 0; i < n; i++) {
      char c = (char)chunk[i];
      if (c == '\r') continue;
      if (c == '\n') {
        if (!lineOverflow) {
          line[lineLen] = '\0';
          handleLine(line, lineLen);
        }
        lineLen = 0;
        lineOverflow = false;
        continue;
      }
      if (lineLen + 1 < kLineMax) {
        line[lineLen++] = c;
      } else {
        lineOverflow = true;
      }
    }
  }

  bool online = sawCompanion && !companionLeft && millis() - lastRxMs < kOfflineAfterMs;
  if (online != g_model.online) {
    g_model.online = online;
    g_model.valuesDirty = true;
  }

  // Announce ourselves while offline so the companion can find the knob by listening only, without
  // writing to other serial devices. Skipped while the TX buffer is full (port not open on the PC)
  // so a beacon never blocks the UI.
  if (!online && millis() - lastBeaconMs >= kBeaconMs) {
    lastBeaconMs = millis();
    if (Serial.availableForWrite() >= 48) sendf("!VOLKNOB 1 %s\n", deviceId);
  }

  if (refreshPending && (int32_t)(millis() - refreshDueMs) >= 0) {
    refreshPending = false;
    if (online) sendf("r\n");
  }
}

void linkSendMaster(int volume, bool muted) { sendf("g %d %d\n", volume, muted ? 1 : 0); }

void linkSendApp(int id, int volume, bool muted) { sendf("a %d %d %d\n", id, volume, muted ? 1 : 0); }
