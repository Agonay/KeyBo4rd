#include "MenuManager.h"
#include "DisplayManager.h"
#include "BluetoothHID.h"
#include "HardwareConfig.h"
#include "OtaUpdater.h"

// ---------- persistence ----------
static Preferences prefs;
static KbdSettings settings;
static const char *ns = "kbd";

static void loadSettings() {
  prefs.begin(ns, true);
  settings.volStepsPerDetent = prefs.getUChar("volSteps", 2);
  settings.volInvertDir      = prefs.getBool("volInv", false);
  settings.oledBrightness    = prefs.getUChar("oledBrt", 100);
  settings.einkIdleMode      = (MenuIdleMode)prefs.getUChar("idleMode", 0);
  settings.showBattery       = prefs.getBool("showBat", true);
  settings.lowBatteryPct     = prefs.getUChar("lowBat", 15);
  settings.longPressMs       = prefs.getUShort("longMs", 800);
  settings.veryLongPressMs   = prefs.getUShort("vlongMs", 2500);
  prefs.end();
  // sanitize values written by older firmware
  settings.volStepsPerDetent = constrain(settings.volStepsPerDetent, 1, 4);
  settings.oledBrightness    = constrain(settings.oledBrightness, 0, 100);
  settings.lowBatteryPct     = constrain(settings.lowBatteryPct, 5, 50);
  if (settings.einkIdleMode >= IdleCount) settings.einkIdleMode = IdleTotoro;
#if EINK_SHARES_ENCODER_PIN
  if (settings.einkIdleMode == IdleClock) settings.einkIdleMode = IdleTotoro;
#endif
}

void saveSettings() {
  prefs.begin(ns, false);
  prefs.putUChar("volSteps", settings.volStepsPerDetent);
  prefs.putBool ("volInv",   settings.volInvertDir);
  prefs.putUChar("oledBrt",  settings.oledBrightness);
  prefs.putUChar("idleMode", (uint8_t)settings.einkIdleMode);
  prefs.putBool ("showBat",  settings.showBattery);
  prefs.putUChar("lowBat",   settings.lowBatteryPct);
  prefs.putUShort("longMs",  settings.longPressMs);
  prefs.putUShort("vlongMs", settings.veryLongPressMs);
  prefs.end();
}

// ---------- items ----------
enum ItemId { IT_VOLSTEP, IT_DIR, IT_BRIGHT, IT_SHOWBAT, IT_LOWBAT, IT_IDLE,
              IT_UPDATE, IT_ABOUT, IT_EXIT };

static const ItemId items[] = {
  IT_VOLSTEP, IT_DIR, IT_BRIGHT, IT_SHOWBAT, IT_LOWBAT,
#if ENABLE_EINK
  IT_IDLE,   // only meaningful with the E-Ink panel enabled
#endif
  IT_UPDATE, IT_ABOUT, IT_EXIT
};
static const int ITEM_COUNT = sizeof(items) / sizeof(items[0]);

static const char *itemLabel(ItemId id) {
  switch (id) {
    case IT_VOLSTEP: return "Volume step";
    case IT_DIR:     return "Direction";
    case IT_BRIGHT:  return "OLED bright";
    case IT_SHOWBAT: return "Show battery";
    case IT_LOWBAT:  return "Low battery";
    case IT_IDLE:    return "Idle screen";
    case IT_UPDATE:  return "Check update";
    case IT_ABOUT:   return "About";
    case IT_EXIT:    return "< Exit";
  }
  return "?";
}

static bool isEditable(ItemId id) { return id <= IT_IDLE; }

static void itemValue(ItemId id, char *buf, int sz) {
  static const char *idleNames[IdleCount] = { "Totoro", "Status", "Animation", "Clock" };
  switch (id) {
    case IT_VOLSTEP: snprintf(buf, sz, "%d", settings.volStepsPerDetent); break;
    case IT_DIR:     snprintf(buf, sz, "%s", settings.volInvertDir ? "Invert" : "Normal"); break;
    case IT_BRIGHT:  snprintf(buf, sz, "%d%%", settings.oledBrightness); break;
    case IT_SHOWBAT: snprintf(buf, sz, "%s", settings.showBattery ? "ON" : "OFF"); break;
    case IT_LOWBAT:  snprintf(buf, sz, "%d%%", settings.lowBatteryPct); break;
    case IT_IDLE:    snprintf(buf, sz, "%s", idleNames[settings.einkIdleMode]); break;
    case IT_UPDATE:  snprintf(buf, sz, "%s", otaStatusText()); break;
    case IT_ABOUT:   snprintf(buf, sz, ">"); break;
    default:         buf[0] = 0; break;
  }
}

