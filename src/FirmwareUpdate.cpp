#include "FirmwareUpdate.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_system.h>
#include <Wire.h>
#include "DisplayManager.h"
#include "OtaUpdater.h"
#include "HomeControl.h"

static WebServer server(80);
static bool active = false;
static bool failed = false;
static bool received = false;
static unsigned long started = 0;
static unsigned long restartAt = 0;
static String password;

static String htmlEscape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  return s;
}

bool startFirmwareUpdateIfRequested() {
  pinMode(13, INPUT_PULLUP);
  if (digitalRead(13) != LOW) return false;
  const unsigned long heldAt = millis();
  while (millis() - heldAt < 1500) {
    if (digitalRead(13) != LOW) return false;
    delay(10);
  }
  // Fixed, easy to type. WPA2 needs at least 8 characters ("1234" makes softAP fail).
  // The portal is only up for 5 minutes after holding the knob at power-on.
  password = "12345678";
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP("K3YBo4rd-Update", password.c_str(), 1, false, 1)) {
    Serial.println("[UPDATE] AP failed; continuing normal boot");
    WiFi.mode(WIFI_OFF);
    return false;
  }
  active = true;
  started = millis();
  Serial.printf("[UPDATE] Wi-Fi: K3YBo4rd-Update  Password: %s\n", password.c_str());
  Serial.printf("[UPDATE] Open http://%s (five-minute window)\n", WiFi.softAPIP().toString().c_str());
  server.on("/", HTTP_GET, [] {
    String ssid = otaSavedSsid();
    ssid.replace("&", "&amp;"); ssid.replace("<", "&lt;"); ssid.replace("\"", "&quot;");
    String html = "<h1>K3YBo4rd</h1><p>Running firmware v" + fwVersionString(FW_VERSION) +
      (FW_VERSION == 0 ? " (dev build: auto-update off)" : "") + "</p>"
      "<h2>Home Wi-Fi (for automatic updates)</h2><p>Saved network: " +
      (ssid.length() ? ssid : String("<i>none</i>")) + "</p>"
      "<form method='POST' action='/wifi'><input name='ssid' placeholder='SSID' value='" + ssid +
      "' required> <input name='pass' type='password' placeholder='Password'> <button>Save &amp; reboot</button></form>"
      "<h2>Home Assistant (Fn layer)</h2>"
      "<p>Use the plain <b>http://</b> LAN address (HTTPS doesn't fit in memory next to Bluetooth). "
      "Token: HA profile &rarr; Security &rarr; Long-lived access tokens.</p>"
      "<form method='POST' action='/ha'>"
      "<p><input name='url' size='40' placeholder='http://192.168.1.10:8123' value='" + htmlEscape(homeUrl()) + "'></p>"
      "<p><input name='token' type='password' size='40' placeholder='" +
      (homeHasToken() ? String("token saved (leave empty to keep)") : String("long-lived access token")) + "'></p>"
      "<p>One device per line, line N = Fn+N (max 9). <code>Name = entity</code> or "
      "<code>Name = press_entity, turn_entity</code>:</p>"
      "<textarea name='slots' rows='9' cols='60' placeholder='Office fan = light.office_fan, fan.office_fan&#10;Desk lamp = light.desk&#10;Speaker = media_player.office'>" +
      htmlEscape(homeSlotsText()) + "</textarea>"
      "<p><button>Save &amp; reboot</button></p></form>"
      "<h2>Manual firmware upload (recovery)</h2><p>Upload firmware.bin for esp32dev. Keep power connected.</p>"
      "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='firmware' accept='.bin' required><button>Install firmware</button></form>";
    server.send(200, "text/html", html);
  });
  server.on("/wifi", HTTP_POST, [] {
    const String ssid = server.arg("ssid");
    if (!ssid.length() || ssid.length() > 32 || server.arg("pass").length() > 64) {
      server.send(400, "text/plain", "Invalid SSID or password length.");
      return;
    }
    otaSaveWifiCredentials(ssid, server.arg("pass"));
    Serial.printf("[UPDATE] Saved Wi-Fi '%s'\n", ssid.c_str());
    server.send(200, "text/plain", "Saved. Rebooting; the keyboard will check GitHub for updates on boot.");
    restartAt = millis() + 1000;
  });
  server.on("/ha", HTTP_POST, [] {
    if (server.arg("url").length() > 100 || server.arg("token").length() > 400 || server.arg("slots").length() > 1500) {
      server.send(400, "text/plain", "Value too long.");
      return;
    }
    homeSaveConfig(server.arg("url"), server.arg("token"), server.arg("slots"));
    Serial.println(F("[UPDATE] Saved Home Assistant settings"));
    server.send(200, "text/plain", "Saved. Rebooting.");
    restartAt = millis() + 1000;
  });
  server.on("/update", HTTP_POST, [] {
    const bool ok = received && !failed && !Update.hasError();
    server.send(ok ? 200 : 400, "text/plain", ok ? "Installed. Rebooting." : "Update failed. Upload the correct firmware.bin and retry.");
    if (ok) restartAt = millis() + 1000;
  }, [] {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      failed = false;
      received = false;
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) failed = true;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (!failed && Update.write(upload.buf, upload.currentSize) != upload.currentSize) failed = true;
    } else if (upload.status == UPLOAD_FILE_END) {
      if (!failed) {
        received = Update.end(true);
        failed = !received;
      } else Update.abort();
      if (failed) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      Update.abort();
      failed = true;
    }
  });
  Wire.begin();
  Wire.setTimeOut(10);
  if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3c)) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("K3YBo4rd-Update");
    oled.println("Wi-Fi password:");
    oled.println(password);
    oled.println("Open browser:");
    oled.println(WiFi.softAPIP());
    oled.println("Wi-Fi setup/upload");
    oled.display();
  }
  server.begin();
  return true;
}
bool firmwareUpdateActive() { return active; }
void handleFirmwareUpdate() {
  server.handleClient();
  if ((restartAt && (long)(millis() - restartAt) >= 0) || millis() - started > 300000) ESP.restart();
  delay(2);
}
