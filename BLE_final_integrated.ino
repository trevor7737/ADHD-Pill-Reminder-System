/* ESP32 ADHD Reminder - improved buzzer + NFC disarm removes the triggered time
   - Web UI (embedded or SPIFFS)
   - /api/schedule POST accepted (you already verified)
   - buzzer uses LEDC (works for passive & active buzzers)
   - NFC disarm removes the current triggered time for that day
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_NeoPixel.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "time.h"

// -------- WiFi Credentials ------------
const char* ssid     = "DukeVisitor";
const char* password = "";

// -------- Hardware Config -------------
#define BUZZER_PIN /*26*/ 33
#define RING_PIN   2 /*27*/
#define RING_COUNT /*7*/ 24
//#define STRIP_PIN  33
//#define STRIP_COUNT 30
#define SS_PIN     5
#define RST_PIN    13

// -------- Global Objects --------------
WebServer server(80);
MFRC522 rfid(SS_PIN, RST_PIN);
Adafruit_NeoPixel ring(RING_COUNT, RING_PIN, NEO_GRB + NEO_KHZ800);
//Adafruit_NeoPixel strip(STRIP_COUNT, STRIP_PIN, NEO_GRB + NEO_KHZ800);

// -------- Alarm Schedule --------------
String dayTimes[7][20]; // increased capacity to 20 times/day
int dayCount[7] = {0};
const char* dayKeys[7] = {"sun","mon","tue","wed","thu","fri","sat"};

// -------- State Variables -------------
bool alarmActive = false;
unsigned long alarmStart = 0;
const unsigned long ALARM_TIMEOUT = 5 * 60 * 1000; // 5 min safety
String lastTriggeredTime = ""; // "HH:MM"
int lastTriggeredDay = -1;     // 0..6

// -------- BLE -------------------------
// Initialize all pointers
BLEServer* pServer = NULL;                        // Pointer to the server
BLECharacteristic* pCharacteristic_1 = NULL;      // Pointer to Characteristic 1
BLECharacteristic* pCharacteristic_2 = NULL;      // Pointer to Characteristic 2
BLEDescriptor *pDescr_1;                          // Pointer to Descriptor of Characteristic 1
BLE2902 *pBLE2902_1;                              // Pointer to BLE2902 of Characteristic 1
BLE2902 *pBLE2902_2;                              // Pointer to BLE2902 of Characteristic 2

// Some variables to keep track on device connected
bool deviceConnected = false;
bool oldDeviceConnected = false;

// Variable that will continuously be increased and written to the client
//uint8_t value = 0;
uint8_t lastValue = 255; 

#define SERVICE_UUID          "ab219760-9820-4be9-ba75-36a441f56e67"
#define CHARACTERISTIC_UUID_1 "ab29a8ea-6f7e-4d57-8eeb-e19b70ec9f68"
#define CHARACTERISTIC_UUID_2 "5ecbecb9-faef-4a23-96b6-3f84b577ccf9"

// -------- Time (NTP) ------------------
const char* ntpServer1 = "time.google.com";
const char* ntpServer2 = "time.nist.gov";

const char* tzString = "EST5EDT,M3.2.0/2,M11.1.0/2";
//const char* ntpServer = "pool.ntp.org";
//const long gmtOffset_sec = 0;
//const int daylightOffset_sec = 0;

// -------- Buzzer (LEDC) config --------
// we'll use channel 0, freq variable via ledcWriteTone
const int BUZZER_LEDC_CH = 0;
const int BUZZER_LEDC_FREQ = 2000; // default tone frequency

