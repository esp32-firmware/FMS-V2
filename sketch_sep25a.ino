// =================================================================================
// FEES MANAGEMENT SYSTEM - DUAL CORE EDITION (Mochi UI + FMS Engine)
// BATCH UPLOAD + SECURE OTA + STABLE HOTSPOT HANDOFF + SAFE QUEUE
// NEW: 60-Second AP Timer + Captive Portal Intercept + Spinlock Safety
// =================================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h> 
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h> 
#include <RTClib.h>
#include <Adafruit_INA219.h>
#include <LittleFS.h>
#include <WiFiManager.h>
#include <DNSServer.h>
#include "time.h"
#include <map> 
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Fonts/FreeSansBold9pt7b.h> 
#include <Fonts/FreeSans9pt7b.h>       
#include <Fonts/FreeSansBold12pt7b.h> 

// ---------------------------------------------------------------------------------
// PIN DEFINITIONS & CONSTANTS (FMS Hardware)
// ---------------------------------------------------------------------------------
#define RST_PIN         4
#define SS_PIN          5
#define BTN_PREV        32
#define BTN_NEXT        33
#define BTN_SYNC        25
#define LED_GREEN       13
#define LED_RED         14  
#define BUZZER_PIN      27
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1

const long    gmtOffset_sec      = 19800; // IST Offset (UTC +5:30)
const int     daylightOffset_sec = 0;

String configUrl = "Paste Appscript url here";
String configName = "Your Name";
String configUser = "admin";
String configPass = "1234";

// ---------------------------------------------------------------------------------
// GLOBAL OBJECTS & VARIABLES
// ---------------------------------------------------------------------------------
MFRC522 rfid(SS_PIN, RST_PIN);
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
RTC_DS3231 rtc;
Adafruit_INA219 ina219(0x40); 
WebServer server(80);

SemaphoreHandle_t fsMutex;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED; 
TaskHandle_t backgroundTaskHandle;

unsigned long bootTime = 0;        
unsigned long lastScreenDraw = 0; 

// ---------------------------------------------------------------------------------
// CLOUD OTA VARIABLES
// ---------------------------------------------------------------------------------
String currentFirmwareVersion = "1.1";  //#########################################$$$$$$$$$$$$$$$$$##########%%%%%%%%%%%%%%%########################//
volatile int updateProgressPercent = 0;
volatile bool hasCheckedUpdate = false; // One-shot OTA flag

// Power & State Management
enum SystemState { AWAKE, SLEEPY, MOCHI_WAKE, ALARM, UPDATING };
volatile SystemState sysState = AWAKE;
unsigned long lastActivityTime = 0;
unsigned long stateTimer = 0; 
volatile float globalVoltage = 0.0;
volatile int globalBatPercent = 0;

// Shared State Variables (Protected by stateMux)
volatile bool isHotspotActive = false; 
volatile bool isPortalRequested = false; // Triggers Core 1 Portal Handoff
volatile bool isManualSyncing = false;
volatile bool needsAutoUpload = false;
volatile int wifiSignalBars = 0;
volatile int syncProgress = 0;
volatile int batchTotal = 0;
volatile int batchDone = 0;
volatile bool notifyUploadSuccess = false;
volatile bool wasUploading = false;

// Non-Blocking Hardware Timers
unsigned long buzzerOffTime = 0;
unsigned long ledOffTime = 0;

// UI Messaging
String currentMessage = "Ready";
String displayStudentName = ""; 
unsigned long lastMessageTime = 0;
String systemError = "OK";

