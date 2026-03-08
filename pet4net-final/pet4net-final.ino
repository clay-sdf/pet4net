//DEMO
#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// --- JPG BACKGROUND SUPPORT ---
#include <LittleFS.h>
#include <TJpg_Decoder.h>

// =====================
// NETWORK & PREFS
// =====================
const char* ssid = "PET4NET Vendo";
const char* password = "";

IPAddress local_IP(10, 0, 0, 100);
IPAddress gateway(10, 0, 0, 1);
IPAddress subnet(255, 255, 255, 0);
WebServer server(80);
Preferences prefs;

// =====================
// MIKROTIK REST SETTINGS
// =====================
const char* MT_HOST = "10.0.0.1";
const uint16_t MT_PORT = 443;
const bool MT_USE_HTTPS = true;
const char* MT_USER = "admin";
const char* MT_PASS = "BOTTLE2WIFI";

String mtBaseUrl() {
  return (MT_USE_HTTPS ? "https://" : "http://") + String(MT_HOST) + ":" + String(MT_PORT) + "/rest";
}

String mtGET(const String& url) {
  HTTPClient http;
  if (MT_USE_HTTPS) {
    WiFiClientSecure client;
    client.setInsecure();
    http.begin(client, url);
  } else {
    WiFiClient client;
    http.begin(client, url);
  }
  http.setAuthorization(MT_USER, MT_PASS);
  int code = http.GET();
  String body = (code > 0) ? http.getString() : "";
  http.end();
  return body;
}

bool mtDELETE(const String& url) {
  HTTPClient http;
  if (MT_USE_HTTPS) {
    WiFiClientSecure client;
    client.setInsecure();
    http.begin(client, url);
  } else {
    WiFiClient client;
    http.begin(client, url);
  }
  http.setAuthorization(MT_USER, MT_PASS);
  int code = http.sendRequest("DELETE");
  http.end();
  return (code > 0 && code < 400);
}

bool mtEndHotspotByMac(const String& mac) {
  if (mac.length() < 11) return false;
  bool didSomething = false;

  String actUrl = mtBaseUrl() + "/ip/hotspot/active?mac-address=" + mac + "&.proplist=.id";
  String actBody = mtGET(actUrl);
  if (actBody.length() > 2) {
    StaticJsonDocument<2048> doc;
    if (!deserializeJson(doc, actBody) && doc.is<JsonArray>()) {
      for (JsonObject obj : doc.as<JsonArray>()) {
        const char* id = obj[".id"];
        if (id) didSomething |= mtDELETE(mtBaseUrl() + "/ip/hotspot/active/" + String(id));
      }
    }
  }

  String ckUrl = mtBaseUrl() + "/ip/hotspot/cookie?mac-address=" + mac + "&.proplist=.id";
  String ckBody = mtGET(ckUrl);
  if (ckBody.length() > 2) {
    StaticJsonDocument<2048> doc;
    if (!deserializeJson(doc, ckBody) && doc.is<JsonArray>()) {
      for (JsonObject obj : doc.as<JsonArray>()) {
        const char* id = obj[".id"];
        if (id) didSomething |= mtDELETE(mtBaseUrl() + "/ip/hotspot/cookie/" + String(id));
      }
    }
  }
  return didSomething;
}

// Direct MikroTik REST API: Creates a temporary Hotspot user
bool mtCreateHotspotUser(const String& mac, int minutes) {
  Serial.println("\n=== SENDING TO MIKROTIK ===");
  Serial.print("MAC Address: ");
  Serial.println(mac);
  Serial.print("Total Time (Mins): ");
  Serial.println(minutes);
  Serial.println("===========================\n");
  String url = mtBaseUrl() + "/ip/hotspot/user";
  
  // 1. If user already exists, remove them first
  String existingBody = mtGET(url + "?name=" + mac + "&.proplist=.id");
  if (existingBody.length() > 2) {
    StaticJsonDocument<1024> doc;
    if (!deserializeJson(doc, existingBody) && doc.is<JsonArray>()) {
      for (JsonObject obj : doc.as<JsonArray>()) {
        const char* id = obj[".id"];
        if (id) mtDELETE(url + "/" + String(id));
      }
    }
  }

  // 2. Add the new user with a blank password and time limit
  HTTPClient http;
  if (MT_USE_HTTPS) {
    WiFiClientSecure client; client.setInsecure(); http.begin(client, url);
  } else {
    WiFiClient client; http.begin(client, url);
  }
  
  http.setAuthorization(MT_USER, MT_PASS);
  http.addHeader("Content-Type", "application/json");
  
  // Changed "password\":\"\"" to "password\":\"" + mac + "\""
  String payload = "{\"name\":\"" + mac + "\",\"password\":\"" + mac + "\",\"profile\":\"default\",\"limit-uptime\":\"" + String(minutes) + "m\",\"comment\":\"ESP32 Vendo\"}";
  
  int code = http.PUT(payload); // PUT creates the entry
  http.end();
  
  Serial.print("MT CREATE USER -> ");
  Serial.println(code);
  return (code >= 200 && code < 300);
}


