#include "MenuManager.h"
#include "DisplayManager.h"
#include "BluetoothHID.h"

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

// ---------- menu state ----------
static bool menuOpen_ = false;
static int  highlight = 0;

static const char *rowLabels[MENU_ROW_COUNT] = {
  "1. Knob steps",
  "2. Direction",
  "3. OLED bright",
  "4. Idle screen",
  "5. Show battery",
  "6. Low bat %"
};

static void rowValue(int idx, char *buf, int sz) {
  switch (idx) {
    case 0: snprintf(buf, sz, "%d", settings.volStepsPerDetent); break;
    case 1: snprintf(buf, sz, "%s", settings.volInvertDir ? "Inverted" : "Normal"); break;
    case 2: snprintf(buf, sz, "%d%%", settings.oledBrightness); break;
    case 3: {
      switch (settings.einkIdleMode) {
        case IdleTotoro:    snprintf(buf, sz, "Totoro"); break;
        case IdleStatus:    snprintf(buf, sz, "Status"); break;
        case IdleAnimation: snprintf(buf, sz, "Animation"); break;
        default:            snprintf(buf, sz, "?"); break;
      }
      break;
    }
    case 4: snprintf(buf, sz, "%s", settings.showBattery ? "ON" : "OFF"); break;
    case 5: snprintf(buf, sz, "%d%%", settings.lowBatteryPct); break;
  }
}

static void cycleValue(int idx) {
  switch (idx) {
    case 0: {
      uint8_t v = settings.volStepsPerDetent + 1;
      if (v > 4) v = 1;
      settings.volStepsPerDetent = v;
      break;
    }
    case 1:
      settings.volInvertDir = !settings.volInvertDir;
      break;
    case 2: {
      uint8_t v = settings.oledBrightness;
      v = (v >= 95) ? 100 : v + 10;
      settings.oledBrightness = v;
      applyOledBrightness(v);
      break;
    }
    case 3:
      settings.einkIdleMode = (MenuIdleMode)((settings.einkIdleMode + 1) % IdleCount);
      break;
    case 4:
      settings.showBattery = !settings.showBattery;
      break;
    case 5: {
      uint8_t v = settings.lowBatteryPct;
      v = (v >= 95) ? 5 : v + 5;
      settings.lowBatteryPct = v;
      break;
    }
  }
  saveSettings();
  Serial.printf("[MENU] row%d cycled\n", idx);
}

// ---------- render flags ----------
static bool needsRender = false;
static int  lastHighlight = -1;

static void markDirty() { needsRender = true; }

// ---------- e-ink drawing ----------
void menuDrawEink() {
  char buf[20];
  einkDisplay.init(115200, true, 50, false);
  einkDisplay.setRotation(0);
  einkDisplay.setFullWindow();
  einkDisplay.firstPage();
  do {
    einkDisplay.fillScreen(GxEPD_WHITE);
    einkDisplay.setTextSize(1);
    einkDisplay.setTextColor(GxEPD_BLACK);

    einkDisplay.setCursor(10, 10);
    einkDisplay.print(F("MENU — rotate to scroll, short to change"));
    einkDisplay.drawFastHLine(10, 18, 180, GxEPD_BLACK);

    int y = 30;
    int lineH = 20;
    for (int i = 0; i < MENU_ROW_COUNT; i++) {
      rowValue(i, buf, sizeof(buf));
      bool hl = (i == highlight);
      if (hl) {
        einkDisplay.fillRect(6, y - 8, 188, lineH + 2, GxEPD_BLACK);
        einkDisplay.setTextColor(GxEPD_WHITE);
      }
      einkDisplay.setCursor(10, y);
      einkDisplay.print(rowLabels[i]);
      einkDisplay.print(F(": "));
      einkDisplay.print(buf);
      einkDisplay.setTextColor(GxEPD_BLACK);
      y += lineH;
    }

    einkDisplay.drawFastHLine(10, 170, 180, GxEPD_BLACK);
    einkDisplay.setCursor(10, 176);
    einkDisplay.print(F("Short=change  Long=close"));
  } while (einkDisplay.nextPage());
  einkDisplay.hibernate();
}

// ---------- public ----------
KbdSettings &getSettings() { return settings; }
int menuHighlightIndex() { return highlight; }

bool isMenuOpen() { return menuOpen_; }

void menuInit() {
  loadSettings();
  menuOpen_ = false;
  highlight = 0;
}

void menuOpen() {
  menuOpen_ = true;
  highlight = 0;
  markDirty();
  Serial.println(F("[MENU] open"));
}

void menuClose() {
  menuOpen_ = false;
  Serial.println(F("[MENU] close"));
}

void menuEventShort() {
  if (!menuOpen_) return;
  cycleValue(highlight);
}

void menuEventLong() {
  if (!menuOpen_) {
    menuOpen();
    return;
  }
  menuClose();
}

void menuEventVeryLong() {
  if (menuOpen_) {
    menuClose();
    return;
  }
  Serial.println(F("[MENU] very-long on idle"));
}

void menuRotate(int dir) {
  if (!menuOpen_) return;
  highlight = (highlight + dir + MENU_ROW_COUNT) % MENU_ROW_COUNT;
  markDirty();
  Serial.printf("[MENU] highlight=%d\n", highlight);
}

void menuRenderIfChanged() {
  if (!menuOpen_) { needsRender = false; return; }
  if (!needsRender && highlight == lastHighlight) return;
  menuDrawEink();
  lastHighlight = highlight;
  needsRender = false;
}