// FMS Specific Variables
int selectedMonth = 1;
const char* monthNames[] = {"", "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

// RAM Storage
std::map<String, String> studentCache; 
std::map<String, unsigned long> recentScans; 
std::map<String, int> sessionPaidCache;      

// ---------------------------------------------------------------------------------
// OTA CALLBACKS
// ---------------------------------------------------------------------------------
void ota_onStart() {}
void ota_onEnd() {}
void ota_onProgress(int current, int total) {
  updateProgressPercent = (current * 100) / total;
  vTaskDelay(10 / portTICK_PERIOD_MS); 
}
void ota_onError(int err) {}

// ---------------------------------------------------------------------------------
// CONFIGURATION MANAGERS
// ---------------------------------------------------------------------------------
void loadSystemConfig() {
  xSemaphoreTake(fsMutex, portMAX_DELAY);
  File f = LittleFS.open("/config.txt", "r");
  if (f) {
    configUrl = f.readStringUntil('\n'); configUrl.trim();
    configName = f.readStringUntil('\n'); configName.trim();
    configUser = f.readStringUntil('\n'); configUser.trim();
    configPass = f.readStringUntil('\n'); configPass.trim();
    f.close();
    if(configUser == "") configUser = "admin";
    if(configPass == "") configPass = "1234";
  } else {
    f = LittleFS.open("/config.txt", "w");
    f.println(configUrl); f.println(configName); f.println(configUser); f.println(configPass);
    f.close();
  }
  xSemaphoreGive(fsMutex);
}

void saveSystemConfig() {
  xSemaphoreTake(fsMutex, portMAX_DELAY);
  File f = LittleFS.open("/config.txt", "w");
  f.println(configUrl); f.println(configName); f.println(configUser); f.println(configPass);
  f.close();
  xSemaphoreGive(fsMutex);
}

void loadStudentCache() {
  xSemaphoreTake(fsMutex, portMAX_DELAY);
  studentCache.clear();
  File file = LittleFS.open("/students.csv", "r");
  if (file) {
    String line;
    line.reserve(64); 
    while (file.available()) {
      line = file.readStringUntil('\n'); line.trim();
      int commaIndex = line.indexOf(',');
      if (commaIndex != -1) studentCache[line.substring(0, commaIndex)] = line.substring(commaIndex + 1);
    }
    file.close();
  }
  xSemaphoreGive(fsMutex);
}

// ---------------------------------------------------------------------------------
// ASYNC HARDWARE FEEDBACK & LOGIC
// ---------------------------------------------------------------------------------
void triggerBuzzer(int duration) {
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerOffTime = millis() + duration;
}

void triggerLed(int pin, int duration) {
  analogWrite(pin, 255);
  ledOffTime = millis() + duration;
}

void updateBreathingLED(int pin) {
  if(ledOffTime > millis()) return; 
  float pulse = (sin(millis() / 636.6) + 1.0) / 2.0; 
  int pwm = pulse * pulse * 255; 
  analogWrite(pin, pwm); 
}

void updateSleepBreathingLED(int pin) {
  if(ledOffTime > millis()) return;
  float pulse = (sin(millis() / 800.0) + 1.0) / 2.0; 
  int pwm = pulse * pulse * pulse * 30; 
  analogWrite(pin, pwm); 
}

void processAsyncHardware() {
  unsigned long now = millis();
  if (buzzerOffTime > 0 && now >= buzzerOffTime) {
    digitalWrite(BUZZER_PIN, LOW); buzzerOffTime = 0;
  }
  if (ledOffTime > 0 && now >= ledOffTime) {
    analogWrite(LED_RED, 0); analogWrite(LED_GREEN, 0); ledOffTime = 0;
  }
}

bool isAnyButtonPressed() {
  return (digitalRead(BTN_PREV) == LOW || digitalRead(BTN_NEXT) == LOW || digitalRead(BTN_SYNC) == LOW);
}

// ---------------------------------------------------------------------------------
// MOCHI ROBOT UI ENGINE 
// ---------------------------------------------------------------------------------
unsigned long nextBlinkTime = 0;
bool isBlinking = false;
float currentEyeHeight = 3.0; 
unsigned long nextLookTime = 0;
int lookOffsetX = 0, lookOffsetY = 0;

void drawMochiFace(int state) {
  int cx = 64; int cy = 32; 
  unsigned long currentMillis = millis();

  if (state == 0) { 
    if (currentMillis > nextBlinkTime && !isBlinking) {
      isBlinking = true;
      nextBlinkTime = currentMillis + random(2000, 5000); 
    }
    if (isBlinking) {
      currentEyeHeight -= 8.0; 
      if (currentEyeHeight <= 3.0) { currentEyeHeight = 3.0; isBlinking = false; }
    } else {
      if (currentEyeHeight < 24.0) currentEyeHeight += 6.0; 
      if (currentEyeHeight > 24.0) currentEyeHeight = 24.0;
    }
    if (currentMillis > nextLookTime && currentEyeHeight > 20.0) {
      lookOffsetX = random(-4, 5); lookOffsetY = random(-2, 3); 
      nextLookTime = currentMillis + random(1000, 3500);
    } 
    if (currentEyeHeight < 15.0) { lookOffsetX = 0; lookOffsetY = 0; } 

    int eyeY = cy - (currentEyeHeight / 2) + lookOffsetY;
    display.fillRoundRect(cx - 22 + lookOffsetX, eyeY, 14, (int)currentEyeHeight, 4, SH110X_WHITE);
    display.fillRoundRect(cx + 8 + lookOffsetX, eyeY, 14, (int)currentEyeHeight, 4, SH110X_WHITE);
    display.fillCircle(cx + lookOffsetX, cy + 12 + lookOffsetY, 5, SH110X_WHITE);
    display.fillCircle(cx + lookOffsetX, cy + 10 + lookOffsetY, 6, SH110X_BLACK); 
  } else if (state == 1) { 
    int yBounce = (cos(millis() / 1000.0) * 4); 
    display.fillRoundRect(cx - 22, cy + 4 + yBounce, 14, 4, 2, SH110X_WHITE);
    display.fillRoundRect(cx + 8, cy + 4 + yBounce, 14, 4, 2, SH110X_WHITE);
    display.fillCircle(cx, cy + 12 + yBounce, 5, SH110X_WHITE);
    display.fillCircle(cx, cy + 10 + yBounce, 6, SH110X_BLACK);
    display.setTextSize(1); display.setFont();
    display.setCursor(cx + 35, cy - 20 + yBounce); display.print("Z");
    display.setCursor(cx + 45, cy - 28 + yBounce); display.print("z");
  }
}

// ---------------------------------------------------------------------------------
// STARTUP ANIMATION (ADM STYLE - "FEES MACHINE")
// ---------------------------------------------------------------------------------
void showPremiumStartup() {
  unsigned long start = millis();
  currentEyeHeight = 3.0; isBlinking = false;
  
  while(millis() - start < 3000) { display.clearDisplay(); drawMochiFace(0); display.display(); delay(30); }
  for(int i=0; i<2; i++) {
     currentEyeHeight = 3.0; display.clearDisplay(); drawMochiFace(0); display.display(); delay(80);
     currentEyeHeight = 24.0; lookOffsetX = random(-5, 6); lookOffsetY = random(-3, 4);
     display.clearDisplay(); drawMochiFace(0); display.display(); delay(150);
  }
  lookOffsetX = 0; lookOffsetY = 0; currentEyeHeight = 24.0;
  display.clearDisplay(); drawMochiFace(0); display.display(); delay(200);

  display.clearDisplay(); display.setTextColor(SH110X_WHITE);
  int16_t x1, y1; uint16_t w, h; display.setFont(&FreeSansBold9pt7b);
  
  display.getTextBounds("FEES", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 24); display.print("FEES");
  display.getTextBounds("MACHINE", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 42); display.print("MACHINE");

  display.setFont(); String bottomText = "- " + configName + " -";
  display.getTextBounds(bottomText, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 55); display.print(bottomText);
  display.display(); delay(1500);
}

// ---------------------------------------------------------------------------------
// SECURE WEB PORTAL (FMS HTML)
// ---------------------------------------------------------------------------------
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Fees Machine Admin</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    :root { --bg: #0f172a; --card: #1e293b; --text: #e2e8f0; --accent: #3b82f6; --accent-hover: #2563eb; --danger: #ef4444; }
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 20px; }
    .container { width: 100%; box-sizing: border-box; }
    .card { background: var(--card); border-radius: 12px; padding: 24px; box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1), 0 2px 4px -1px rgba(0, 0, 0, 0.06); margin-bottom: 20px; width: 100%; box-sizing: border-box; }
    h2, h3 { margin-top: 0; color: #fff; }
    input { width: 100%; padding: 12px; margin: 8px 0 16px; background: #0f172a; color: white; border: 1px solid #334155; border-radius: 8px; box-sizing: border-box; outline: none; transition: border 0.2s; }
    input:focus { border-color: var(--accent); }
    button { width: 100%; padding: 12px; background: var(--accent); color: white; border: none; border-radius: 8px; font-weight: 600; cursor: pointer; transition: background 0.2s; }
    button:hover { background: var(--accent-hover); }
    .btn-danger { background: var(--danger); }
    .btn-danger:hover { background: #dc2626; }
    table { width: 100%; border-collapse: collapse; margin-top: 10px; }
    th, td { padding: 12px; text-align: left; border-bottom: 1px solid #334155; }
    th { color: #94a3b8; text-transform: uppercase; font-size: 12px; font-weight: 600; }
    .tabs { display: flex; gap: 10px; margin-bottom: 20px; flex-wrap: wrap; }
    .tab-btn { background: #334155; flex: 1; min-width: 110px; padding: 10px; font-size: 14px; }
    .tab-btn.active { background: var(--accent); }
    .tab-content { display: none; }
    .tab-content.active { display: block; }
    #loginScreen { display: block; margin-top: 5vh; max-width: 500px; margin-left: auto; margin-right: auto; }
    #dashboard { display: none; }
    .toast { color: #10b981; font-size: 14px; margin-top: 10px; text-align: center; height: 20px; }
    label { font-size: 13px; color: #94a3b8; font-weight: 600; margin-bottom: 4px; display: block; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; }
    .stat-card { background: #0f172a; padding: 15px; border-radius: 8px; }
    .status-dot { height: 8px; width: 8px; border-radius: 50%; display: inline-block; margin-right: 6px; }
  </style>
</head>
<body>
  <div class="container">
    <div id="loginScreen" class="card">
      <h2 style="text-align: center; margin-bottom: 20px;">FMS System Login</h2>
      <label>Username</label>
      <input type="text" id="loginUser" placeholder="Enter Admin Username">
      <label>Password</label>
      <input type="password" id="loginPass" placeholder="Enter Password">
      <button onclick="doLogin()">Access Portal</button>
      <div id="loginMsg" style="color:var(--danger); text-align:center; margin-top:10px;"></div>
    </div>

    <div id="dashboard">
      <div class="tabs">
        <button class="tab-btn active" onclick="switchTab('creds')" id="btn-creds">Credentials</button>
        <button class="tab-btn" onclick="switchTab('add')" id="btn-add">Add Student</button>
        <button class="tab-btn" onclick="switchTab('sync')" id="btn-sync">Synced DB</button>
        <button class="tab-btn" onclick="switchTab('ctrl'); fetchHealth();" id="btn-ctrl">Control Panel</button>
      </div>

      <div id="tab-creds" class="tab-content active card">
        <h3>Network & Admin Settings</h3>
        <label>Admin Username</label>
        <input type="text" id="cfgUser" placeholder="admin">
        <label>Portal Password</label>
        <input type="password" id="cfgPass" placeholder="Change Password (Optional)">
        <label>Machine Name</label>
        <input type="text" id="cfgName" placeholder="Your Name">
        <label>Google AppScript URL</label>
        <input type="text" id="cfgUrl" placeholder="Paste Appscript url here">
        <button class="btn-danger" onclick="saveSettings()">Save Configuration & Restart</button>
        <div id="cfgMsg" class="toast"></div>
      </div>

      <div id="tab-add" class="tab-content card">
        <h3>Manual Card Registration</h3>
        <label>Card UID</label>
        <input type="text" id="uid" placeholder="e.g. A1B2C3D4">
        <label>Student Name</label>
        <input type="text" id="name" placeholder="Full Name">
        <button onclick="addStudent()">Register Card</button>
        <div id="addMsg" class="toast"></div>
      </div>

      <div id="tab-sync" class="tab-content card">
        <h3>Local Machine Database</h3>
        <button onclick="loadStudents()" style="margin-bottom:15px;">Refresh Database</button>
        <div style="max-height: 60vh; overflow-y: auto;">
            <table>
              <thead><tr><th>#</th><th>UID</th><th>Name</th></tr></thead>
              <tbody id="studentTable"><tr><td colspan="3">Loading...</td></tr></tbody>
            </table>
        </div>
      </div>

      <div id="tab-ctrl" class="tab-content card">
        <h3>System Diagnostics & Health</h3>
        <button onclick="fetchHealth()" style="margin-bottom:15px;">Run Health Diagnostics</button>
        <div class="grid">
          <div class="stat-card">
            <h4 style="margin:0 0 10px; color:#94a3b8;">Power System</h4>
            <h2 style="margin:0; color:#10b981;" id="diagVolt">-- V</h2>
            <p style="margin:5px 0 0; font-size:12px; color:#cbd5e1;" id="diagBat">Battery: --%</p>
          </div>
          <div class="stat-card">
            <h4 style="margin:0 0 10px; color:#94a3b8;">System Clock</h4>
            <h3 style="margin:0; color:#e2e8f0;" id="diagTime">--:--:--</h3>
            <p style="margin:5px 0 0; font-size:12px; color:#cbd5e1;" id="diagDate">--/--/----</p>
          </div>
          <div class="stat-card" style="grid-column: 1 / -1;">
            <h4 style="margin:0 0 10px; color:#94a3b8;">Live Hardware Status</h4>
            <table style="margin-top:0;">
              <tbody id="diagHw">
                <tr><td>Running diagnostics...</td></tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </div>
  </div>
  <script>
    let authHeader = '';

    function switchTab(tab) {
      document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
      document.querySelectorAll('.tab-btn').forEach(el => el.classList.remove('active'));
      document.getElementById('tab-' + tab).classList.add('active');
      document.getElementById('btn-' + tab).classList.add('active');
    }

    function doLogin() {
      let u = document.getElementById('loginUser').value;
      let p = document.getElementById('loginPass').value;
      authHeader = 'Basic ' + btoa(u + ':' + p);
      
      fetch('/api/settings', { headers: { 'Authorization': authHeader } })
        .then(res => {
          if(res.ok) {
            document.getElementById('loginScreen').style.display = 'none';
            document.getElementById('dashboard').style.display = 'block';
            return res.json();
          } else throw new Error('Auth failed');
        })
        .then(data => {
          document.getElementById('cfgUser').value = data.user;
          document.getElementById('cfgName').value = data.name;
          document.getElementById('cfgUrl').value = data.url;
          loadStudents();
        })
        .catch(err => document.getElementById('loginMsg').innerText = "Invalid credentials.");
    }

    function saveSettings() {
      let confirmPass = prompt("Security Check: Enter CURRENT admin password to authorize changes:");
      if(confirmPass === null) return;

      let u = document.getElementById('cfgUser').value;
      let p = document.getElementById('cfgPass').value;
      let n = document.getElementById('cfgName').value;
      let url = document.getElementById('cfgUrl').value;
      
      let body = `user=${encodeURIComponent(u)}&name=${encodeURIComponent(n)}&url=${encodeURIComponent(url)}&confirmPass=${encodeURIComponent(confirmPass)}`;
      if(p) body += `&pass=${encodeURIComponent(p)}`;

      fetch('/api/settings', { 
        method: 'POST', 
        headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Authorization': authHeader }, 
        body: body 
      }).then(res => {
          if(res.ok) {
             document.getElementById('cfgMsg').style.color = '#10b981';
             document.getElementById('cfgMsg').innerText = "Saved successfully! Rebooting...";
             setTimeout(() => location.reload(), 4000);
          } else {
             document.getElementById('cfgMsg').style.color = 'var(--danger)';
             document.getElementById('cfgMsg').innerText = "Failed! Incorrect authorization password.";
             setTimeout(() => document.getElementById('cfgMsg').innerText = '', 3000);
          }
      });
    }

    function loadStudents() {
      fetch('/api/students', { headers: { 'Authorization': authHeader } })
        .then(res => res.json())
        .then(data => {
          let arr = Object.keys(data).map(k => ({ uid: k, name: data[k] }));
          arr.sort((a, b) => a.name.localeCompare(b.name));
          let rows = arr.map((s, i) => `<tr><td>${i+1}</td><td>${s.uid}</td><td>${s.name}</td></tr>`).join('');
          document.getElementById('studentTable').innerHTML = rows || '<tr><td colspan="3">Database Empty</td></tr>';
        });
    }

    function addStudent() {
      let u = document.getElementById('uid').value; let n = document.getElementById('name').value;
      if(!u || !n) return alert("Fill both fields");
      fetch('/api/add', { 
        method: 'POST', 
        headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Authorization': authHeader }, 
        body: `uid=${encodeURIComponent(u)}&name=${encodeURIComponent(n)}` 
      }).then(res => {
        if(res.ok) {
          document.getElementById('addMsg').innerText = "Added Successfully!";
          document.getElementById('uid').value = '';
          document.getElementById('name').value = '';
          setTimeout(() => document.getElementById('addMsg').innerText = '', 3000);
          loadStudents(); 
        }
      });
    }

    function fetchHealth() {
      fetch('/api/health', { headers: { 'Authorization': authHeader } })
        .then(res => res.json())
        .then(data => {
          document.getElementById('diagVolt').innerText = data.voltage + ' V';
          document.getElementById('diagBat').innerText = 'Battery: ' + (data.battery === -1 ? 'USB Power Mode' : data.battery + '%');
          
          let dt = data.time.split(' ');
          if(dt.length === 2) {
             document.getElementById('diagDate').innerText = dt[0];
             document.getElementById('diagTime').innerText = dt[1];
          }
          
          const hardware = [
            { name: 'MFRC522 RFID Core (SPI)', status: data.hw_rfid },
            { name: 'DS3231 Real Time Clock (I2C)', status: data.hw_rtc },
            { name: 'INA219 Power Monitor (I2C)', status: data.hw_ina },
            { name: 'OLED Display Output (I2C)', status: data.hw_oled },
            { name: 'LittleFS Storage Engine', status: data.hw_fs }
          ];
          
          let rows = '';
          hardware.forEach(hw => {
            let color = hw.status ? '#10b981' : '#ef4444';
            let label = hw.status ? 'ONLINE' : 'ERROR';
            rows += `<tr>
              <td style="padding:10px 12px; border-bottom:1px solid #1e293b;">${hw.name}</td>
              <td style="padding:10px 12px; border-bottom:1px solid #1e293b; text-align:right;">
                <span style="color:${color}; font-size:12px; font-weight:700; letter-spacing:1px;">
                  <span class="status-dot" style="background:${color};"></span>${label}
                </span>
              </td>
            </tr>`;
          });
          document.getElementById('diagHw').innerHTML = rows;
        });
    }
  </script>
</body>
</html>
)rawliteral";

// ---------------------------------------------------------------------------------
// WEB SERVER ENDPOINTS (INTEGRATED CAPTIVE PORTAL)
// ---------------------------------------------------------------------------------
void setupWebEndpoints() {
  server.on("/", HTTP_GET, []() { 
    bool loc_ap;
    portENTER_CRITICAL(&stateMux); loc_ap = isHotspotActive; portEXIT_CRITICAL(&stateMux);
    
    if (loc_ap) {
      String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
      html += "<meta http-equiv='refresh' content='0;url=/'></head>";
      html += "<body style='background:#121212;color:#fff;text-align:center;font-family:sans-serif;padding-top:20%;'>";
      html += "<h2>Loading Setup...</h2><p>Please wait a moment.</p></body></html>";
      server.send(200, "text/html", html);
      portENTER_CRITICAL(&stateMux); isPortalRequested = true; portEXIT_CRITICAL(&stateMux);
    } else {
      server.send(200, "text/html", index_html); 
    }
  });

  server.onNotFound([]() {
    bool loc_ap;
    portENTER_CRITICAL(&stateMux); loc_ap = isHotspotActive; portEXIT_CRITICAL(&stateMux);
    
    if (loc_ap) {
      String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
      html += "<meta http-equiv='refresh' content='0;url=/'></head>";
      html += "<body style='background:#121212;color:#fff;text-align:center;font-family:sans-serif;padding-top:20%;'>";
      html += "<h2>Loading Setup...</h2><p>Please wait a moment.</p></body></html>";
      server.send(200, "text/html", html);
      portENTER_CRITICAL(&stateMux); isPortalRequested = true; portEXIT_CRITICAL(&stateMux);
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });
  
  server.on("/api/settings", HTTP_GET, []() {
    if (!server.authenticate(configUser.c_str(), configPass.c_str())) return server.requestAuthentication();
    String json = "{\"name\":\"" + configName + "\",\"url\":\"" + configUrl + "\",\"user\":\"" + configUser + "\"}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/settings", HTTP_POST, []() {
    if (!server.authenticate(configUser.c_str(), configPass.c_str())) return server.requestAuthentication();
    if (!server.hasArg("confirmPass") || server.arg("confirmPass") != configPass) { server.send(403, "text/plain", "Forbidden: Invalid Confirmation Password"); return; }
    if (server.hasArg("name")) configName = server.arg("name");
    if (server.hasArg("url")) configUrl = server.arg("url");
    if (server.hasArg("user")) configUser = server.arg("user");
    if (server.hasArg("pass") && server.arg("pass").length() > 0) configPass = server.arg("pass");
    saveSystemConfig(); server.send(200, "text/plain", "OK"); delay(500); ESP.restart(); 
  });

  server.on("/api/students", HTTP_GET, []() {
    if (!server.authenticate(configUser.c_str(), configPass.c_str())) return server.requestAuthentication();
    String json = ""; json.reserve(16384); json = "{"; bool first = true;
    xSemaphoreTake(fsMutex, portMAX_DELAY);
    for (auto const& x : studentCache) {
      if(!first) json += ",";
      String safeName = x.second; safeName.replace("\"", "\\\""); 
      json += "\"" + x.first + "\":\"" + safeName + "\"";
      first = false;
    }
    xSemaphoreGive(fsMutex);
    json += "}"; server.send(200, "application/json", json);
  });
  
  server.on("/api/add", HTTP_POST, []() {
    if (!server.authenticate(configUser.c_str(), configPass.c_str())) return server.requestAuthentication();
    if (server.hasArg("uid") && server.hasArg("name")) {
      String u = server.arg("uid"); String n = server.arg("name"); u.toUpperCase();
      xSemaphoreTake(fsMutex, portMAX_DELAY);
      File f = LittleFS.open("/students.csv", "a"); f.println(u + "," + n); f.close();
      studentCache[u] = n; 
      xSemaphoreGive(fsMutex);
      server.send(200, "text/plain", "OK");
    }
  });

  server.on("/api/health", HTTP_GET, []() {
    if (!server.authenticate(configUser.c_str(), configPass.c_str())) return server.requestAuthentication();
    DateTime now = rtc.now(); char timeStr[32]; snprintf(timeStr, sizeof(timeStr), "%02d/%02d/%04d %02d:%02d:%02d", now.day(), now.month(), now.year(), now.hour(), now.minute(), now.second());
    Wire.beginTransmission(0x68); bool hw_rtc = (Wire.endTransmission() == 0);
    Wire.beginTransmission(0x40); bool hw_ina = (Wire.endTransmission() == 0); 
    Wire.beginTransmission(0x3C); bool hw_oled = (Wire.endTransmission() == 0);
    byte rfidVer = rfid.PCD_ReadRegister(rfid.VersionReg); bool hw_rfid = (rfidVer != 0x00 && rfidVer != 0xFF);
    bool hw_fs = LittleFS.totalBytes() > 0;
    String json = "{";
    json += "\"voltage\":" + String(globalVoltage, 2) + ",\"battery\":" + String(globalBatPercent) + ",\"time\":\"" + String(timeStr) + "\",";
    json += "\"hw_rfid\":" + String(hw_rfid ? "true" : "false") + ",\"hw_rtc\":" + String(hw_rtc ? "true" : "false") + ",\"hw_ina\":" + String(hw_ina ? "true" : "false") + ",";
    json += "\"hw_oled\":" + String(hw_oled ? "true" : "false") + ",\"hw_fs\":" + String(hw_fs ? "true" : "false") + "}";
    server.send(200, "application/json", json);
  });
}

// ---------------------------------------------------------------------------------
// 1000% SAFE QUEUE FILE ERASER (3-STEP BACKUP)
// ---------------------------------------------------------------------------------
void removeLinesFromQueue(int linesToRemove) {
  xSemaphoreTake(fsMutex, portMAX_DELAY);
  File file = LittleFS.open("/queue.txt", "r");
  
  if (file) {
    File temp = LittleFS.open("/temp.txt", "w");
    if (temp) {
      int skipped = 0;
      while (file.available()) {
        String line = file.readStringUntil('\n');
        if (line.length() > 0 && line != "\r") {
          if (skipped < linesToRemove) {
            skipped++; 
          } else {
            temp.println(line); 
          }
        }
      }
      temp.close();
    }
    file.close();
    
    // SAFE 3-STEP RENAME PROCESS
    if (LittleFS.exists("/temp.txt")) {
      LittleFS.rename("/queue.txt", "/queue.bak"); // Step 1: Secure old data
      LittleFS.rename("/temp.txt", "/queue.txt");  // Step 2: Write new data
      LittleFS.remove("/queue.bak");               // Step 3: Cleanup
    }
  }
  xSemaphoreGive(fsMutex);
}

// ---------------------------------------------------------------------------------
// FREERTOS TASK (CORE 0 - FMS BATCH SYNC, CAPTIVE PORTAL & OTA)
// ---------------------------------------------------------------------------------
void backgroundNetworkTask(void * parameter) {
  unsigned long lastCycle = 0;
  static unsigned long lastReconnectAttempt = 0;
  unsigned long apStartTime = 0;
  DNSServer* dnsServer = NULL;
  bool portalWasActive = false;

  while(true) {
    if (WiFi.status() == WL_CONNECTED) {
       server.handleClient(); 
       
       // Stop hotspot if we successfully connected to a known network
       if (isHotspotActive) {
          if (dnsServer) { dnsServer->stop(); delete dnsServer; dnsServer = NULL; }
          WiFi.softAPdisconnect(true);
          portENTER_CRITICAL(&stateMux); isHotspotActive = false; portEXIT_CRITICAL(&stateMux);
       }
    } else {
       // --- BACKGROUND HOTSPOT & DNS LOGIC ---
       if (!isHotspotActive) {
          WiFi.softAP("Fees Machine");
          apStartTime = millis();
          portENTER_CRITICAL(&stateMux); isHotspotActive = true; portEXIT_CRITICAL(&stateMux);
          
          dnsServer = new DNSServer();
          dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
          dnsServer->start(53, "*", WiFi.softAPIP());
       }

       bool loc_isHotspotActive;
       portENTER_CRITICAL(&stateMux); loc_isHotspotActive = isHotspotActive; portEXIT_CRITICAL(&stateMux);

       // 1-MINUTE HOTSPOT TIMEOUT LOGIC
       if (loc_isHotspotActive && (millis() - apStartTime > 60000) && !isPortalRequested) {
          if (dnsServer) { dnsServer->stop(); delete dnsServer; dnsServer = NULL; }
          WiFi.softAPdisconnect(true);
          portENTER_CRITICAL(&stateMux); isHotspotActive = false; portEXIT_CRITICAL(&stateMux);
          loc_isHotspotActive = false;
          lastReconnectAttempt = millis() - 30000; // Reset timer so it attempts connection soon
       }
       
       // Try reconnecting quietly every 30 seconds
       if (!loc_isHotspotActive && (millis() - lastReconnectAttempt > 30000)) {
          WiFi.begin(); // Attempt known networks
          lastReconnectAttempt = millis();
       }

       if (dnsServer) dnsServer->processNextRequest();
       if (loc_isHotspotActive) server.handleClient();
    }
    
    portENTER_CRITICAL(&stateMux);
    bool checkAutoUpload = needsAutoUpload;
    bool checkManualSync = isManualSyncing;
    bool checkPortal = isPortalRequested;
    portEXIT_CRITICAL(&stateMux);

    // STATE A: Handing over to Core 1 (Stop background web processes)
    if (checkPortal) {
      if (!portalWasActive) {
        if (dnsServer) { dnsServer->stop(); delete dnsServer; dnsServer = NULL; }
        server.stop(); // Free Port 80 immediately!
        portalWasActive = true;
      }
      vTaskDelay(500 / portTICK_PERIOD_MS);
      continue;
    }

    // STATE B: Core 1 returned control without connecting (Portal timeout/closed)
    if (portalWasActive && !checkPortal) {
      portalWasActive = false;
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      WiFi.begin(); 
      server.begin(); 
      
      portENTER_CRITICAL(&stateMux);
      isHotspotActive = false; 
      portEXIT_CRITICAL(&stateMux);
    }

    // Pause heavy network operations if system asleep
    if (sysState != AWAKE && !checkAutoUpload && !checkManualSync) { 
        vTaskDelay(500 / portTICK_PERIOD_MS); 
        continue; 
    }

    if (millis() - lastCycle > 2000) {
      lastCycle = millis();

      if (WiFi.status() == WL_CONNECTED) {
        int rssi = WiFi.RSSI();
        portENTER_CRITICAL(&stateMux);
        if (rssi > -60) wifiSignalBars = 3; else if (rssi > -75) wifiSignalBars = 2; else if (rssi > -85) wifiSignalBars = 1; else wifiSignalBars = 0;
        portEXIT_CRITICAL(&stateMux);
        
        // -------------------------------------------------------------
        // ONE-SHOT OTA CLOUD UPDATE CHECK
        // -------------------------------------------------------------
        if (!hasCheckedUpdate && !checkManualSync && !checkAutoUpload && sysState == AWAKE) { 
            hasCheckedUpdate = true; // Never check again this session
            
            WiFiClientSecure otaClient;
            otaClient.setInsecure();
            HTTPClient httpOta;
            
            String versionUrl = "https://raw.githubusercontent.com/esp32-firmware/FMS-V2/refs/heads/main/version.txt";
            httpOta.begin(otaClient, versionUrl);
            
            int httpCode = httpOta.GET();
            if (httpCode == 200) {
                String onlineVersion = httpOta.getString();
                onlineVersion.trim();
                
                if (onlineVersion != currentFirmwareVersion && onlineVersion.length() > 0 && onlineVersion.length() < 10) {
                    portENTER_CRITICAL(&stateMux); sysState = UPDATING; portEXIT_CRITICAL(&stateMux);
                    
                    vTaskDelay(1500 / portTICK_PERIOD_MS); // Yield for Core 1 UI
                    Wire.end(); SPI.end(); // Sever Hardware Buses
                    
                    httpUpdate.onStart(ota_onStart);
                    httpUpdate.onEnd(ota_onEnd);
                    httpUpdate.onProgress(ota_onProgress);
                    httpUpdate.onError(ota_onError);
                    
                    String firmwareUrl = "https://raw.githubusercontent.com/esp32-firmware/FMS-V2/refs/heads/main/firmware.bin";
                    t_httpUpdate_return ret = httpUpdate.update(otaClient, firmwareUrl);
                    ESP.restart(); 
                }
            }
            httpOta.end();
        }

        // --- MANUAL SYNC LOGIC ---
        if (checkManualSync) {
          portENTER_CRITICAL(&stateMux); syncProgress = 30; portEXIT_CRITICAL(&stateMux);
          
          WiFiClientSecure client; 
          client.setInsecure();
          HTTPClient httpNames;
          httpNames.begin(client, configUrl + "?action=get_names");
          httpNames.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
          int httpCode = httpNames.GET(); 
          
          if (httpCode == 200 || httpCode == 302) {
            String payload = httpNames.getString();
            
            if (payload.indexOf("<html") == -1) { 
              payload.reserve(2048); 
              
              xSemaphoreTake(fsMutex, portMAX_DELAY);
              File f = LittleFS.open("/students.csv", "w");
              studentCache.clear(); 
              
              int strLen = payload.length(); int startIdx = 0;
              while(startIdx < strLen) {
                  int endIdx = payload.indexOf('\n', startIdx); if (endIdx == -1) endIdx = strLen;
                  String line = payload.substring(startIdx, endIdx); line.trim();
                  int comma = line.indexOf(',');
                  if (comma != -1) {
                      String u = line.substring(0, comma); String n = line.substring(comma + 1); u.trim(); n.trim();
                      if (n.length() > 0) { f.println(u + "," + n); studentCache[u] = n; }
                  }
                  startIdx = endIdx + 1; vTaskDelay(1 / portTICK_PERIOD_MS); 
              }
              f.close(); xSemaphoreGive(fsMutex);
              portENTER_CRITICAL(&stateMux); syncProgress = 100; portEXIT_CRITICAL(&stateMux);
            }
          }
          httpNames.end();
          vTaskDelay(800 / portTICK_PERIOD_MS); 
          portENTER_CRITICAL(&stateMux); isManualSyncing = false; portEXIT_CRITICAL(&stateMux); 
        }
        else {
          // --- BATCH UPLOAD LOGIC ---
          String payload = "";
          int linesRead = 0;
          
          xSemaphoreTake(fsMutex, portMAX_DELAY);
          File file = LittleFS.open("/queue.txt", "r");
          
          if (file && file.size() > 2) { 
             payload.reserve(file.size() + 100); 
             while (file.available()) {
                 String line = file.readStringUntil('\n');
                 line.trim();
                 if (line.length() > 0) {
                     if (linesRead > 0) payload += "\n";
                     payload += line;
                     linesRead++;
                 }
             }
             file.close();
             
             portENTER_CRITICAL(&stateMux); needsAutoUpload = true; batchTotal = linesRead; portEXIT_CRITICAL(&stateMux);
          } else {
             if (file) file.close(); 
             portENTER_CRITICAL(&stateMux); needsAutoUpload = false; batchTotal = 0; batchDone = 0; portEXIT_CRITICAL(&stateMux);
          }
          xSemaphoreGive(fsMutex);

          if (linesRead > 0) {
            WiFiClientSecure client; 
            client.setInsecure();
            HTTPClient http; 
            
            const char* headerKeys[] = {"Location"};
            http.collectHeaders(headerKeys, 1);
            
            http.begin(client, configUrl); 
            http.addHeader("Content-Type", "text/plain"); 
            http.setTimeout(15000); 
            
            int httpCode = http.POST(payload);
            
            if (httpCode == 302 || httpCode == 303 || httpCode == 301 || httpCode == 307 || httpCode == 308) {
                String redirectUrl = http.header("Location");
                http.end(); 
                
                if (redirectUrl.length() > 0) {
                    http.begin(client, redirectUrl);
                    httpCode = http.GET(); 
                }
            }
            
            if(httpCode == 200) {
                String response = http.getString();
                response.trim();
                
                if (response.startsWith("SUCCESS:")) {
                    String countStr = response.substring(8);
                    int processedCount = countStr.toInt();
                    
                    if (processedCount > 0) {
                        removeLinesFromQueue(processedCount);
                        
                        portENTER_CRITICAL(&stateMux);
                        batchDone = processedCount; 
                        notifyUploadSuccess = true; 
                        portEXIT_CRITICAL(&stateMux);
                    }
                } else {
                    vTaskDelay(3000 / portTICK_PERIOD_MS); 
                }
            } else {
                vTaskDelay(3000 / portTICK_PERIOD_MS); 
            }
            http.end();
          }
        }
      } else {
        portENTER_CRITICAL(&stateMux); wifiSignalBars = -1; needsAutoUpload = false; portEXIT_CRITICAL(&stateMux); 
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS); 
  }
}

// ---------------------------------------------------------------------------------
// STRING PARSERS
// ---------------------------------------------------------------------------------
String getStudentName(String uid) {
  xSemaphoreTake(fsMutex, portMAX_DELAY); String name = ""; 
  if (studentCache.count(uid) > 0) name = studentCache[uid];
  xSemaphoreGive(fsMutex); return name;
}

String toTitleCase(String str) {
  str.trim(); String result = ""; bool nextUpper = true;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str[i];
    if (c == ' ') { nextUpper = true; result += c; }
    else if (nextUpper) { result += (char)toupper(c); nextUpper = false; }
    else { result += (char)tolower(c); }
  }
  return result;
}

String getFirstName(String fullName) {
  fullName.trim(); int spaceIdx = fullName.indexOf(' ');
  if (spaceIdx != -1) return fullName.substring(0, spaceIdx);
  return fullName;
}

// ---------------------------------------------------------------------------------
// USER INTERFACE
// ---------------------------------------------------------------------------------
void renderSyncingBlock() {
  display.clearDisplay(); display.setFont(&FreeSans9pt7b); display.setTextSize(1);
  String txt = "SYNCING..."; int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 28); display.print(txt);
  display.drawRect(14, 40, 100, 10, SH110X_WHITE);
  display.fillRect(14, 40, syncProgress, 10, SH110X_WHITE);
  display.setFont(); display.display();
}

void updateScreen(int batPercent) {
  display.clearDisplay(); int16_t x1, y1; uint16_t w, h;

  if (millis() - lastMessageTime < 3000) {
    display.fillRoundRect(2, 2, 124, 60, 6, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK); display.setTextSize(1);
    
    if (displayStudentName != "") {
      int nl = currentMessage.indexOf('\n');
      if (nl != -1) {
        String l1 = currentMessage.substring(0, nl); String l2 = currentMessage.substring(nl + 1);
        display.setFont(&FreeSansBold9pt7b);
        display.getTextBounds(l1, 0, 0, &x1, &y1, &w, &h); display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 16); display.print(l1);
        display.getTextBounds(l2, 0, 0, &x1, &y1, &w, &h); display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 32); display.print(l2);
        display.drawLine(16, 38, 112, 38, SH110X_BLACK); 
        String printName = toTitleCase(displayStudentName); display.setFont(&FreeSansBold9pt7b);
        display.getTextBounds(printName, 0, 0, &x1, &y1, &w, &h);
        if (w > 116) { printName = getFirstName(printName); display.getTextBounds(printName, 0, 0, &x1, &y1, &w, &h);
          if (w > 116) { display.setFont(&FreeSans9pt7b); display.getTextBounds(printName, 0, 0, &x1, &y1, &w, &h); } }
        display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 55); display.print(printName);
      } 
      else {
        display.setFont(&FreeSansBold9pt7b); display.getTextBounds(currentMessage, 0, 0, &x1, &y1, &w, &h);
        if (w > 120) { display.setFont(&FreeSans9pt7b); display.getTextBounds(currentMessage, 0, 0, &x1, &y1, &w, &h); }
        display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 24); display.print(currentMessage);
        display.drawLine(16, 32, 112, 32, SH110X_BLACK);
        String printName = toTitleCase(displayStudentName); display.setFont(&FreeSansBold9pt7b); 
        display.getTextBounds(printName, 0, 0, &x1, &y1, &w, &h);
        if (w > 116) { printName = getFirstName(printName); display.getTextBounds(printName, 0, 0, &x1, &y1, &w, &h);
          if (w > 116) { display.setFont(&FreeSans9pt7b); display.getTextBounds(printName, 0, 0, &x1, &y1, &w, &h); } }
        display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 54); display.print(printName);
      }
    } 
    else {
      display.setFont(&FreeSansBold12pt7b); display.getTextBounds(currentMessage, 0, 0, &x1, &y1, &w, &h);
      if (w > 116) { display.setFont(&FreeSansBold9pt7b); display.getTextBounds(currentMessage, 0, 0, &x1, &y1, &w, &h); }
      display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 40); display.print(currentMessage);
    }
    display.setFont(); display.setTextColor(SH110X_WHITE);
  } 
  else {
    display.setTextColor(SH110X_WHITE); display.drawLine(0, 13, SCREEN_WIDTH, 13, SH110X_WHITE);
    display.setCursor(0, 3);
    if (isHotspotActive) { if ((millis() / 800) % 2 == 0) display.print("WIFI"); }
    else if (wifiSignalBars == -1) { display.print("OFF"); }
    else {
      for(int i=0; i<3; i++) {
        if(i < wifiSignalBars) display.fillRect(i*4, 9-(i*3), 3, (i*3)+3, SH110X_WHITE);
        else display.drawRect(i*4, 9-(i*3), 3, (i*3)+3, SH110X_WHITE);
      }
    }

    DateTime now = rtc.now();
    int hr = now.hour(); String ampm = hr >= 12 ? " PM" : " AM"; hr = hr % 12; if (hr == 0) hr = 12;
    display.setCursor(40, 3); display.print(hr); display.print(":"); 
    if(now.minute() < 10) display.print("0"); display.print(now.minute()); display.print(ampm);

    if (batPercent == -1) { display.setCursor(108, 3); display.print("USB"); }
    else {
      display.drawRect(102, 3, 20, 8, SH110X_WHITE); display.fillRect(122, 5, 2, 4, SH110X_WHITE);
      int fillWidth = map(batPercent, 0, 100, 0, 16); display.fillRect(104, 5, fillWidth, 4, SH110X_WHITE);
    }

    if (systemError != "OK") { display.setCursor(0, 15); display.print("SYS: " + systemError); }

    display.drawRoundRect(6, 18, 116, 24, 4, SH110X_WHITE); 
    display.fillTriangle(16, 30, 22, 26, 22, 34, SH110X_WHITE); 
    display.fillTriangle(112, 30, 106, 26, 106, 34, SH110X_WHITE); 

    display.setFont(&FreeSansBold9pt7b); display.setTextSize(1);
    String fullMonth = monthNames[selectedMonth]; String shortMonth = fullMonth.substring(0, 3); shortMonth.toUpperCase(); 
    display.getTextBounds(shortMonth, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 35); display.print(shortMonth);
    display.setFont(); 

    float wave = (sin(millis() / 350.0) + 1.0) / 2.0; int offset = wave * 4; 
    String prompt = "Tap Reg Book"; display.setTextSize(1); display.getTextBounds(prompt, 0, 0, &x1, &y1, &w, &h);
    int textX = (SCREEN_WIDTH - w) / 2 - x1;
    display.setCursor(textX, 48); display.print(prompt);
    display.setCursor(textX - 18 - offset, 48); display.print(">>");
    display.setCursor(textX + w + 6 + offset, 48); display.print("<<");
  }
  display.display();
}

// ---------------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  fsMutex = xSemaphoreCreateMutex();

  pinMode(BTN_PREV, INPUT_PULLUP); pinMode(BTN_NEXT, INPUT_PULLUP); pinMode(BTN_SYNC, INPUT_PULLUP);
  pinMode(LED_GREEN, OUTPUT); pinMode(LED_RED, OUTPUT); pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_GREEN, LOW); digitalWrite(LED_RED, LOW); digitalWrite(BUZZER_PIN, LOW); 

  if(!display.begin(0x3C, true)) for(;;);
  delay(50); display.clearDisplay(); display.display();
  
  SPI.begin(); rfid.PCD_Init(); rtc.begin(); ina219.begin(); LittleFS.begin(true);
  
  // SAFE BACKUP RECOVERY CHECK
  if (LittleFS.exists("/queue.bak") && !LittleFS.exists("/queue.txt")) {
    LittleFS.rename("/queue.bak", "/queue.txt");
  } else if (LittleFS.exists("/queue.bak")) {
    LittleFS.remove("/queue.bak"); 
  }

  loadSystemConfig(); 
  loadStudentCache(); 
  
  showPremiumStartup(); 
  
  // Fast Non-Blocking WiFi Start
  WiFi.mode(WIFI_STA); 
  WiFi.begin(); 
  
  // Start Web Server for Portal Intercept & API
  setupWebEndpoints();
  server.begin();

  DateTime now = rtc.now();
  selectedMonth = now.month();
  
  xTaskCreatePinnedToCore(backgroundNetworkTask, "NetTask", 32768, NULL, 1, &backgroundTaskHandle, 0);
  
  triggerBuzzer(150); 
  bootTime = millis(); lastActivityTime = millis(); 
}

// ---------------------------------------------------------------------------------
// MAIN LOOP (CORE 1)
// ---------------------------------------------------------------------------------
void loop() {
  
  // --- CORE 1 PORTAL HANDOFF (CRITICAL STABILITY) ---
  portENTER_CRITICAL(&stateMux); bool checkPortal = isPortalRequested; portEXIT_CRITICAL(&stateMux);
  
  if (checkPortal) {
    delay(150); 
    
    analogWrite(LED_GREEN, 0); analogWrite(LED_RED, 0); digitalWrite(BUZZER_PIN, LOW);
    
    display.clearDisplay(); display.setTextColor(SH110X_WHITE);
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(15, 24); display.print("PORTAL ACTIVE");
    display.setFont();
    display.setCursor(10, 42); display.print("Connect to AP:");
    display.setCursor(10, 54); display.print("Fees Machine");
    display.display();
    
    WiFi.softAPdisconnect(true); // Stop Core 0 Hotspot
    delay(100);
    
    WiFiManager wm;
    wm.setConfigPortalTimeout(180); // 3-minute timeout
    wm.setDebugOutput(false);
    
    bool connected = wm.startConfigPortal("Fees Machine"); // Core 1 takes over 100% processing
    
    if (connected) {
      ESP.restart(); // Safely reboot once finished
    } else {
      // Recover safely to Core 0 if user closed or timeout
      portENTER_CRITICAL(&stateMux); isPortalRequested = false; portEXIT_CRITICAL(&stateMux);
      display.clearDisplay(); display.display();
      lastScreenDraw = 0; // force redraw
    }
  }

  // --- CLOUD OTA UI LOCKOUT (CRITICAL SAFETY) ---
  if (sysState == UPDATING) {
    static bool updatingDrawn = false;
    if (!updatingDrawn) {
        analogWrite(LED_GREEN, 0); analogWrite(LED_RED, 0); digitalWrite(BUZZER_PIN, LOW);
        
        display.clearDisplay(); display.setTextColor(SH110X_WHITE); display.setFont(); display.setTextSize(1);
        int16_t x1, y1; uint16_t w, h;
        display.getTextBounds("UPDATING SOFTWARE", 0, 0, &x1, &y1, &w, &h);
        display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 24); display.print("UPDATING SOFTWARE");
        
        display.getTextBounds("Wait for 2 minutes", 0, 0, &x1, &y1, &w, &h);
        display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 42); display.print("Wait for 2 minutes");
        
        display.display(); updatingDrawn = true;
    }
    return; // Block ALL other UI updates
  }

  processAsyncHardware(); 

  // 1. BATTERY & USB READING
  static float smoothedVoltage = 0; static bool firstRead = true;
  float currentVoltage = ina219.getBusVoltage_V();
  
  if (!isnan(currentVoltage) && currentVoltage > 1.0) {
    if (firstRead) { smoothedVoltage = currentVoltage; firstRead = false; } 
    else { smoothedVoltage = (smoothedVoltage * 0.95) + (currentVoltage * 0.05); }
  }
  
  int voltageInt = (int)(smoothedVoltage * 100);
  int batPercent = constrain(map(voltageInt, 580, 820, 0, 100), 0, 100);
  if (ina219.getCurrent_mA() < 5.0) batPercent = -1; 
  
  globalVoltage = smoothedVoltage; globalBatPercent = batPercent;

  // --- ZERO BATTERY LOCKOUT ---
  if (batPercent == 0) {
    static unsigned long lastLowBatDraw = 0;
    if (millis() - lastLowBatDraw > 1000) {
      display.clearDisplay();
      display.drawRect(39, 20, 50, 24, SH110X_WHITE); 
      display.fillRect(89, 26, 4, 12, SH110X_WHITE);  
      display.display(); lastLowBatDraw = millis();
    }
    analogWrite(LED_GREEN, 0); analogWrite(LED_RED, 0); digitalWrite(BUZZER_PIN, LOW);
    return; 
  }

  // 2. MULTI-BUTTON TOUCH LOGIC
  bool isButtonTouched = isAnyButtonPressed();
  static bool prevButtonState = false;
  bool buttonJustPressed = isButtonTouched && !prevButtonState;
  prevButtonState = isButtonTouched;

  if (isButtonTouched && sysState != ALARM && sysState != AWAKE) { triggerLed(LED_RED, 200); }

  // 3. FMS STATE MACHINE 
  portENTER_CRITICAL(&stateMux);
  bool checkAutoUpload = needsAutoUpload;
  bool checkManualSync = isManualSyncing;
  portEXIT_CRITICAL(&stateMux);

  if (sysState == AWAKE) {
    if (millis() - lastActivityTime > 300000 && !checkAutoUpload && !checkManualSync) { sysState = ALARM; } 
    else if (millis() - lastActivityTime > 180000 && !checkAutoUpload && !checkManualSync) {
      sysState = SLEEPY; currentEyeHeight = 3.0; isBlinking = false;
    }
  } 
  else if (sysState == SLEEPY) {
    if (buttonJustPressed) {
      sysState = MOCHI_WAKE; stateTimer = millis(); currentEyeHeight = 3.0; isBlinking = false;
      nextLookTime = millis() + 400;  nextBlinkTime = millis() + 800; 
    } else if (millis() - lastActivityTime > 300000 && !checkAutoUpload && !checkManualSync) { 
      sysState = ALARM; display.clearDisplay(); display.display();
    }
  }
  else if (sysState == MOCHI_WAKE) {
    if (buttonJustPressed) { sysState = AWAKE; lastActivityTime = millis(); }
    else if (millis() - stateTimer > 30000) { sysState = SLEEPY; }
  }

  // 4. SCREEN & LED DRAWING BASED ON STATE
  static bool wasManualSyncing = false; 

  if (sysState == ALARM) {
    if ((millis() / 500) % 2 == 0) { 
      if (!isButtonTouched) triggerLed(LED_RED, 500); 
      triggerBuzzer(500);
    } 
    
    if (millis() - lastScreenDraw > 100) {
      display.clearDisplay(); display.setContrast(255);
      if ((millis() / 500) % 2 == 0) {
        display.setFont(&FreeSansBold12pt7b); display.setTextColor(SH110X_WHITE); display.setTextSize(1);
        int16_t x1, y1; uint16_t w, h; display.getTextBounds("TURN OFF", 0, 0, &x1, &y1, &w, &h);
        display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 40); display.print("TURN OFF"); display.setFont();
      }
      display.display(); lastScreenDraw = millis();
    }
    
    if (buttonJustPressed) { sysState = AWAKE; lastActivityTime = millis(); buzzerOffTime = 0; ledOffTime = 0; }
  } 
  else if (sysState == SLEEPY || sysState == MOCHI_WAKE) {
    if (!isButtonTouched) { updateSleepBreathingLED(LED_GREEN); }
    
    if (millis() - lastScreenDraw > 40) {
      display.clearDisplay(); display.setContrast(255);
      if (sysState == MOCHI_WAKE) drawMochiFace(0); else drawMochiFace(1); 
      display.display(); lastScreenDraw = millis();
    }
  } 
  else if (sysState == AWAKE) {
    display.setContrast(255);
    
    if (checkManualSync) {
      wasManualSyncing = true; updateBreathingLED(LED_GREEN); triggerLed(LED_RED, 50);
    } else {
      if (wasManualSyncing) {
        wasManualSyncing = false; currentMessage = "Sync Done"; displayStudentName = ""; lastMessageTime = millis();
        triggerBuzzer(300); 
      }
      if (!isButtonTouched) {
        if (isHotspotActive) { updateBreathingLED(LED_RED); } 
        else if (checkAutoUpload) { updateBreathingLED(LED_GREEN); } 
      }
    }
    
    if (millis() - lastScreenDraw > 40) { 
      if (checkManualSync) renderSyncingBlock(); else updateScreen(batPercent);
      lastScreenDraw = millis();
    }

    // BUTTON DEBOUNCE COOLDOWN
    static unsigned long lastActionBtn = 0;
    
    if (digitalRead(BTN_SYNC) == LOW && millis() - lastActionBtn > 500) {
      lastActionBtn = millis(); lastActivityTime = millis(); 
      if (WiFi.status() != WL_CONNECTED) { triggerBuzzer(800); currentMessage = "Offline!"; lastMessageTime = millis(); } 
      else { portENTER_CRITICAL(&stateMux); isManualSyncing = true; syncProgress = 0; portEXIT_CRITICAL(&stateMux); }
      lastScreenDraw = millis() + 300; 
    }
    
    if (digitalRead(BTN_NEXT) == LOW && millis() - lastActionBtn > 300) { 
      lastActionBtn = millis(); lastActivityTime = millis(); 
      selectedMonth = selectedMonth > 11 ? 1 : selectedMonth + 1; 
      lastScreenDraw = millis() + 200; 
    }
    
    if (digitalRead(BTN_PREV) == LOW && millis() - lastActionBtn > 300) { 
      lastActionBtn = millis(); lastActivityTime = millis(); 
      selectedMonth = selectedMonth < 2 ? 12 : selectedMonth - 1; 
      lastScreenDraw = millis() + 200; 
    }
  }

  // Upload Callbacks
  portENTER_CRITICAL(&stateMux); bool checkUploadSuccess = notifyUploadSuccess; portEXIT_CRITICAL(&stateMux);

  if (checkUploadSuccess) {
    portENTER_CRITICAL(&stateMux); notifyUploadSuccess = false; portEXIT_CRITICAL(&stateMux);
    lastActivityTime = millis(); 
    if (millis() - lastMessageTime > 1500) {
      currentMessage = String(batchDone) + " / " + String(batchTotal);
      displayStudentName = ""; lastMessageTime = millis();
    }
  }

  if (checkAutoUpload) {
    wasUploading = true;
  } else if (wasUploading && !checkAutoUpload) {
    wasUploading = false; 
    if (WiFi.status() == WL_CONNECTED) {
      currentMessage = "All Uploaded"; displayStudentName = ""; lastMessageTime = millis();
      triggerBuzzer(400); 
    }
  }

  // RFID Processing (STRICT LOCAL RAM DUPLICATE CHECK)
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    sysState = AWAKE; lastActivityTime = millis(); 
    
    String scannedUID = "";
    for (byte i = 0; i < rfid.uid.size; i++) scannedUID += String(rfid.uid.uidByte[i], HEX);
    scannedUID.toUpperCase(); rfid.PICC_HaltA(); 

    String sName = getStudentName(scannedUID);

    if (recentScans.count(scannedUID) > 0 && (millis() - recentScans[scannedUID] < 10000)) {
      triggerLed(LED_RED, 800); triggerBuzzer(800);
      currentMessage = "ALREADY\nMARKED";
      displayStudentName = (sName == "") ? "NEW CARD" : sName; 
      lastMessageTime = millis();
    } 
    // --- LOCAL DUPLICATE RAM CHECK HERE ---
    else if (sessionPaidCache.count(scannedUID) > 0 && sessionPaidCache[scannedUID] == selectedMonth) {
      triggerBuzzer(800); triggerLed(LED_RED, 1000); 
      currentMessage = "Already Paid"; displayStudentName = sName; lastMessageTime = millis();
    } 
    else {
      triggerLed(LED_GREEN, 300); 
      recentScans[scannedUID] = millis(); 
      sessionPaidCache[scannedUID] = selectedMonth; 
      
      if (sName == "") { triggerBuzzer(400); currentMessage = "Queued"; displayStudentName = "NEW CARD"; } 
      else { triggerBuzzer(150); currentMessage = "Saved"; displayStudentName = sName; }

      xSemaphoreTake(fsMutex, portMAX_DELAY);
      File file = LittleFS.open("/queue.txt", "a");
      file.println(scannedUID + "," + monthNames[selectedMonth]);
      file.close();
      
      portENTER_CRITICAL(&stateMux);
      if (batchTotal > 0) batchTotal++; 
      needsAutoUpload = true; 
      portEXIT_CRITICAL(&stateMux);
      xSemaphoreGive(fsMutex);
      
      lastMessageTime = millis();
    }
  }
}