// =====================
// HARDWARE PINS
// =====================
#define BUZZER_PIN 12
#define SERVO_PIN 13
#define RX2_PIN 16
#define TX2_PIN 17
#define TOUCH_CS 21
XPT2046_Touchscreen touch(TOUCH_CS);

// =====================
// UI / STATE VARIABLES
// =====================
TFT_eSPI tft = TFT_eSPI();
Servo dropServo;

#define THEME_MAROON 0x7800
#define THEME_DKGRAY 0x2104
#define THEME_BLACK 0x0000
#define THEME_WHITE 0xFFFF
#define THEME_GREEN 0x07E0
#define THEME_RED 0xF800

const char* BG_BOOTING = "/11.jpg";
const char* BG_SYSTEM_READY = "/4.jpg";
const char* BG_LOCKED_UI = "/5.jpg";
const char* BG_GET_READY = "/6.jpg";
const char* BG_SCANNING = "/7.jpg";
const char* BG_SUCCESS = "/9.jpg";
const char* BG_INVALID = "/10.jpg";
const char* BG_ADMIN = "/8.jpg";

String activeMac = "";
bool isProcessing = false;
bool transactionDone = false;
unsigned long lastSeen = 0;
const unsigned long SESSION_TIMEOUT = 15000;

int bottleCount = 0;
int canCount = 0;
unsigned long scanStartTime = 0;
bool waitingForCam = false;

// Admin Data
bool adminArmed = false;
bool adminEnabled = false;
String adminPass = "admin";
int rateBottle = 15;
int rateCan = 30;
bool bypassb=0;
bool bypassc=0;

// Touch State
unsigned long touchStartTime = 0;
bool isTouching = false;

// =====================
// TFT HELPER FUNCTIONS
// =====================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

void drawBackgroundJpg(const char* path) {
  tft.fillScreen(THEME_BLACK);  // Guarantee dark background fallback
  if (!path || !LittleFS.exists(path)) return;
  TJpgDec.drawFsJpg(0, 0, path, LittleFS);
}

void drawFrame() {
  tft.drawRect(0, 0, tft.width(), tft.height(), THEME_DKGRAY);
  tft.drawRect(5, 5, tft.width() - 10, tft.height() - 10, THEME_MAROON);
}

void updateScreen(const String& title, uint32_t titleColor, const String& subtitle, uint32_t subColor, const char* bgPath) {
  tft.setTextPadding(0);
  drawBackgroundJpg(bgPath);
  drawFrame();

  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(THEME_BLACK);
  tft.drawString(title, tft.width() / 2 + 2, tft.height() / 2 - 38, 4);
  tft.setTextColor(titleColor);
  tft.drawString(title, tft.width() / 2, tft.height() / 2 - 40, 4);
  tft.setTextSize(1);
  tft.setTextColor(subColor);
  tft.drawString(subtitle, tft.width() / 2, tft.height() / 2 + 18, 2);
}

void beep(int ms) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(ms);
  digitalWrite(BUZZER_PIN, LOW);
}

// =====================
// API HANDLERS (USER)
// =====================
void handleStart() {
  if (!server.hasArg("mac")) {
    server.send(400, "application/json", "{}");
    return;
  }

  String requestingMac = server.arg("mac");

  // Allow if no one is using it OR if the SAME MAC is asking again
  if (!isProcessing || requestingMac == activeMac) {
    activeMac = requestingMac;
    isProcessing = true;
    transactionDone = false;
    // Don't reset bottle/can count if it's the same user re-opening the page
    if (requestingMac != activeMac) {
        bottleCount = 0;
        canCount = 0;
    }
    lastSeen = millis();
    server.send(200, "application/json", "{\"status\":\"GO\"}");
  } else {
    // Truly a different user while the machine is active
    server.send(200, "application/json", "{\"status\":\"BUSY\"}");
  }
}

