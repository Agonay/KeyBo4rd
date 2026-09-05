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
static bool isEditing = false; // short press locks into edit mode

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
        case IdleClock:     snprintf(buf, sz, "Clock"); break;
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
static bool lastEditing = false;

static void markDirty() { needsRender = true; }

// ---------- OLED drawing ----------
void menuDrawOled() {
  char buf[20];
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // header
  oled.setCursor(2, 1);
  oled.print(F("MENU"));
  oled.drawFastHLine(0, 9, 128, SSD1306_WHITE);

  // 6 rows: rows 0..5, each 8px tall, starting at y=12
  int y = 12;
  int lineH = 8;
  for (int i = 0; i < MENU_ROW_COUNT; i++) {
    rowValue(i, buf, sizeof(buf));
    bool hl = (i == highlight);

    // abbreviated label for OLED
    const char *shortLabel;
    switch (i) {
      case 0: shortLabel = "Knob steps"; break;
      case 1: shortLabel = "Direction";   break;
      case 2: shortLabel = "OLED brt";    break;
      case 3: shortLabel = "Idle screen"; break;
      case 4: shortLabel = "Battery";     break;
      case 5: shortLabel = "Low bat %";   break;
      default: shortLabel = rowLabels[i]; break;
    }

    if (hl) {
      oled.fillRect(0, y, 128, lineH, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    }

    // compact layout: "Label: value"
    char line[22];
    snprintf(line, sizeof(line), "%-11s%s", shortLabel, buf);
    oled.setCursor(2, y + 1);
    oled.print(line);

    oled.setTextColor(SSD1306_WHITE);
    y += lineH;
  }

  // footer
  oled.drawFastHLine(0, 60, 128, SSD1306_WHITE);
  oled.setCursor(2, 61);
  if (isEditing) {
    oled.print(F("S=done L=exit"));
  } else {
    oled.print(F("S=edit L=exit"));
  }

  oled.display();
}

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
  isEditing = false;
  Serial.println(F("[MENU] close"));
}

void menuForceRender() {
  needsRender = true;
}

void menuEventShort() {
  if (!menuOpen_) return;
  if (isEditing) {
    // Exit edit mode, back to browse
    isEditing = false;
    markDirty();
    Serial.printf("[MENU] exit edit mode, highlight=%d\n", highlight);
  } else {
    // Enter edit mode for current row
    isEditing = true;
    cycleValue(highlight); // apply first change immediately
    markDirty();
    Serial.printf("[MENU] enter edit mode, row=%d\n", highlight);
  }
}

void menuEventLong() {
  if (isEditing) {
    // Exit edit mode first
    isEditing = false;
    markDirty();
    Serial.println(F("[MENU] exit edit mode (long)"));
    return;
  }
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
  if (isEditing) {
    // In edit mode: rotate changes the value
    cycleValue(highlight);
  } else {
    // In browse mode: rotate scrolls through options
    highlight = (highlight + dir + MENU_ROW_COUNT) % MENU_ROW_COUNT;
    Serial.printf("[MENU] highlight=%d\n", highlight);
  }
  markDirty();
}

void menuRenderIfChanged() {
  if (!menuOpen_) {
    needsRender = false;
    lastHighlight = -1;
    lastEditing = false;
    return;
  }
  if (!needsRender && highlight == lastHighlight && isEditing == lastEditing) return;
  menuDrawOled();
  lastHighlight = highlight;
  lastEditing = isEditing;
  needsRender = false;
}