// -------- Embedded HTML (shorter UI; keep your previously working block if you prefer) ---
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>Reminder Scheduler</title>
<style>
:root{
  --bg:#06181a;--card:#0f2a2e;--muted:#94d2bd;--accent:#2ec4b6;--accent-2:#00a896;
  --text:#e6fffb;--sub:#b7f4e8;--chip:#07373c;
  --blue:#219ebc;--blue-light:#38c3da;
  --radius:16px;
}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui;color:var(--text);
background:linear-gradient(180deg,#031013,#06181a 40%,#041013)}
header{text-align:center;padding:28px 20px 10px}
.title{font-size:clamp(22px,4vw,32px);font-weight:700}
.subtitle{max-width:750px;margin:0 auto;color:var(--sub);opacity:.85;font-size:14px}
.container{max-width:980px;margin:22px auto 60px;padding:0 16px}
.grid{display:grid;gap:18px;grid-template-columns:repeat(auto-fit,minmax(450px,1fr));}
.card{background:linear-gradient(180deg,#103239,#0f2326);
border:1px solid rgba(255,255,255,.06);border-radius:var(--radius);box-shadow:0 6px 18px rgba(0,0,0,.35)}
.card .head{padding:16px 18px;border-bottom:1px solid rgba(255,255,255,.06)}
.card .head h2{margin:0;font-size:16px;font-weight:700}
.card .body{padding:18px}
.pill-days{display:grid;grid-template-columns:repeat(7,1fr);gap:8px}
.day{border:1px solid rgba(255,255,255,.08);background:#0b262a;color:var(--sub);
padding:10px 0;border-radius:10px;cursor:pointer;text-align:center;font-weight:600;
transition:.2s}
.day[aria-pressed="true"]{background:linear-gradient(180deg,var(--accent),var(--accent-2));
color:#012a2a;box-shadow:0 0 0 3px rgba(46,196,182,.35);}
.field{display:flex;align-items:center;gap:6px;background:#0b262a;
border:1px solid rgba(255,255,255,.1);border-radius:12px;padding:10px 12px;position:relative}
input[type="time"]{background:transparent;border:none;color:var(--text);font-size:16px;width:130px;outline:none}
.btn{border:none;cursor:pointer;padding:10px 14px;font-weight:700;border-radius:12px;transition:.15s}
.btn.teal{background:linear-gradient(180deg,#3ed7c1,#22a593);color:#012a2a;}
.btn.save{background:linear-gradient(180deg,#3fa9f5,#219ebc);color:#012a2a;}
.btn.load{background:linear-gradient(180deg,#42d4e0,#38c3da);color:#012a2a;}
.schedule{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(130px,1fr))}
.daycard{background:var(--card);border:1px solid rgba(255,255,255,.06);border-radius:14px;
padding:12px;display:flex;flex-direction:column;gap:10px}
.daycard h3{margin:0;font-size:13px;text-transform:uppercase;color:var(--muted)}
.chips{display:flex;flex-wrap:wrap;gap:8px}
.chip{background:var(--chip);border:1px solid rgba(255,255,255,.1);border-radius:999px;
padding:6px 10px;font-weight:700;font-size:13px;display:inline-flex;align-items:center;gap:8px}
.chip button{border:none;cursor:pointer;width:18px;height:18px;border-radius:50%;
font-weight:900;color:#062d31;background:var(--muted)}
</style>
</head>
<body>
<header>
  <h1 class="title">Reminder Scheduler</h1>
  <p class="subtitle">Set daily reminders. Pick a time, choose days, and add them to your schedule.</p>
</header>
<main class="container">
  <div class="grid">
    <section class="card">
      <div class="head"><h2>Build a Reminder</h2></div>
      <div class="body">
        <label class="hint">Pick a time</label>
        <div class="field">
          <input id="time" type="time" value="08:00">
        </div>
        <div class="hint" style="margin-top:14px;">Select days</div>
        <div class="pill-days" style="margin-top:6px;">
          <button class="day" data-day="sun">Sun</button>
          <button class="day" data-day="mon">Mon</button>
          <button class="day" data-day="tue">Tue</button>
          <button class="day" data-day="wed">Wed</button>
          <button class="day" data-day="thu">Thu</button>
          <button class="day" data-day="fri">Fri</button>
          <button class="day" data-day="sat">Sat</button>
        </div>
        <div style="margin-top:12px;display:flex;gap:8px;flex-wrap:wrap;">
          <button id="select-all" class="btn save">Select All</button>
          <button id="add-time" class="btn teal">Add</button>
        </div>
      </div>
    </section>
    <section class="card">
      <div class="head"><h2>Device Controls</h2></div>
      <div class="body">
        <button id="save" class="btn save">Save to Device</button>
        <button id="load" class="btn load">Load Schedule</button>
      </div>
    </section>
  </div>
  <section class="card" style="margin-top:18px;">
    <div class="head"><h2>Per-Day Schedule</h2></div>
    <div class="body"><div id="schedule" class="schedule"></div></div>
  </section>
</main>
<script>
const DAYS=["sun","mon","tue","wed","thu","fri","sat"];
const LABELS={sun:"Sunday",mon:"Monday",tue:"Tuesday",wed:"Wednesday",thu:"Thursday",fri:"Friday",sat:"Saturday"};
const schedule=Object.fromEntries(DAYS.map(d=>[d,[]]));
const $=s=>document.querySelector(s),$$=s=>[...document.querySelectorAll(s)];
function render(){
  const wrap=$("#schedule");wrap.innerHTML="";
  for(const d of DAYS){
    const card=document.createElement("div");card.className="daycard";
    card.innerHTML=`<h3>${LABELS[d]}</h3>`;
    const chips=document.createElement("div");chips.className="chips";
    if(!schedule[d].length){chips.innerHTML="<span style='opacity:.6'>No times</span>";}
    for(const t of schedule[d]){
      const chip=document.createElement("span");chip.className="chip";chip.textContent=t;
      const btn=document.createElement("button");btn.textContent="×";btn.onclick=()=>{schedule[d]=schedule[d].filter(x=>x!==t);render();};
      chip.appendChild(btn);chips.appendChild(chip);
    }
    card.appendChild(chips);wrap.appendChild(card);
  }
}
function getSelectedDays(){return $$(".day[aria-pressed='true']").map(b=>b.dataset.day);}
$("#add-time").onclick=()=>{
  const t=$("#time").value;
  const days=getSelectedDays();if(!days.length)return alert("Select days first");
  for(const d of days){if(!schedule[d].includes(t))schedule[d].push(t);}
  render();
};
$("#save").onclick=()=>fetch("/api/schedule",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(schedule)});
$("#load").onclick=()=>fetch("/api/schedule").then(r=>r.json()).then(j=>{Object.assign(schedule,j);render();});
$$(".day").forEach(b=>b.onclick=()=>b.setAttribute("aria-pressed",b.getAttribute("aria-pressed")!=="true"));
$("#select-all").onclick=()=>$$(".day").forEach(b=>b.setAttribute("aria-pressed","true"));
render();
</script>
</body>
</html>
)rawliteral";

// Callback function that is called whenever a client is connected or disconnected
/*class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};*/
class MyServerCallbacks: public BLEServerCallbacks {
  public:
    void onConnect(BLEServer* pServer) override {
      deviceConnected = true;
      Serial.println("BLE: client connected");
    }
    void onDisconnect(BLEServer* pServer) override {
      deviceConnected = false;
      Serial.println("BLE: client disconnected");
    }
};

// ---------- Helper functions ------------
// Simple tone() fallback — reliable and portable
void startBuzzerTone(int freq) {
  // tone() is available on Arduino for ESP32; it will start generating a PWM tone on the pin.
  // If you have a passive piezo, this will produce an audible tone.
  tone(BUZZER_PIN, freq);
}

void stopBuzzerTone() {
  noTone(BUZZER_PIN);
}


void startAlarm() {
  if (alarmActive) return;
  alarmActive = true;
  alarmStart = millis();
  Serial.println("⏰ Alarm Activated!");
  // immediate visual & sound start
  for (int i = 0; i < RING_COUNT; i++) ring.setPixelColor(i, ring.Color(255, 0, 0));
  //for (int i = 0; i < STRIP_COUNT; i++) strip.setPixelColor(i, strip.Color(255, 0, 0));
  ring.show(); //strip.show();
  startBuzzerTone(1500);
}

void stopAlarm() {
  if (!alarmActive) return;
  alarmActive = false;
  Serial.println("✅ Alarm Stopped");
  ring.clear(); //strip.clear();
  ring.show(); //strip.show();
  stopBuzzerTone();
}

// small toggling visual pattern while alarmActive (non-blocking-ish)
void alarmVisualStep() {
  static unsigned long last = 0;
  static bool on = false;
  unsigned long now = millis();
  if (now - last < 250) return;
  last = now;
  on = !on;
  if (on) {
    for (int i = 0; i < RING_COUNT; i++) ring.setPixelColor(i, ring.Color(255, 0, 0));
    //for (int i = 0; i < STRIP_COUNT; i++) strip.setPixelColor(i, strip.Color(255, 0, 0));
    ring.show(); //strip.show();
    startBuzzerTone(1500);
  } else {
    ring.clear(); //strip.clear();
    ring.show(); //strip.show();
    stopBuzzerTone();
  }
}

String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time (NTP not synced yet)");
    return "";
  }
  char buf[6];
  sprintf(buf, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  return String(buf);
}

/*String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "";
  char buf[6];
  sprintf(buf, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  return String(buf);
}*/

int getDayIndex() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return -1;
  return timeinfo.tm_wday; // 0=Sun .. 6=Sat
}

// ---------- Schedule check ----------
void checkSchedule() {
  static String lastChecked = "";
  String now = getTimeString();
  if (now == "" || now == lastChecked) return;
  lastChecked = now;
  int wd = getDayIndex();
  if (wd < 0 || wd > 6) return;
  // iterate through today's entries
  for (int i = 0; i < dayCount[wd]; i++) {
    String t = dayTimes[wd][i];
    if (t == now) {
      Serial.printf("🎯 Match %s -> Alarm\n", now.c_str());
      lastTriggeredTime = t;
      lastTriggeredDay = wd;
      startAlarm();
      break; // only trigger once per minute
    }
  }
}

// ---------- NFC: disarm & remove triggered time ----------
void checkNFC() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  Serial.println("🔑 NFC Tag Detected");
  // If an alarm is active and we have a recorded triggered time for today, remove it
  if (alarmActive && lastTriggeredDay >= 0 && lastTriggeredTime.length() == 5) {
    int d = lastTriggeredDay;
    bool removed = false;
    for (int i = 0; i < dayCount[d]; i++) {
      if (dayTimes[d][i] == lastTriggeredTime) {
        Serial.printf("Removing triggered time %s from day %s\n", lastTriggeredTime.c_str(), dayKeys[d]);
        // shift left
        for (int j = i; j < dayCount[d] - 1; j++) dayTimes[d][j] = dayTimes[d][j+1];
        dayCount[d]--;
        removed = true;
        break;
      }
    }
    if (!removed) {
      Serial.println("Triggered time not found in today's schedule (maybe already removed).");
    }
  }
  // stop the alarm regardless
  stopAlarm();
  // green flash acknowledgement
  for (int i = 0; i < RING_COUNT; i++) ring.setPixelColor(i, ring.Color(0,255,0));
  //for (int i = 0; i < STRIP_COUNT; i++) strip.setPixelColor(i, strip.Color(0,255,0));
  ring.show(); //strip.show();
  delay(450);
  ring.clear(); /*strip.clear();*/ ring.show(); //strip.show();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ---------- HTTP handlers (root & schedule) ----------
void handleRoot() { server.send_P(200, "text/html", index_html); }

void handleGetSchedule() {
  StaticJsonDocument<4096> doc;
  for (int d = 0; d < 7; d++) {
    JsonArray arr = doc.createNestedArray(dayKeys[d]);
    for (int i = 0; i < dayCount[d]; i++) arr.add(dayTimes[d][i]);
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Debug-friendly POST handler (same as last working for you)
void handlePostSchedule() {
  if (server.method() == HTTP_OPTIONS) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send (204, "text/plain", "");
    return;
  }

  Serial.println("=== /api/schedule POST ===");
  String body;
  if (server.hasArg("plain")) {
    body = server.arg("plain");
    Serial.println("Got body from server.arg(\"plain\")");
  } else if (server.args() > 0) {
    body = server.arg(0);
    Serial.println("Got body from server.arg(0)");
  } else {
    Serial.println("No args; trying to read client stream...");
    WiFiClient client = server.client();
    while (client.available()) body += (char)client.read();
  }

  Serial.printf("Raw body len=%u\n", body.length());
  if (body.length() == 0) {
    server.send(400, "application/json", "{\"status\":\"error\",\"reason\":\"empty body\"}");
    return;
  }

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.print("JSON parse error: "); Serial.println(err.c_str());
    server.send(400, "application/json", "{\"status\":\"error\",\"reason\":\"bad json\"}");
    return;
  }

  // copy into dayTimes/dayCount (clear first)
  for (int d = 0; d < 7; d++) {
    dayCount[d] = 0;
    JsonArray arr = doc[dayKeys[d]].as<JsonArray>();
    if (!arr.isNull()) {
      for (JsonVariant v : arr) {
        if (dayCount[d] < 20) { // respect new capacity
          const char* t = v.as<const char*>();
          dayTimes[d][dayCount[d]++] = String(t);
        }
      }
    }
  }

  // debug print
  for (int d = 0; d < 7; d++) {
    Serial.print(dayKeys[d]); Serial.println(":");
    for (int i = 0; i < dayCount[d]; i++) {
      Serial.print("  "); Serial.println(dayTimes[d][i]);
    }
  }

  if (pCharacteristic_2) pCharacteristic_2->setValue(body.c_str());
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", "{\"status\":\"ok\",\"saved\":true}");
}

// ---------- setup & loop ----------
void setup() {
  Serial.begin(115200);
  delay(50);

  // Create the BLE Device
  BLEDevice::init("ESP32");

  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create the BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create a BLE Characteristic
  pCharacteristic_1 = pService->createCharacteristic(
                      CHARACTERISTIC_UUID_1,
                      BLECharacteristic::PROPERTY_NOTIFY
                    );                   

  pCharacteristic_2 = pService->createCharacteristic(
                      CHARACTERISTIC_UUID_2,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_WRITE  |                      
                      BLECharacteristic::PROPERTY_NOTIFY
                    );  

  // Create a BLE Descriptor  

  /*pDescr_1 = new BLEDescriptor((uint16_t)0x2901);
  pDescr_1->setValue("A very interesting variable");
  pCharacteristic_1->addDescriptor(pDescr_1);*/
  pCharacteristic_1->addDescriptor(new BLEDescriptor(BLEUUID((uint16_t)0x2901)));
  //pCharacteristic_1->addDescriptor(new BLE2902());

  // Add the BLE2902 Descriptor because we are using "PROPERTY_NOTIFY"
  pBLE2902_1 = new BLE2902();
  pBLE2902_1->setNotifications(true);                 
  pCharacteristic_1->addDescriptor(pBLE2902_1);
  

  pBLE2902_2 = new BLE2902();
  pBLE2902_2->setNotifications(true);
  pCharacteristic_2->addDescriptor(pBLE2902_2);

  // Start the service
  pService->start();

  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0);  // set value to 0x00 to not advertise this parameter
  BLEDevice::startAdvertising();
  Serial.println("Waiting a client connection to notify...");

  // init hardware
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  ring.begin(); ring.setBrightness(80); ring.clear(); ring.show();
  //strip.begin(); strip.setBrightness(80); strip.clear(); strip.show();
  SPI.begin(); rfid.PCD_Init();

  // WiFi connect
  Serial.printf("Connecting to %s", ssid);
  WiFi.begin(ssid, password);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n❌ WiFi connect failed.");
  }

  // NTP
  configTime(0, 0, ntpServer1, ntpServer2);   // start NTP client
  setenv("TZ", tzString, 1);                 // set time‑zone string
  tzset();

  /*configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("NTP setup done.");*/

  // BLE (same as before)
  /*BLEDevice::init("ESP32-Reminder");
  pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic_2 = pService->createCharacteristic(
    CHARACTERISTIC_UUID_2,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
  pService->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  BLEDevice::startAdvertising();*/

  // Web routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/schedule", HTTP_GET, handleGetSchedule);
  server.on("/api/schedule", HTTP_POST, handlePostSchedule);
  server.on("/api/schedule", HTTP_OPTIONS, [](){
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send(204);
  });
  server.on("/arm", HTTP_GET, [](){ startAlarm(); server.sendHeader("Location","/"); server.send(303); });
  server.on("/disarm", HTTP_GET, [](){ stopAlarm(); server.sendHeader("Location","/"); server.send(303); });
  server.begin();
  Serial.println("Web server started.");
}

void loop() {
  server.handleClient();
  // BLE characteristic ping / bookkeeping (kept minimal)
  if (deviceConnected) {
    // notify a small counter if you want...
    
  }

  checkNFC();
  checkSchedule();

  uint16_t value = alarmActive ? 1 : 0;
  if (value != lastValue) {
    Serial.println(value);
    pCharacteristic_1->setValue(value);
    pCharacteristic_1->notify();
    lastValue = value;
  }
  //pCharacteristic_1->setValue(&value, 1); // pass address + length
  //pCharacteristic_1->notify();            // notify client

  if (alarmActive) {
    alarmVisualStep();
    if (millis() - alarmStart > ALARM_TIMEOUT) {
      Serial.println("Alarm auto-stop (timeout)");
      stopAlarm();
    }
  }

  delay(10);
}
