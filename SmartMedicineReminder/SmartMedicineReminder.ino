#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <RTClib.h>
#include <LiquidCrystal.h>

// ===================== DEBUG / CONFIG =====================
// If your IR sensor reads LOW when it detects the medicine tray being
// opened/removed, leave this as true. If it reads HIGH when triggered,
// set this to false. Watch the Serial Monitor (115200 baud) after
// opening the dispenser to see which one is correct, then hardcode it.
#define IR_ACTIVE_LOW true

// ESP32 Access Point (Hotspot) Setup
const char *ap_ssid = "Smart_Medicine_Reminder";
const char *ap_password = "12345678";

// Parallel LCD Pins Definition
const int rs = 15, en = 2, d4 = 4, d5 = 16, d6 = 17, d7 = 5;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

RTC_DS1307 rtc;
WebServer server(80);

// Hardware Pins
#define BUZZER 18
const int ledPins[3] = {32, 33, 25};   // LED 1, 2, 3
const int irPins[3]  = {23, 19, 26};   // IR Sensor 1, 2, 3

// System Variables (Initialized to -1 = No default reminder)
int remHours[3]   = {-1, -1, -1};
int remMinutes[3] = {-1, -1, -1};
bool alerted[3]   = {false, false, false};

// Status Tracking: 0 = Pending, 1 = Taken, 2 = Missed
int medStatus[3] = {0, 0, 0};