// Direction-aware change; numbers clamp at their limits instead of wrapping.
static void adjustItem(ItemId id, int dir) {
  switch (id) {
    case IT_VOLSTEP:
      settings.volStepsPerDetent = constrain(settings.volStepsPerDetent + dir, 1, 4);
      break;
    case IT_DIR:
      settings.volInvertDir = !settings.volInvertDir;
      break;
    case IT_BRIGHT:
      settings.oledBrightness = constrain((int)settings.oledBrightness + dir * 10, 0, 100);
      applyOledBrightness(settings.oledBrightness);  // live preview
      break;
    case IT_SHOWBAT:
      settings.showBattery = !settings.showBattery;
      break;
    case IT_LOWBAT:
      settings.lowBatteryPct = constrain((int)settings.lowBatteryPct + dir * 5, 5, 50);
      break;
    case IT_IDLE:
      settings.einkIdleMode = (MenuIdleMode)((settings.einkIdleMode + IdleCount + dir) % IdleCount);
#if EINK_SHARES_ENCODER_PIN
      if (settings.einkIdleMode == IdleClock)   // clock mode needs a free SPI clock pin
        settings.einkIdleMode = (MenuIdleMode)((settings.einkIdleMode + IdleCount + dir) % IdleCount);
#endif
      break;
    default:
      break;
  }
}

// ---------- state ----------
static const unsigned long AUTO_CLOSE_MS = 20000;
static const int VISIBLE_ROWS = 5;

static bool menuOpen_ = false;
static int  highlight = 0;
static int  scrollTop = 0;
static bool isEditing = false;
static bool aboutOpen = false;
static KbdSettings editBackup;
static unsigned long lastInputAt = 0;
static bool needsRender = false;
static String lastOtaStatus;

static void markDirty() { needsRender = true; lastInputAt = millis(); }

// ---------- OLED drawing ----------
static void drawHeader(const char *title) {
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 1);
  oled.print(title);
  char fw[12];
  if (FW_VERSION == 0) snprintf(fw, sizeof(fw), "FW dev");
  else { char v[10]; snprintf(fw, sizeof(fw), "FW v%s", fwVersionLabel(FW_VERSION, v, sizeof(v))); }
  oled.setCursor(126 - (int)strlen(fw) * 6, 1);
  oled.print(fw);
  oled.drawFastHLine(0, 9, 128, SSD1306_WHITE);
}

static void drawFooter(const char *hint) {
  oled.drawFastHLine(0, 54, 128, SSD1306_WHITE);
  oled.setCursor(2, 56);
  oled.print(hint);
}

static void drawAbout() {
  drawHeader("ABOUT");
  char line[24];
  int y = 12;
  auto row = [&](const char *text) { oled.setCursor(2, y); oled.print(text); y += 8; };
  if (FW_VERSION == 0) row("Firmware: dev build");
  else { char v[10]; snprintf(line, sizeof(line), "Firmware: v%s", fwVersionLabel(FW_VERSION, v, sizeof(v))); row(line); }
  String ssid = otaSavedSsid();
  snprintf(line, sizeof(line), "WiFi: %.15s", ssid.length() ? ssid.c_str() : "not set");
  row(line);
  snprintf(line, sizeof(line), "IP: %.17s", otaLastIp());
  row(line);
  snprintf(line, sizeof(line), "Bluetooth: %s", bleKeyboard.isConnected() ? "connected" : "waiting");
  row(line);
  const unsigned long m = millis() / 60000;
  snprintf(line, sizeof(line), "Uptime: %luh %02lum", m / 60, m % 60);
  row(line);
  drawFooter("Press: back");
}

