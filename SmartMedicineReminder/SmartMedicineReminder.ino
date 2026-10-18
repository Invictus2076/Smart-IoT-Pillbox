/*
 * Smart IoT Pillbox - ESP32 medicine reminder with 3 compartments
 *
 * Features
 *  - Reminder times, tablet counts, window and today's dose status are saved
 *    in flash (Preferences), so they survive power cuts and resets.
 *  - Each compartment has its own sound: 1, 2 or 3 beeps per cycle, so the
 *    user can tell which medicine is due just by listening.
 *  - Tablet counter: every time a tray is opened its count goes down by one.
 *    A "Refill" warning shows when it reaches LOW_STOCK or below.
 *  - Dose window (default +/-30 min, settable from the web page). For a 12:30
 *    dose the window is 12:00-13:00:
 *      * tray opened 12:00-12:29 -> Taken, alarm at 12:30 is skipped
 *      * not opened by 12:30     -> alarm rings (40 s bursts every 5 min)
 *      * opened 12:30-12:59      -> Taken, alarm stops
 *      * not opened by 13:00     -> Missed
 *    Windows are clipped at midnight (they do not cross into the next day).
 *  - Test mode: web button plays a compartment's sound pattern and lets you
 *    open its tray to confirm the IR sensor works (doesn't change the count).
 *  - Non-blocking: the clock, web page and other compartments keep working
 *    while an alarm is ringing.
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <RTClib.h>
#include <LiquidCrystal.h>

// ===================== CONFIG =====================
// If your IR sensor reads LOW when the tray is opened, leave this true.
// If it reads HIGH when triggered, set it to false. Watch the Serial Monitor
// (115200 baud) while opening a tray to see which one is correct.
#define IR_ACTIVE_LOW true

// Set to true if you replace the DS1307 with a DS3231 (more accurate, 3.3 V).
#define USE_DS3231 false

// Print raw IR readings to Serial every 2 s.
#define DEBUG_IR true

// ESP32 Access Point (Hotspot). Use a stronger password for real use (8+ chars).
const char *ap_ssid     = "Smart_Medicine_Reminder";
const char *ap_password = "12345678";

// Dose window and alarm timing
const int DEFAULT_WINDOW_MIN = 30;    // +/- minutes around each dose time
const int MIN_WINDOW_MIN     = 5;
const int MAX_WINDOW_MIN     = 180;
const int RING_SECONDS       = 40;    // length of each alarm burst
const int REALERT_MINUTES    = 5;     // repeat the burst every N minutes until taken
const unsigned long DEBOUNCE_MS      = 300;   // tray must stay open this long to count
const unsigned long TEST_DURATION_MS = 8000;

// Tablets
const int LOW_STOCK   = 2;            // show "Refill" at or below this count
const int MAX_TABLETS = 999;

// Sound pattern: compartment N beeps N times, then pauses
const unsigned long BEEP_ON_MS       = 200;
const unsigned long BEEP_GAP_MS      = 200;
const unsigned long PATTERN_PAUSE_MS = 1200;

// ===================== HARDWARE =====================
// Parallel LCD pins (note: GPIO 2, 5 and 15 are ESP32 strapping pins; if the
// board ever fails to boot or flash, check these first)
const int rs = 15, en = 2, d4 = 4, d5 = 16, d6 = 17, d7 = 5;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

#if USE_DS3231
RTC_DS3231 rtc;
#else
RTC_DS1307 rtc;
#endif

WebServer server(80);
Preferences prefs;

#define BUZZER 18
const int ledPins[3] = {32, 33, 25};   // LED 1, 2, 3
const int irPins[3]  = {23, 19, 26};   // IR sensor 1, 2, 3

// ===================== STATE =====================
enum DoseStatus { ST_PENDING = 0, ST_TAKEN = 1, ST_MISSED = 2, ST_TOMORROW = 3 };

int  remHours[3]   = {-1, -1, -1};     // -1 = no reminder set
int  remMinutes[3] = {-1, -1, -1};
int  tablets[3]    = {0, 0, 0};
int  medStatus[3]  = {ST_PENDING, ST_PENDING, ST_PENDING};
int  windowMin     = DEFAULT_WINDOW_MIN;
long statusDay     = 0;                // yyyymmdd that medStatus[] belongs to

bool rtcOk = false;
bool alarmActive[3] = {false, false, false};   // dose due and not yet taken
bool ringing[3]     = {false, false, false};   // inside a 40 s burst

// IR debounce
bool irRaw[3]    = {false, false, false};
bool irStable[3] = {false, false, false};
unsigned long irChangedAt[3] = {0, 0, 0};

// Test mode
int  testSlot = -1;
unsigned long testStartedAt = 0;
bool testSensorSeen = false;

// ===================== WEB PAGE (HTML + CSS + JS) =====================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart IoT Pillbox</title>
<style>
  body{font-family:'Segoe UI',Tahoma,Verdana,sans-serif;background:#f0f4f8;margin:0;padding:16px;color:#2d3748}
  .container{max-width:600px;margin:0 auto}
  h1{color:#2c3e50;text-align:center;font-size:22px;margin:8px 0 16px}
  .panel{background:#fff;border-radius:12px;box-shadow:0 4px 15px rgba(0,0,0,.08);padding:16px;margin-bottom:14px}
  .clock{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px}
  .clock .t{font-size:26px;font-weight:700;font-variant-numeric:tabular-nums}
  .clock .d{color:#718096;font-size:14px}
  .conn{font-size:13px;font-weight:600}
  .conn.ok{color:#2f855a}.conn.bad{color:#c53030}
  .card-header{display:flex;justify-content:space-between;align-items:center}
  h3{margin:0;color:#3182ce;font-size:18px}
  .meta{color:#718096;font-size:13px;margin:6px 0 4px}
  .row{display:flex;align-items:center;gap:8px;margin-top:10px;flex-wrap:wrap}
  .row label{flex:1;min-width:140px;font-size:14px}
  input{padding:8px;border:1px solid #cbd5e0;border-radius:6px;font-size:16px;width:110px}
  button{background:#3182ce;color:#fff;border:none;padding:9px 14px;border-radius:6px;cursor:pointer;font-size:14px;font-weight:600}
  button:hover{background:#2b6cb0}
  button.ghost{background:#edf2f7;color:#2d3748}
  button.ghost:hover{background:#e2e8f0}
  button.wide{width:100%;margin-top:12px}
  .badge{padding:5px 12px;border-radius:15px;font-weight:700;font-size:12px;text-transform:uppercase}
  .s0{background:#e2e8f0;color:#4a5568}.s1{background:#c6f6d5;color:#22543d}.s2{background:#fed7d7;color:#742a2a}
  .s3{background:#e9d8fd;color:#44337a}.sA{background:#feebc8;color:#7b341e}.sN{background:#edf2f7;color:#a0aec0}
  .low{background:#fed7d7;color:#9b2c2c;border-radius:10px;padding:2px 8px;font-size:12px;font-weight:700;margin-left:6px}
  .hidden{display:none}
  #toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:#2d3748;color:#fff;padding:10px 16px;border-radius:8px;font-size:14px;opacity:0;transition:opacity .3s;pointer-events:none;max-width:90%}
  #toast.show{opacity:1}
  #toast.err{background:#c53030}
</style>
</head>
<body>
<div class="container">
  <h1>Smart IoT Pillbox</h1>
  <div class="panel clock">
    <div><div class="t" id="rtc-time">--:--:--</div><div class="d" id="rtc-date">&nbsp;</div></div>
    <span class="conn bad" id="conn">Connecting...</span>
  </div>
  <div id="slots"></div>
  <div class="panel">
    <h3>Settings</h3>
    <div class="row"><label>Dose window (&plusmn; minutes)</label><input type="number" id="window" min="5" max="180"><button onclick="setWindow()">Set</button></div>
    <div class="meta">Opening a tray inside its window marks the dose Taken. Opening it before the dose time skips the alarm.</div>
    <button class="ghost wide" onclick="syncClock()">Sync clock to this device</button>
  </div>
</div>
<div id="toast"></div>
<script>
const SOUND = ['1 beep', '2 beeps', '3 beeps'];
const STATUS = [['Pending','s0'], ['Taken','s1'], ['Missed','s2'], ['From tomorrow','s3']];
const $ = id => document.getElementById(id);
const dirty = {};

function buildCards() {
  let html = '';
  for (let i = 1; i <= 3; i++) {
    html += `<div class="panel">
      <div class="card-header"><h3>Compartment ${i}</h3><span id="status-${i}" class="badge sN">Not set</span></div>
      <div class="meta">Sound: <b>${SOUND[i-1]}</b> &middot; Tray: <span id="tray-${i}">-</span> &middot; Window: <span id="win-${i}">-</span></div>
      <div class="row"><label>Dose time</label><input type="time" id="time-${i}"><button onclick="setReminder(${i})">Set</button><button class="ghost" onclick="clearReminder(${i})">Clear</button></div>
      <div class="row"><label>Tablets left: <b id="count-${i}">-</b><span id="low-${i}" class="low hidden">Refill</span></label><input type="number" id="tabs-${i}" min="0" max="999"><button onclick="setTablets(${i})">Set</button></div>
      <button class="ghost wide" onclick="testSlot(${i})">Test sound &amp; sensor</button>
    </div>`;
  }
  $('slots').innerHTML = html;
  document.querySelectorAll('input').forEach(el => el.addEventListener('input', () => { dirty[el.id] = true; }));
}

// Fill an input from the device, unless the user is editing it.
function fill(id, value) {
  const el = $(id);
  if (!dirty[id] && document.activeElement !== el && el.value !== String(value)) el.value = value;
}

function hhmm(min) {
  min = Math.max(0, Math.min(1440, min));
  return String(Math.floor(min / 60) % 24).padStart(2, '0') + ':' + String(min % 60).padStart(2, '0');
}

function render(d) {
  $('rtc-time').textContent = d.rtcOk ? d.time : 'RTC error';
  $('rtc-date').textContent = d.rtcOk ? d.date : 'Check RTC wiring, then sync the clock';
  fill('window', d.window);
  d.slots.forEach((s, k) => {
    const i = k + 1;
    let label, cls;
    if (!s.time) { label = 'Not set'; cls = 'sN'; }
    else if (s.alarm) { label = 'Take now'; cls = 'sA'; }
    else { [label, cls] = STATUS[s.status] || STATUS[0]; }
    const badge = $('status-' + i);
    badge.textContent = label;
    badge.className = 'badge ' + cls;
    $('tray-' + i).textContent = s.tray ? 'open' : 'closed';
    if (s.time) {
      const [h, m] = s.time.split(':').map(Number), t = h * 60 + m;
      $('win-' + i).textContent = hhmm(t - d.window) + '-' + hhmm(t + d.window);
    } else {
      $('win-' + i).textContent = '-';
    }
    $('count-' + i).textContent = s.tablets;
    $('low-' + i).classList.toggle('hidden', !s.time || s.tablets > d.lowStock);
    fill('time-' + i, s.time);
    fill('tabs-' + i, s.tablets);
  });
}

function setConn(ok) {
  const c = $('conn');
  c.textContent = ok ? 'Connected' : 'Offline - reconnecting';
  c.className = 'conn ' + (ok ? 'ok' : 'bad');
}

function poll() {
  fetch('/status', {cache: 'no-store'})
    .then(r => { if (!r.ok) throw new Error(); return r.json(); })
    .then(d => { render(d); setConn(true); setTimeout(poll, 1000); })
    .catch(() => { setConn(false); setTimeout(poll, 2000); });
}

let toastTimer;
function toast(msg, err) {
  const t = $('toast');
  t.textContent = msg;
  t.className = 'show' + (err ? ' err' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.className = ''; }, 3000);
}

function post(path, params, okMsg, doneIds) {
  return fetch(path, {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: new URLSearchParams(params)
    })
    .then(r => r.text().then(t => { if (!r.ok) throw new Error(t || ('Error ' + r.status)); }))
    .then(() => { (doneIds || []).forEach(id => { dirty[id] = false; }); toast(okMsg); })
    .catch(e => toast(e instanceof TypeError ? 'Could not reach the pillbox' : e.message, true));
}

function setReminder(i) {
  const v = $('time-' + i).value;
  if (!v) return toast('Pick a time first', true);
  post('/setReminder', {id: i, time: v}, 'Compartment ' + i + ' set to ' + v, ['time-' + i]);
}
function clearReminder(i) {
  post('/setReminder', {id: i, time: 'off'}, 'Compartment ' + i + ' cleared', ['time-' + i]);
}
function setTablets(i) {
  const v = $('tabs-' + i).value;
  if (v === '') return toast('Enter a number of tablets', true);
  post('/setTablets', {id: i, count: v}, 'Compartment ' + i + ': ' + v + ' tablets', ['tabs-' + i]);
}
function setWindow() {
  const v = $('window').value;
  post('/setWindow', {min: v}, 'Dose window set to ±' + v + ' min', ['window']);
}
function testSlot(i) {
  post('/test', {id: i}, 'Testing compartment ' + i + ': listen, then open its tray');
}
function syncClock() {
  const n = new Date();
  post('/setTime', {y: n.getFullYear(), mo: n.getMonth() + 1, d: n.getDate(),
                    h: n.getHours(), mi: n.getMinutes(), s: n.getSeconds()}, 'Clock synced');
}

buildCards();
poll();
</script>
</body>
</html>
)rawliteral";

// ===================== HELPERS =====================
int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Returns true if the IR sensor at `index` reports the tray as opened,
// accounting for active-low vs active-high wiring (see IR_ACTIVE_LOW).
bool isTriggered(int index) {
  int val = digitalRead(irPins[index]);
  return IR_ACTIVE_LOW ? (val == LOW) : (val == HIGH);
}

bool hasReminder(int i) { return remHours[i] >= 0 && remMinutes[i] >= 0; }
int  doseMin(int i)     { return remHours[i] * 60 + remMinutes[i]; }
int  windowStart(int i) { return clampInt(doseMin(i) - windowMin, 0, 1440); }
int  windowEnd(int i)   { return clampInt(doseMin(i) + windowMin, 0, 1440); }  // exclusive

bool timeValid(const DateTime &t) {
  return t.year() >= 2020 && t.year() <= 2099 && t.month() >= 1 && t.month() <= 12 &&
         t.day() >= 1 && t.day() <= 31 && t.hour() < 24 && t.minute() < 60 && t.second() < 60;
}

long dayKey(const DateTime &t) { return t.year() * 10000L + t.month() * 100L + t.day(); }

bool readClock(DateTime &out) {
  if (!rtcOk) return false;
  out = rtc.now();
  return timeValid(out);
}

// ===================== PERSISTENT STORAGE =====================
const char *prefKey(const char *base, int i) {
  static char k[12];
  snprintf(k, sizeof(k), "%s%d", base, i);
  return k;
}

void loadSettings() {
  prefs.begin("pillbox", false);
  for (int i = 0; i < 3; i++) {
    int h = prefs.getInt(prefKey("h", i), -1);
    int m = prefs.getInt(prefKey("m", i), -1);
    bool valid = h >= 0 && h <= 23 && m >= 0 && m <= 59;
    remHours[i]   = valid ? h : -1;
    remMinutes[i] = valid ? m : -1;
    tablets[i]    = clampInt(prefs.getInt(prefKey("t", i), 0), 0, MAX_TABLETS);
    medStatus[i]  = clampInt(prefs.getInt(prefKey("s", i), ST_PENDING), ST_PENDING, ST_TOMORROW);
  }
  windowMin = clampInt(prefs.getInt("win", DEFAULT_WINDOW_MIN), MIN_WINDOW_MIN, MAX_WINDOW_MIN);
  statusDay = prefs.getLong("day", 0);
}

void saveReminder(int i) {
  prefs.putInt(prefKey("h", i), remHours[i]);
  prefs.putInt(prefKey("m", i), remMinutes[i]);
}
void saveTablets(int i) { prefs.putInt(prefKey("t", i), tablets[i]); }
void saveWindow()       { prefs.putInt("win", windowMin); }
void saveStatus() {
  for (int i = 0; i < 3; i++) prefs.putInt(prefKey("s", i), medStatus[i]);
  prefs.putLong("day", statusDay);
}

// ===================== DOSE LOGIC =====================
// New day (or first boot): every dose goes back to Pending.
void handleDayChange(const DateTime &now) {
  long today = dayKey(now);
  if (today == statusDay) return;
  statusDay = today;
  for (int i = 0; i < 3; i++) medStatus[i] = ST_PENDING;
  saveStatus();
  Serial.printf("[DAY] %ld: all doses reset to Pending\n", today);
}

// Decide which doses are due, ringing or missed.
void updateDoses(const DateTime &now) {
  int  nowMin = now.hour() * 60 + now.minute();
  long nowSec = nowMin * 60L + now.second();

  for (int i = 0; i < 3; i++) {
    alarmActive[i] = false;
    ringing[i] = false;
    if (!hasReminder(i) || medStatus[i] != ST_PENDING) continue;

    if (nowMin >= windowEnd(i)) {
      medStatus[i] = ST_MISSED;
      saveStatus();
      Serial.printf("[DOSE %d] Missed (window ended)\n", i + 1);
      continue;
    }
    if (nowMin >= doseMin(i)) {
      alarmActive[i] = true;
      long elapsed = nowSec - doseMin(i) * 60L;
      ringing[i] = (elapsed % (REALERT_MINUTES * 60L)) < RING_SECONDS;
    }
  }
}

void onTrayOpened(int i, const DateTime &now, bool timeOk) {
  // In test mode, opening the tested tray just confirms the sensor works.
  if (testSlot == i) {
    testSensorSeen = true;
    Serial.printf("[TEST %d] Sensor OK\n", i + 1);
    return;
  }

  if (tablets[i] > 0) {
    tablets[i]--;
    saveTablets(i);
  }
  Serial.printf("[TRAY %d] Opened, tablets left: %d\n", i + 1, tablets[i]);

  if (timeOk && hasReminder(i) && medStatus[i] == ST_PENDING) {
    int nowMin = now.hour() * 60 + now.minute();
    if (nowMin >= windowStart(i) && nowMin < windowEnd(i)) {
      medStatus[i] = ST_TAKEN;
      alarmActive[i] = false;
      ringing[i] = false;
      saveStatus();
      Serial.printf("[DOSE %d] Taken%s\n", i + 1, nowMin < doseMin(i) ? " early, alarm skipped" : "");
    }
  }
}

// Debounced edge detection: a tray counts as opened only after it has read
// "open" continuously for DEBOUNCE_MS, and only on the closed -> open change.
void updateTraySensors(const DateTime &now, bool timeOk) {
  unsigned long ms = millis();
  for (int i = 0; i < 3; i++) {
    bool raw = isTriggered(i);
    if (raw != irRaw[i]) {
      irRaw[i] = raw;
      irChangedAt[i] = ms;
    } else if (raw != irStable[i] && ms - irChangedAt[i] >= DEBOUNCE_MS) {
      irStable[i] = raw;
      if (raw) onTrayOpened(i, now, timeOk);
    }
  }
}

// ===================== BUZZER + LEDS =====================
// Compartment `slot` beeps (slot + 1) times per cycle, then pauses.
bool patternOn(int slot, unsigned long t) {
  unsigned long beeps = slot + 1;
  unsigned long beepLen = BEEP_ON_MS + BEEP_GAP_MS;
  unsigned long cycle = beeps * beepLen + PATTERN_PAUSE_MS;
  unsigned long p = t % cycle;
  if (p >= beeps * beepLen) return false;
  return (p % beepLen) < BEEP_ON_MS;
}

bool testRunning() { return testSlot >= 0; }

void updateOutputs() {
  unsigned long ms = millis();
  if (testSlot >= 0 && ms - testStartedAt >= TEST_DURATION_MS) testSlot = -1;

  // Real alarms take priority over a test.
  int soundSlot = -1;
  for (int i = 0; i < 3; i++) {
    if (ringing[i]) { soundSlot = i; break; }
  }
  bool buzz;
  if (soundSlot >= 0)      buzz = patternOn(soundSlot, ms);
  else if (testRunning())  buzz = patternOn(testSlot, ms - testStartedAt);
  else                     buzz = false;
  digitalWrite(BUZZER, buzz ? HIGH : LOW);

  for (int i = 0; i < 3; i++) {
    bool on = alarmActive[i] || (testSlot == i && patternOn(i, ms - testStartedAt));
    digitalWrite(ledPins[i], on ? HIGH : LOW);
  }
}

// ===================== LCD =====================
char lcdCache[2][17] = {"", ""};

// Prints a full 16-char row (padded), only when it changed, to avoid flicker.
void lcdRow(int row, const char *text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16.16s", text);
  if (strcmp(buf, lcdCache[row]) == 0) return;
  strcpy(lcdCache[row], buf);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void reminderShort(int i, char *out) {   // "0830" or "----" (4 chars)
  if (hasReminder(i)) snprintf(out, 5, "%02d%02d", remHours[i], remMinutes[i]);
  else strcpy(out, "----");
}

void updateDisplay(const DateTime &now, bool timeOk) {
  char r0[24], r1[24];

  if (!timeOk) {
    lcdRow(0, "RTC ERROR");
    lcdRow(1, "Check RTC wiring");
    return;
  }

  if (testRunning()) {
    snprintf(r0, sizeof(r0), "TEST SLOT %d", testSlot + 1);
    if (testSensorSeen) snprintf(r1, sizeof(r1), "Sensor OK!");
    else snprintf(r1, sizeof(r1), "Open tray %d", testSlot + 1);
    lcdRow(0, r0);
    lcdRow(1, r1);
    return;
  }

  for (int i = 0; i < 3; i++) {
    if (alarmActive[i]) {
      snprintf(r0, sizeof(r0), "TAKE MEDICINE %d", i + 1);
      if (tablets[i] == 0) snprintf(r1, sizeof(r1), "Tray %d EMPTY!", i + 1);
      else snprintf(r1, sizeof(r1), "Pills left: %d", tablets[i]);
      lcdRow(0, r0);
      lcdRow(1, r1);
      return;
    }
  }

  snprintf(r0, sizeof(r0), "Time %02d:%02d:%02d", now.hour(), now.minute(), now.second());
  lcdRow(0, r0);

  // Row 1 rotates every 3 s: reminder times, tablet counts, refill warning.
  bool anyLow = false;
  for (int i = 0; i < 3; i++) if (hasReminder(i) && tablets[i] <= LOW_STOCK) anyLow = true;
  int pages = anyLow ? 3 : 2;
  int page = (millis() / 3000) % pages;

  if (page == 0) {
    char a[5], b[5], c[5];
    reminderShort(0, a); reminderShort(1, b); reminderShort(2, c);
    snprintf(r1, sizeof(r1), "%s %s %s", a, b, c);            // "0830 1400 2100"
  } else if (page == 1) {
    char mark[3];
    for (int i = 0; i < 3; i++) mark[i] = (hasReminder(i) && tablets[i] <= LOW_STOCK) ? '!' : ' ';
    snprintf(r1, sizeof(r1), "Pill%3d%c%3d%c%3d%c",
             tablets[0], mark[0], tablets[1], mark[1], tablets[2], mark[2]);
  } else {
    strcpy(r1, "Refill:");
    for (int i = 0; i < 3; i++) {
      if (hasReminder(i) && tablets[i] <= LOW_STOCK) {
        char s[4];
        snprintf(s, sizeof(s), " %d", i + 1);
        strcat(r1, s);
      }
    }
  }
  lcdRow(1, r1);
}

// ===================== WEB HANDLERS =====================
// Reads a whole-number argument and checks its range. Rejects empty or
// non-numeric input (String::toInt() would silently turn it into 0).
bool readIntArg(const char *name, int lo, int hi, int &out) {
  if (!server.hasArg(name)) return false;
  String v = server.arg(name);
  v.trim();
  if (v.length() == 0 || v.length() > 6) return false;
  for (unsigned int k = 0; k < v.length(); k++) {
    if (!isDigit(v[k])) return false;
  }
  int n = v.toInt();
  if (n < lo || n > hi) return false;
  out = n;
  return true;
}

void sendOk()                 { server.send(200, "text/plain", "OK"); }
void sendBad(const char *msg) { server.send(400, "text/plain", msg); }

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", index_html);
}

void handleStatus() {
  DateTime now;
  bool ok = readClock(now);
  char buf[24];
  String j;
  j.reserve(640);

  j += F("{\"rtcOk\":");
  j += ok ? "true" : "false";

  if (ok) snprintf(buf, sizeof(buf), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  else strcpy(buf, "--:--:--");
  j += F(",\"time\":\"");
  j += buf;

  if (ok) snprintf(buf, sizeof(buf), "%04d-%02d-%02d", now.year(), now.month(), now.day());
  else buf[0] = '\0';
  j += F("\",\"date\":\"");
  j += buf;

  j += F("\",\"window\":");
  j += windowMin;
  j += F(",\"lowStock\":");
  j += LOW_STOCK;
  j += F(",\"slots\":[");
  for (int i = 0; i < 3; i++) {
    if (i) j += ',';
    j += F("{\"time\":\"");
    if (hasReminder(i)) {
      snprintf(buf, sizeof(buf), "%02d:%02d", remHours[i], remMinutes[i]);
      j += buf;
    }
    j += F("\",\"status\":");
    j += medStatus[i];
    j += F(",\"tablets\":");
    j += tablets[i];
    j += F(",\"tray\":");
    j += irStable[i] ? 1 : 0;
    j += F(",\"alarm\":");
    j += alarmActive[i] ? 1 : 0;
    j += '}';
  }
  j += F("]}");

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

// POST id=1..3, time=HH:MM or time=off
void handleSetReminder() {
  int id;
  if (!readIntArg("id", 1, 3, id)) return sendBad("Invalid compartment");
  id -= 1;

  String t = server.arg("time");
  t.trim();

  if (t == "off") {
    remHours[id] = -1;
    remMinutes[id] = -1;
    medStatus[id] = ST_PENDING;
    saveReminder(id);
    saveStatus();
    Serial.printf("[SET] Reminder %d cleared\n", id + 1);
    return sendOk();
  }

  if (t.length() != 5 || t[2] != ':' || !isDigit(t[0]) || !isDigit(t[1]) ||
      !isDigit(t[3]) || !isDigit(t[4])) {
    return sendBad("Time must be HH:MM");
  }
  int h = t.substring(0, 2).toInt();
  int m = t.substring(3, 5).toInt();
  if (h > 23 || m > 59) return sendBad("Time out of range");

  remHours[id] = h;
  remMinutes[id] = m;

  // If today's window for the new time is already over, start from tomorrow
  // instead of instantly showing "Missed".
  medStatus[id] = ST_PENDING;
  DateTime now;
  if (readClock(now) && now.hour() * 60 + now.minute() >= windowEnd(id)) medStatus[id] = ST_TOMORROW;

  saveReminder(id);
  saveStatus();
  Serial.printf("[SET] Reminder %d = %02d:%02d\n", id + 1, h, m);
  sendOk();
}

// POST id=1..3, count=0..999
void handleSetTablets() {
  int id, count;
  if (!readIntArg("id", 1, 3, id)) return sendBad("Invalid compartment");
  if (!readIntArg("count", 0, MAX_TABLETS, count)) return sendBad("Tablets must be 0-999");
  tablets[id - 1] = count;
  saveTablets(id - 1);
  Serial.printf("[SET] Tablets %d = %d\n", id, count);
  sendOk();
}

// POST min=5..180
void handleSetWindow() {
  int w;
  if (!readIntArg("min", MIN_WINDOW_MIN, MAX_WINDOW_MIN, w)) return sendBad("Window must be 5-180 minutes");
  windowMin = w;
  saveWindow();
  Serial.printf("[SET] Window = +/-%d min\n", w);
  sendOk();
}

// POST y, mo, d, h, mi, s (full date + time from the phone)
void handleSetTime() {
  int y, mo, d, h, mi, s;
  if (!readIntArg("y", 2020, 2099, y) || !readIntArg("mo", 1, 12, mo) || !readIntArg("d", 1, 31, d) ||
      !readIntArg("h", 0, 23, h) || !readIntArg("mi", 0, 59, mi) || !readIntArg("s", 0, 59, s)) {
    return sendBad("Invalid date/time");
  }
  static const int daysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int maxDay = daysInMonth[mo - 1] + ((mo == 2 && y % 4 == 0) ? 1 : 0);
  if (d > maxDay) return sendBad("Invalid date");
  if (!rtcOk) return sendBad("RTC not found - check wiring");

  rtc.adjust(DateTime(y, mo, d, h, mi, s));
  Serial.printf("[SYNC] RTC set to %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
  sendOk();
}

// POST id=1..3: play that compartment's pattern; opening its tray shows "Sensor OK"
void handleTest() {
  int id;
  if (!readIntArg("id", 1, 3, id)) return sendBad("Invalid compartment");
  testSlot = id - 1;
  testStartedAt = millis();
  testSensorSeen = false;
  Serial.printf("[TEST %d] Started\n", id);
  sendOk();
}

// ===================== SETUP / LOOP =====================
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
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SMART IOT");
  lcd.setCursor(0, 1);
  lcd.print("PILLBOX");
  delay(1500);

  loadSettings();

  rtcOk = rtc.begin();
  if (!rtcOk) {
    Serial.println("[BOOT] RTC not found - check SDA/SCL wiring");
  } else {
#if USE_DS3231
    if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
#else
    // Only fires when the RTC was never started (e.g. dead coin cell).
    // Use "Sync clock" on the web page to correct drift afterwards.
    if (!rtc.isrunning()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
#endif
    DateTime t = rtc.now();
    Serial.printf("[BOOT] RTC: %04d-%02d-%02d %02d:%02d:%02d\n",
                  t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
  }
  Serial.printf("[BOOT] IR_ACTIVE_LOW = %s, window = +/-%d min\n", IR_ACTIVE_LOW ? "true" : "false", windowMin);
  for (int i = 0; i < 3; i++) {
    Serial.printf("[BOOT] Slot %d: time %02d:%02d, tablets %d, status %d\n",
                  i + 1, remHours[i], remMinutes[i], tablets[i], medStatus[i]);
  }

  // Start the debounce from the current tray state, so a tray that is
  // already open at boot doesn't count as an opening.
  for (int i = 0; i < 3; i++) {
    irRaw[i] = irStable[i] = isTriggered(i);
    irChangedAt[i] = millis();
  }

  WiFi.softAP(ap_ssid, ap_password);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("[BOOT] AP IP: ");
  Serial.println(ip);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Wi-Fi ready");
  lcd.setCursor(0, 1);
  lcd.print(ip);
  delay(3000);
  lcd.clear();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/setReminder", HTTP_POST, handleSetReminder);
  server.on("/setTablets", HTTP_POST, handleSetTablets);
  server.on("/setWindow", HTTP_POST, handleSetWindow);
  server.on("/setTime", HTTP_POST, handleSetTime);
  server.on("/test", HTTP_POST, handleTest);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void loop() {
  server.handleClient();

  // Read the RTC 5 times a second (not every loop) to keep the I2C bus quiet.
  static DateTime now;
  static bool timeOk = false;
  static unsigned long lastClockRead = 0;
  if (lastClockRead == 0 || millis() - lastClockRead >= 200) {
    lastClockRead = millis();
    timeOk = readClock(now);
    if (timeOk) {
      handleDayChange(now);
      updateDoses(now);
    } else {
      for (int i = 0; i < 3; i++) alarmActive[i] = ringing[i] = false;
    }
  }

  updateTraySensors(now, timeOk);
  updateOutputs();

  static unsigned long lastLcd = 0;
  if (millis() - lastLcd >= 250) {
    lastLcd = millis();
    updateDisplay(now, timeOk);
  }

#if DEBUG_IR
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug >= 2000) {
    lastDebug = millis();
    Serial.printf("[DEBUG] IR raw: %d %d %d | open: %d %d %d\n",
                  digitalRead(irPins[0]), digitalRead(irPins[1]), digitalRead(irPins[2]),
                  irStable[0], irStable[1], irStable[2]);
  }
#endif

  delay(2);
}