void handleStatus() {
  if (server.arg("mac") == activeMac) lastSeen = millis();
  int totalMins = (bottleCount * rateBottle) + (canCount * rateCan);
  String res = "{\"ok\":true,\"bottles\":" + String(bottleCount) + ",\"cans\":" + String(canCount) + ",\"totalMinutes\":" + String(totalMins) + "}";
  server.send(200, "application/json", res);
}

void handleDrop() {
  if (!isProcessing) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  lastSeen = millis();
  String mode = server.arg("mode");
  Serial2.print(mode.charAt(0));
  waitingForCam = true;
  scanStartTime = millis();
  server.send(200, "application/json", "{\"ok\":true}");
}

bool mtRunMikrotikScript(const String& mac, int minutes) {
  String url = mtBaseUrl() + "/execute";
  HTTPClient http;
  
  if (MT_USE_HTTPS) {
    WiFiClientSecure client; client.setInsecure(); http.begin(client, url);
  } else {
    WiFiClient client; http.begin(client, url);
  }
  
  http.setAuthorization(MT_USER, MT_PASS);
  http.addHeader("Content-Type", "application/json");
  
  // Defines the globals, then runs your script. 
  // IMPORTANT: Ensure your script in MikroTik is named exactly "AddVendoUser"
  String runCmd = ":global mac \"" + mac + "\"; :global time \"" + String(minutes) + "\"; /system script run AddVendoUser;";
  String payload = "{\"script\":\"" + runCmd + "\"}";
  
  int code = http.POST(payload);
  http.end();
  
  Serial.print("MT SCRIPT RUN -> ");
  Serial.println(code);
  return (code >= 200 && code < 300);
}

void handleFinalize() {
  if (!isProcessing) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  
  int totalMins = (bottleCount * rateBottle) + (canCount * rateCan);
  mtCreateHotspotUser(activeMac, totalMins);
  
  // Clear processing flag immediately after creation
  isProcessing = false; 
  transactionDone = true; 

  String res = "{\"ok\":true,\"mac\":\"" + activeMac + "\",\"time\":" + String(totalMins) + "}";
  server.send(200, "application/json", res);
  
  updateScreen("SUCCESS!", THEME_GREEN, "Authorized: " + String(totalMins) + " mins", THEME_WHITE, BG_SUCCESS);
  beep(500);
}

void handleEnd() {
  if (server.hasArg("mac")) mtEndHotspotByMac(server.arg("mac"));
  isProcessing = false;
  activeMac = "";
  transactionDone = false;
  server.send(200, "application/json", "{\"ok\":true}");
  updateScreen("", THEME_MAROON, "", THEME_DKGRAY, BG_SYSTEM_READY);
}

void handleUI() {
  String screen = server.arg("screen");
  if (screen == "home") updateScreen("", THEME_MAROON, "", THEME_DKGRAY, BG_SYSTEM_READY);
  else if (screen == "welcome") updateScreen("", THEME_MAROON, "", THEME_DKGRAY, BG_LOCKED_UI);
  else if (screen == "select") updateScreen("", THEME_MAROON, "", THEME_DKGRAY, BG_GET_READY);
  else if (screen == "scanning") updateScreen("", THEME_MAROON, "", THEME_WHITE, BG_SCANNING);
  server.send(200, "application/json", "{\"ok\":true}");
}

// =====================
// API HANDLERS (ADMIN)
// =====================
void handleAdminState() {
  String res = "{\"armed\":";
  res += adminArmed ? "true" : "false";
  res += ",\"enabled\":";
  res += adminEnabled ? "true" : "false";
  res += "}";
  server.send(200, "application/json", res);
}

void handleAdminLogin() {
  if (server.arg("pass") == adminPass) {
    adminEnabled = true;
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(401, "application/json", "{\"ok\":false}");
  }
}