void menuDrawOled() {
  char val[18];
  char text[22];
  oled.clearDisplay();
  oled.setTextSize(1);

  if (aboutOpen) { drawAbout(); oled.display(); return; }

  drawHeader("MENU");

  // keep the highlight inside the visible window
  if (highlight < scrollTop) scrollTop = highlight;
  if (highlight >= scrollTop + VISIBLE_ROWS) scrollTop = highlight - VISIBLE_ROWS + 1;

  for (int r = 0; r < VISIBLE_ROWS && scrollTop + r < ITEM_COUNT; r++) {
    const int i = scrollTop + r;
    const ItemId id = items[i];
    const bool hl = (i == highlight);
    const int y = 11 + r * 8;
    itemValue(id, val, sizeof(val));
    if (hl && isEditing) snprintf(text, sizeof(text), "%-11s<%s>", itemLabel(id), val);
    else                 snprintf(text, sizeof(text), "%-12s%s", itemLabel(id), val);
    if (hl) {
      oled.fillRect(0, y, 123, 8, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    } else {
      oled.setTextColor(SSD1306_WHITE);
    }
    oled.setCursor(2, y);
    oled.print(text);
  }
  oled.setTextColor(SSD1306_WHITE);

  // scrollbar
  if (ITEM_COUNT > VISIBLE_ROWS) {
    const int trackH = VISIBLE_ROWS * 8;
    const int thumbH = max(4, trackH * VISIBLE_ROWS / ITEM_COUNT);
    const int thumbY = 11 + (trackH - thumbH) * scrollTop / (ITEM_COUNT - VISIBLE_ROWS);
    oled.fillRect(125, thumbY, 2, thumbH, SSD1306_WHITE);
  }

  const ItemId cur = items[highlight];
  if (isEditing)              drawFooter("Turn:adjust Press:OK");
  else if (isEditable(cur))   drawFooter("Turn:move Press:edit");
  else                        drawFooter("Turn:move Press:go");
  oled.display();
}

// ---------- public ----------
KbdSettings &getSettings() { return settings; }
bool isMenuOpen() { return menuOpen_; }

void menuInit() {
  loadSettings();
  menuOpen_ = false;
  highlight = 0;
}

void menuOpen() {
  menuOpen_ = true;
  highlight = 0;
  scrollTop = 0;
  isEditing = false;
  aboutOpen = false;
  markDirty();
  Serial.println(F("[MENU] open"));
}

void menuClose() {
  if (isEditing) {           // closing mid-edit keeps the edited value
    isEditing = false;
  }
  saveSettings();
  menuOpen_ = false;
  aboutOpen = false;
  Serial.println(F("[MENU] close"));
}

void menuForceRender() { needsRender = true; }

void menuEventShort() {
  if (!menuOpen_) return;
  if (aboutOpen) { aboutOpen = false; markDirty(); return; }
  const ItemId id = items[highlight];
  if (isEditing) {
    isEditing = false;       // save
    saveSettings();
    Serial.printf("[MENU] saved %s\n", itemLabel(id));
  } else if (isEditable(id)) {
    isEditing = true;
    editBackup = settings;
    Serial.printf("[MENU] editing %s\n", itemLabel(id));
  } else if (id == IT_UPDATE) {
    otaCheckNow();
  } else if (id == IT_ABOUT) {
    aboutOpen = true;
  } else if (id == IT_EXIT) {
    menuClose();
    return;
  }
  markDirty();
}

void menuEventLong() {
  if (!menuOpen_) return;
  if (aboutOpen) { aboutOpen = false; markDirty(); return; }
  if (isEditing) {           // cancel: restore the value from before editing
    settings = editBackup;
    applyOledBrightness(settings.oledBrightness);
    isEditing = false;
    Serial.println(F("[MENU] edit cancelled"));
    markDirty();
    return;
  }
  menuClose();
}

void menuRotate(int dir) {
  if (!menuOpen_) return;
  if (aboutOpen) return;
  if (isEditing) {
    adjustItem(items[highlight], dir);
  } else {
    highlight = constrain(highlight + dir, 0, ITEM_COUNT - 1);
  }
  markDirty();
}

void menuRenderIfChanged() {
  if (!menuOpen_) { needsRender = false; return; }
  if (otaScreenActive()) return;  // the update screen owns the OLED

  const unsigned long now = millis();
  if (now - lastInputAt > AUTO_CLOSE_MS) {
    Serial.println(F("[MENU] auto-close"));
    menuClose();
    return;
  }
  // live values: update-check status and the About screen
  const String st = otaStatusText();
  if (st != lastOtaStatus) { lastOtaStatus = st; needsRender = true; }
  static unsigned long lastAboutDraw = 0;
  if (aboutOpen && now - lastAboutDraw > 1000) { lastAboutDraw = now; needsRender = true; }

  if (!needsRender) return;
  needsRender = false;
  menuDrawOled();
}