// Embedded Web Interface (HTML/CSS/JS)
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Smart Medicine Reminder</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #f0f4f8; margin: 0; padding: 20px; color: #333; }
    .container { max-width: 600px; margin: 0 auto; background: white; padding: 25px; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.1); }
    h1 { color: #2c3e50; text-align: center; margin-bottom: 20px; font-size: 24px; }
    .card { background: #fafafa; border: 1px solid #e2e8f0; border-radius: 8px; padding: 15px; margin-bottom: 15px; }
    .card-header { display: flex; justify-content: space-between; align-items: center; }
    .card h3 { margin: 0; color: #3182ce; }
    .time-picker { display: flex; align-items: center; justify-content: space-between; margin-top: 15px; }
    input[type="time"], input[type="number"] { padding: 8px; border: 1px solid #cbd5e0; border-radius: 5px; font-size: 16px; }
    button { background-color: #3182ce; color: white; border: none; padding: 9px 15px; border-radius: 5px; cursor: pointer; font-size: 14px; font-weight: bold; }
    button:hover { background-color: #2b6cb0; }
    .header-time { text-align: center; font-size: 20px; font-weight: bold; color: #4a5568; margin-bottom: 20px; background: #e2e8f0; padding: 10px; border-radius: 8px; }
    .sync-row { display: flex; gap: 8px; justify-content: center; margin-bottom: 20px; }

    /* Status Badge Styles */
    .badge { padding: 5px 12px; border-radius: 15px; font-weight: bold; font-size: 13px; text-transform: uppercase; }
    .status-pending { background-color: #e2e8f0; color: #4a5568; }
    .status-taken { background-color: #c6f6d5; color: #22543d; }
    .status-missed { background-color: #fed7d7; color: #742a2a; }
  </style>
  <script>
    function updateStatus() {
      fetch('/status').then(response => response.json()).then(data => {
        document.getElementById('rtc-time').innerText = data.time;
        for (let i = 1; i <= 3; i++) {
          let badge = document.getElementById('status-' + i);
          let st = data.status[i - 1];
          if (st === 1) {
            badge.innerText = 'Taken';
            badge.className = 'badge status-taken';
          } else if (st === 2) {
            badge.innerText = 'Missed';
            badge.className = 'badge status-missed';
          } else {
            badge.innerText = 'Pending';
            badge.className = 'badge status-pending';
          }
        }
      });
    }
    setInterval(updateStatus, 1000);

    function setReminder(id) {
      let val = document.getElementById('time-' + id).value;
      if (!val) return alert('Please select a valid time');
      fetch('/setReminder?id=' + id + '&time=' + val).then(res => alert('Reminder ' + id + ' updated!'));
    }

    function syncClockToPhone() {
      let now = new Date();
      let h = now.getHours(), m = now.getMinutes(), s = now.getSeconds();
      fetch('/setTime?h=' + h + '&m=' + m + '&s=' + s)
        .then(res => res.text())
        .then(msg => alert(msg));
    }
  </script>
</head>
<body>
  <div class="container">
    <h1>Smart Medicine Dispenser</h1>
    <div class="header-time">Current System Time: <span id="rtc-time">--:--:--</span></div>
    <div class="sync-row">
      <button onclick="syncClockToPhone()">Sync Clock to This Device</button>
    </div>

    <div class="card">
      <div class="card-header">
        <h3>Medicine Group 1</h3>
        <span id="status-1" class="badge status-pending">Pending</span>
      </div>
      <div class="time-picker">
        <input type="time" id="time-1">
        <button onclick="setReminder(1)">Set Time</button>
      </div>
    </div>

    <div class="card">
      <div class="card-header">
        <h3>Medicine Group 2</h3>
        <span id="status-2" class="badge status-pending">Pending</span>
      </div>
      <div class="time-picker">
        <input type="time" id="time-2">
        <button onclick="setReminder(2)">Set Time</button>
      </div>
    </div>

    <div class="card">
      <div class="card-header">
        <h3>Medicine Group 3</h3>
        <span id="status-3" class="badge status-pending">Pending</span>
      </div>
      <div class="time-picker">
        <input type="time" id="time-3">
        <button onclick="setReminder(3)">Set Time</button>
      </div>
    </div>
  </div>
</body>
</html>
)rawliteral";

// Returns true if the IR sensor at `index` currently reports the
// dispenser as opened/medicine removed, accounting for active-low vs
// active-high wiring (see IR_ACTIVE_LOW above).
bool isTriggered(int index) {
  int val = digitalRead(irPins[index]);
  return IR_ACTIVE_LOW ? (val == LOW) : (val == HIGH);
}

void setup() {
  Serial.begin(115200);
  Wire.begin();

  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  for (int i = 0; i < 3; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
    pinMode(irPins[i], INPUT);
  }

  lcd.begin(16, 2);
  lcd.setCursor(0, 0);
  lcd.print("SMART MEDICINE");
  lcd.setCursor(0, 1);
  lcd.print("REMINDER SYSTEM");
  delay(2000);

  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
  }
  if (!rtc.isrunning()) {
    // Only fires the very first time the RTC is powered (e.g. dead
    // coin cell). After that it stays "running" forever, so this will
    // NOT fix drift on later resets. Use the /setTime endpoint (or the
    // "Sync Clock" button on the web page) to correct drift going
    // forward, instead of relying on this block or reflashing.
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // Print current RTC time at boot so you can immediately see in the
  // Serial Monitor how far off it is before doing anything else.
  DateTime bootTime = rtc.now();
  Serial.printf("[BOOT] RTC time: %02d:%02d:%02d\n", bootTime.hour(), bootTime.minute(), bootTime.second());
  Serial.printf("[BOOT] IR_ACTIVE_LOW = %s\n", IR_ACTIVE_LOW ? "true" : "false");

  WiFi.softAP(ap_ssid, ap_password);
  IPAddress IP = WiFi.softAPIP();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AP: MedicineSys");
  lcd.setCursor(0, 1);
  lcd.print("IP: ");
  lcd.print(IP);
  delay(3000);

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", index_html);
  });

  server.on("/status", HTTP_GET, []() {
    DateTime now = rtc.now();
    char timeBuff[9];
    snprintf(timeBuff, sizeof(timeBuff), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());

    String json = "{\"time\":\"" + String(timeBuff) + "\",\"status\":["
                + String(medStatus[0]) + ","
                + String(medStatus[1]) + ","
                + String(medStatus[2]) + "]}";
    server.send(200, "application/json", json);
  });

  server.on("/setReminder", HTTP_GET, []() {
    if (server.hasArg("id") && server.hasArg("time")) {
      int id = server.arg("id").toInt() - 1;
      String t = server.arg("time");
      if (id >= 0 && id < 3 && t.length() == 5) {
        remHours[id] = t.substring(0, 2).toInt();
        remMinutes[id] = t.substring(3, 5).toInt();
        medStatus[id] = 0; // Reset to pending when time is modified
        server.send(200, "text/plain", "OK");
        return;
      }
    }
    server.send(400, "text/plain", "Bad Request");
  });

  // NEW: lets you correct clock drift on the fly, without reflashing.
  // Call as /setTime?h=14&m=30&s=0  (24-hour format)
  // The web page's "Sync Clock to This Device" button calls this
  // automatically using your phone/laptop's current time.
  server.on("/setTime", HTTP_GET, []() {
    if (server.hasArg("h") && server.hasArg("m") && server.hasArg("s")) {
      int h = server.arg("h").toInt();
      int m = server.arg("m").toInt();
      int s = server.arg("s").toInt();
      DateTime current = rtc.now();
      rtc.adjust(DateTime(current.year(), current.month(), current.day(), h, m, s));
      Serial.printf("[SYNC] RTC manually set to %02d:%02d:%02d\n", h, m, s);
      server.send(200, "text/plain", "Clock synced!");
      return;
    }
    server.send(400, "text/plain", "Missing h/m/s params");
  });

  server.begin();
}

void loop() {
  server.handleClient();
  DateTime now = rtc.now();

  updateDisplay(now);
  checkReminders(now);

  // Periodic debug print so you can see the resting (untriggered) IR
  // values even before an alarm fires. Watch how they change when you
  // open/close the dispenser to confirm your wiring and IR_ACTIVE_LOW
  // setting are correct.
  static unsigned long lastDebugPrint = 0;
  if (millis() - lastDebugPrint > 2000) {
    lastDebugPrint = millis();
    Serial.printf("[DEBUG] IR raw: %d %d %d | triggered: %d %d %d\n",
                  digitalRead(irPins[0]), digitalRead(irPins[1]), digitalRead(irPins[2]),
                  isTriggered(0), isTriggered(1), isTriggered(2));
  }

  // Daily reset at midnight (00:00:00)
  if (now.hour() == 0 && now.minute() == 0 && now.second() == 0) {
    for (int i = 0; i < 3; i++) {
      alerted[i] = false;
      medStatus[i] = 0;
    }
  }

  // Minute reset for re-arming alert state after trigger minute passes
  if (now.second() == 0) {
    for (int i = 0; i < 3; i++) {
      if (remHours[i] != -1 && now.minute() != remMinutes[i]) {
        alerted[i] = false;
      }
    }
  }

  delay(200);
}

String getShortTime(int index) {
  if (remHours[index] == -1 || remMinutes[index] == -1) {
    return "--:--";
  }
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", remHours[index], remMinutes[index]);
  return String(buf);
}

void updateDisplay(DateTime now) {
  // Row 0: System RTC Time
  lcd.setCursor(0, 0);
  char timeStr[17];
  snprintf(timeStr, sizeof(timeStr), "Time: %02d:%02d:%02d", now.hour(), now.minute(), now.second());
  lcd.print(timeStr);

  // Row 1: All 3 active reminder times
  lcd.setCursor(0, 1);
  char remStr[17];
  snprintf(remStr, sizeof(remStr), "%s %s %s",
           getShortTime(0).c_str(),
           getShortTime(1).c_str(),
           getShortTime(2).c_str());
  lcd.print(remStr);
}

void checkReminders(DateTime now) {
  for (int i = 0; i < 3; i++) {
    if (remHours[i] != -1 && remMinutes[i] != -1) {
      if (now.hour() == remHours[i] && now.minute() == remMinutes[i] && !alerted[i]) {
        triggerReminder(i);
        alerted[i] = true;
      }
    }
  }
}

void triggerReminder(int index) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("TAKE MEDICINE ");
  lcd.print(index + 1);

  digitalWrite(ledPins[index], HIGH);
  unsigned long startAlert = millis();
  bool medicineTaken = false;

  // Active Alert Loop for 40 SECONDS
  while (millis() - startAlert < 40000) {
    digitalWrite(BUZZER, HIGH);
    delay(200);
    digitalWrite(BUZZER, LOW);
    delay(200);

    server.handleClient();

    // Debug: watch this in Serial Monitor while you open the dispenser
    // during a live alarm to confirm the sensor is actually reacting.
    Serial.printf("[ALERT %d] IR raw=%d triggered=%d\n", index + 1, digitalRead(irPins[index]), isTriggered(index));

    if (isTriggered(index)) {
      medicineTaken = true;
      break;
    }
  }

  // Update status based on IR sensor outcome
  if (medicineTaken) {
    medStatus[index] = 1; // TAKEN
  } else {
    medStatus[index] = 2; // MISSED
  }

  digitalWrite(ledPins[index], LOW);
  digitalWrite(BUZZER, LOW);
  lcd.clear();
}