void handleAdminSet() {
  if (!adminEnabled) {
    server.send(401, "application/json", "{\"ok\":false}");
    return;
  }
  if (server.hasArg("bottle")) {
    rateBottle = server.arg("bottle").toInt();
    prefs.putInt("rBottle", rateBottle);
  }
  if (server.hasArg("can")) {
    rateCan = server.arg("can").toInt();
    prefs.putInt("rCan", rateCan);
  }
  if (server.hasArg("newpass") && server.arg("newpass").length() > 0) {
    adminPass = server.arg("newpass");
    prefs.putString("pass", adminPass);
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleAdminExit() {
  adminArmed = false;
  adminEnabled = false;
  lastSeen = millis();  //
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRates() {
  String res = "{\"ok\":true,\"bottle\":" + String(rateBottle) + ",\"can\":" + String(rateCan) + "}";
  server.send(200, "application/json", res);
}

// =====================
// SETUP / LOOP
// =====================
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RX2_PIN, TX2_PIN);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Load Prefs
  prefs.begin("vendo", false);
  adminPass = prefs.getString("pass", "admin");
  rateBottle = prefs.getInt("rBottle", 15);
  rateCan = prefs.getInt("rCan", 30);

  dropServo.setPeriodHertz(50);
  dropServo.attach(SERVO_PIN, 1000, 2500);
  dropServo.write(30);

  tft.init();
  tft.setRotation(3);
  uint16_t calData[5] = { 451, 3312, 484, 3039, 1 };
  tft.setTouch(calData);

  LittleFS.begin(false);
  TJpgDec.setCallback(tft_output);
  TJpgDec.setSwapBytes(true);

  updateScreen("BOOTING...", THEME_MAROON, "Connecting...", THEME_DKGRAY, BG_BOOTING);

  WiFi.config(local_IP, gateway, subnet);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(300);

  server.enableCORS(true);
  server.on("/start", handleStart);
  server.on("/status", handleStatus);
  server.on("/drop", handleDrop);
  server.on("/finalize", handleFinalize);
  server.on("/end", handleEnd);
  server.on("/ui", handleUI);
  server.on("/admin/state", handleAdminState);
  server.on("/admin/login", handleAdminLogin);
  server.on("/admin/set", handleAdminSet);
  server.on("/admin/exit", handleAdminExit);
  server.on("/rates", handleRates);
  server.begin();

  updateScreen("", THEME_MAROON, "", THEME_DKGRAY, BG_SYSTEM_READY);
}

void loop() {
  server.handleClient();

  if (isProcessing && !transactionDone && !adminArmed && !adminEnabled && (millis() - lastSeen > SESSION_TIMEOUT)) {
    isProcessing = false;
    activeMac = "";
    updateScreen("", THEME_MAROON, "", THEME_DKGRAY, BG_SYSTEM_READY);
  }

  // 2. Hidden 5-Second Admin Touch Check
  uint16_t tx = 0, ty = 0;
  if (tft.getTouch(&tx, &ty)) {
    if (!isTouching) {
      isTouching = true;
      touchStartTime = millis();
    } else if (millis() - touchStartTime > 5000 && !adminArmed) {
      adminArmed = true;
      beep(600);
      updateScreen("", THEME_RED, "", THEME_WHITE, BG_ADMIN);
    }
  } else {
    isTouching = false;
  }

// DEMO BYPASS
if (Serial.available()) {
    char overrideSig = (char)Serial.read();
    if (overrideSig == 'B') {
      Serial.print("BYPASS-BOTTLE TRIGGERED: ");
      if(bypassb==0){
        bypassb =1;
        bypassc =0;
      }
      else if(bypassb==1) bypassb =0;
      Serial.println(bypassb);
    }
    if (overrideSig == 'C') {
      Serial.print("BYPASS-CAN TRIGGERED: ");
      if(bypassc==0) {
        bypassb =0;
        bypassc =1;
      }
      else if(bypassc==1) bypassc =0;
      Serial.println(bypassc);
    }
  }

  // 3. Serial Camera Response Check
  if (waitingForCam && Serial2.available()) {
    char camSignal = Serial2.read();
    waitingForCam = false;

    if (camSignal == 'B'|| bypassb==1 && bypassc==0) bottleCount++;
    else if (camSignal == 'C'|| bypassc==1) canCount++;

    // Briefly display ACCEPTED, HTML logic will overwrite it back to SELECT shortly
    if (camSignal == 'B' || bypassb==1 && bypassc==0 ) {
      updateScreen("", THEME_MAROON, "", THEME_GREEN, BG_SUCCESS);
      beep(100);
      delay(50);
      beep(100);
      dropServo.write(0);
      delay(1500);
      dropServo.write(30);
    } else if (camSignal == 'C' || bypassc==1 ){
      updateScreen("", THEME_MAROON, "", THEME_GREEN, BG_SUCCESS);
      beep(100);
      delay(50);
      beep(100);
      dropServo.write(70);
      delay(1500);
      dropServo.write(30);
    } else {
      updateScreen("", THEME_RED, "", THEME_DKGRAY, BG_INVALID);
      beep(450);
    }
  }
}