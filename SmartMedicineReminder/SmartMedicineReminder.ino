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
<html lang="en" data-look="dabba">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="light dark">
<title>Smart IoT Pillbox</title>
<script>try{var l=localStorage.getItem('look');if(l==='classic'||l==='dabba')document.documentElement.dataset.look=l;}catch(e){}</script>
<style>
:root{
  color-scheme:light dark;
  --font:system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans","Noto Sans Devanagari","Kohinoor Devanagari",sans-serif;
  --fs-xs:.8125rem;--fs-s:.9375rem;--fs-m:1.0625rem;--fs-l:1.25rem;--fs-xl:1.5rem;--fs-2xl:2rem;--fs-3xl:2.5rem;
  --tap:48px;--ease:cubic-bezier(.2,.7,.2,1);
}
/* ---- Masala dabba: brushed-steel ramp, spice katoris ---- */
:root[data-look=dabba]{
  --bg:#E3E7EA;--surface:#F4F6F7;--well:#D7DDE1;--rim:#AEB8BE;--hair:#C9D0D5;--ctl:#77838A;--live:#23824A;
  --ink:#1B2226;--ink-2:#434F56;--ink-3:#566269;--on-ink:#F4F6F7;
  --focus:#0B5CAD;--missed:#B42318;--warn:#8A4B00;--warn-bg:#F8E7C2;
  --btn-bg:var(--ink);--btn-fg:var(--on-ink);
  --haldi:#A86F00;--jeera:#7A4E25;--elaichi:#5C7A2E;
  --pill:#FFFCF3;--pill-edge:rgba(20,20,20,.38);--rim-outer:#CDD4D8;--rim-lip:#B3BCC2;
  --tray-radius:18px;
}
@media (prefers-color-scheme:dark){:root[data-look=dabba]{
  --bg:#101315;--surface:#191D20;--well:#22282C;--rim:#4A545A;--hair:#2C3337;--ctl:#6E7980;--live:#46C27A;
  --ink:#E8EDF0;--ink-2:#B9C3C9;--ink-3:#9BA6AD;--on-ink:#101315;
  --focus:#7DB5FF;--missed:#FF8577;--warn:#F2B552;--warn-bg:#3A2A0C;
  --haldi:#D49A1E;--jeera:#A8743F;--elaichi:#86A94A;
  --pill:#F7F2E6;--pill-edge:rgba(0,0,0,.55);--rim-outer:#394247;--rim-lip:#2A3136;
}}
/* ---- Classic: standard health-app look ---- */
:root[data-look=classic]{
  --bg:#F2F2F7;--surface:#FFFFFF;--well:#F2F2F7;--rim:#C7C7CC;--hair:#E5E5EA;--ctl:#86868B;--live:#23824A;
  --ink:#1C1C1E;--ink-2:#3C3C43;--ink-3:#5E5E63;--on-ink:#FFFFFF;
  --focus:#0A66D1;--missed:#B42318;--warn:#8A4B00;--warn-bg:#FFF0D1;
  --btn-bg:#0A66D1;--btn-fg:#FFFFFF;
  --c-taken-bg:#E2F4E7;--c-taken:#17652F;--c-due-bg:#FFE6B0;--c-due:#6E3800;
  --c-missed-bg:#FDE3E1;--c-missed:#A51F14;--c-up-bg:#ECEEF2;--c-up:#3C3C43;
  --c-tom-bg:#ECE8FB;--c-tom:#4B3A9E;
}
@media (prefers-color-scheme:dark){:root[data-look=classic]{
  --bg:#0E0F11;--surface:#1C1C1E;--well:#2C2C2E;--rim:#55555A;--hair:#38383A;--ctl:#737378;--live:#46C27A;
  --ink:#F2F2F7;--ink-2:#D1D1D6;--ink-3:#A8A8AE;--on-ink:#0E0F11;
  --focus:#5AA2FF;--missed:#FF8A80;--warn:#FFC864;--warn-bg:#3D2C0A;
  --btn-bg:#5AA2FF;--btn-fg:#0E0F11;
  --c-taken-bg:#163A22;--c-taken:#8BE3A9;--c-due-bg:#40300A;--c-due:#FFD27A;
  --c-missed-bg:#45191A;--c-missed:#FF9A91;--c-up-bg:#2C2C2E;--c-up:#D1D1D6;
  --c-tom-bg:#2B2448;--c-tom:#CEC2FF;
}}

*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--ink);font:400 var(--fs-m)/1.5 var(--font);caret-color:var(--focus);
  padding:env(safe-area-inset-top) env(safe-area-inset-right) calc(env(safe-area-inset-bottom) + 88px) env(safe-area-inset-left)}
::selection{background:var(--focus);color:var(--on-ink)}
:focus-visible{outline:3px solid var(--focus);outline-offset:2px}
h1,h2,h3,p,ul,ol{margin:0}
ul,ol{padding:0;list-style:none}
[lang=hi]{line-height:1.6}
.vh{position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0);white-space:nowrap}
.ic{width:1.15em;height:1.15em;flex:none;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.num{font-variant-numeric:tabular-nums}
.wrap{max-width:1080px;margin:0 auto;padding:0 16px}

/* Header */
.top{padding-top:20px;padding-bottom:8px}
.brandrow{display:flex;justify-content:space-between;align-items:center;gap:12px}
h1{font-size:1.375rem;font-weight:700;letter-spacing:-.005em;line-height:1.2}
.conn{display:inline-flex;align-items:center;gap:8px;font-size:var(--fs-s);font-weight:600;color:var(--ink-2)}
.conn .dot{width:10px;height:10px;border-radius:50%;background:var(--ink-3)}
.conn[data-s=live] .dot{background:var(--live)}
.conn[data-s=off]{color:var(--missed)}
.conn[data-s=off] .dot{background:var(--missed)}
.clockrow{display:flex;flex-wrap:wrap;align-items:baseline;gap:4px 16px;margin-top:12px}
.clock{display:inline-block;font-size:var(--fs-3xl);font-weight:650;line-height:1.05;letter-spacing:-.01em}
.clock small{font-size:.5em;font-weight:600;color:var(--ink-2);margin-left:2px}
.date{font-size:var(--fs-s);color:var(--ink-2)}

/* Banner */
.banner{display:flex;gap:12px;align-items:flex-start;margin-top:12px;padding:12px 16px;border:2px solid var(--missed);border-radius:12px;background:var(--surface)}
.banner .ic{color:var(--missed);margin-top:3px}
.banner b{display:block}
.banner [lang=hi]{display:block;color:var(--ink-2);font-size:var(--fs-s)}
.banner .ago{display:block;font-size:var(--fs-s);color:var(--ink-2);margin-top:4px}
.banner[hidden]{display:none}

/* Today summary */
.summary{margin:20px 0 12px;font-size:var(--fs-xl)}
.summary .en{display:block;font-size:1em;font-weight:650;line-height:1.3}
.summary .hint{display:block;font-size:var(--fs-s);font-weight:500;color:var(--ink-2);margin-top:4px}
.summary [lang=hi]{display:block;font-size:var(--fs-m);font-weight:500;color:var(--ink-2);margin-top:2px}


/* Today strip: all three statuses in the first phone viewport */
.strip{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,5.5rem),1fr));gap:8px;margin-bottom:16px}
.strip a{display:block;overflow-wrap:break-word;min-height:var(--tap);padding:8px 10px;border:1.5px solid var(--rim);border-radius:10px;color:var(--ink);text-decoration:none;background:var(--surface)}
.strip a:hover{border-color:var(--ink-2)}
.strip .s-top{display:flex;flex-wrap:wrap;align-items:center;gap:0 6px;font-weight:650}
.strip .s-top i{width:9px;height:9px;border-radius:50%;background:var(--spice,var(--ink-3));flex:none}
.strip .s-st{display:flex;align-items:center;gap:4px;font-size:var(--fs-xs);font-weight:650;color:var(--ink-2);margin-top:2px}
.strip [lang=hi]{display:block;font-size:var(--fs-xs);font-weight:500;color:var(--ink-2)}
.strip [data-state=due] a{background:var(--ink);border-color:var(--ink);color:var(--on-ink)}
.strip [data-state=due] .s-st,.strip [data-state=due] [lang=hi]{color:var(--on-ink)}
.strip [data-state=taken] a{border-color:var(--ink)}
.strip [data-state=taken] .s-st{color:var(--ink)}
.strip [data-state=missed] a{border-color:var(--missed)}
.strip [data-state=missed] .s-st{color:var(--missed)}
.strip [data-state=early] a,.strip [data-state=tomorrow] a{border-style:dashed}
[data-look=dabba] .strip li:nth-child(1){--spice:var(--haldi)}
[data-look=dabba] .strip li:nth-child(2){--spice:var(--jeera)}
[data-look=dabba] .strip li:nth-child(3){--spice:var(--elaichi)}
[data-look=classic] .strip a{border:0;border-radius:12px;box-shadow:0 1px 2px rgba(0,0,0,.06),0 2px 8px rgba(0,0,0,.04)}
[data-look=classic] .strip [data-state=due] a{background:var(--c-due-bg);color:var(--c-due)}
[data-look=classic] .strip [data-state=due] .s-st,[data-look=classic] .strip [data-state=due] [lang=hi]{color:var(--c-due)}
[data-look=classic] .strip [data-state=taken] a{background:var(--c-taken-bg)}
[data-look=classic] .strip [data-state=taken] .s-st{color:var(--c-taken)}
[data-look=classic] .strip [data-state=missed] a{background:var(--c-missed-bg)}
[data-look=classic] .strip [data-state=missed] .s-st{color:var(--c-missed)}
[data-look=classic] .strip [data-state=early] a{background:var(--c-due-bg)}
[data-look=classic] .strip [data-state=early] .s-st,[data-look=classic] .strip [data-state=early] [lang=hi]{color:var(--c-due)}
.slot{scroll-margin-top:12px}

/* Compartments */
.tray{display:grid;gap:12px}
.slot{display:grid;grid-template-columns:96px 1fr;gap:4px 16px;padding:16px;align-items:start}
.bowl{grid-row:span 1;width:96px}
.bowl svg{display:block;width:100%;height:auto}
.info{min-width:0}
.head{display:flex;justify-content:space-between;align-items:center;gap:8px}
h3{font-size:1.125rem;font-weight:650}
h3 [lang=hi]{font-weight:500;color:var(--ink-3);margin-left:4px;white-space:nowrap}
.pips{display:inline-flex;gap:4px;padding:6px 0}
.pips i{width:8px;height:8px;border-radius:50%;background:var(--spice,var(--ink-3))}
.when{display:flex;align-items:baseline;flex-wrap:wrap;gap:0 10px;margin-top:2px}
.time{font-size:var(--fs-2xl);font-weight:650;line-height:1.15}
.part{font-size:var(--fs-s);color:var(--ink-2)}
.stamp{display:inline-flex;flex-wrap:wrap;max-width:100%;align-items:center;gap:0 6px;margin-top:8px;padding:5px 12px;border:2px solid var(--rim);border-radius:6px;color:var(--ink-2);font-weight:700;font-size:var(--fs-s)}
.stamp [lang=hi]{font-weight:500}
.facts{display:flex;flex-wrap:wrap;gap:2px 16px;margin-top:10px;font-size:var(--fs-s);line-height:1.45;color:var(--ink-2)}
.facts li:empty{display:none}
.facts b{color:var(--ink);font-weight:650}
.tag{display:inline-block;margin-left:6px;padding:1px 8px;border-radius:999px;background:var(--warn-bg);color:var(--warn);font-weight:650;font-size:var(--fs-xs)}

/* States */
.slot[data-state=loading] .stamp,.slot[data-state=unset] .stamp{color:var(--ink-3)}
.slot[data-state=early] .stamp{border-style:dashed;border-color:var(--ink-2);color:var(--ink)}
.slot[data-state=due] .stamp{background:var(--ink);border-color:var(--ink);color:var(--on-ink)}
.slot[data-state=taken] .stamp{border-color:var(--ink);color:var(--ink)}
.slot[data-state=missed] .stamp{border-color:var(--missed);color:var(--missed)}
.slot[data-state=missed] .time{text-decoration:line-through;text-decoration-color:var(--missed);text-decoration-thickness:3px}
.slot[data-state=tomorrow] .stamp{border-style:dashed}
.slot[data-state=unset] .time,.slot[data-state=loading] .time{color:var(--ink-3)}

/* Setup disclosure */
.setup{grid-column:1/-1;margin-top:8px;border-top:1px solid var(--hair)}
.setup summary{display:flex;align-items:center;gap:8px;min-height:var(--tap);cursor:pointer;font-weight:650;list-style:none;color:var(--ink)}
.setup summary::-webkit-details-marker{display:none}
.setup summary .ic{transition:transform .2s var(--ease)}
.setup[open] summary .ic{transform:rotate(90deg)}
.setup summary [lang=hi]{font-weight:500;color:var(--ink-2)}
.form{display:grid;gap:16px;padding:4px 0 8px}
.field label{display:block;font-size:var(--fs-s);font-weight:650;margin-bottom:6px}
.field label [lang=hi]{font-weight:500;color:var(--ink-2);margin-left:4px}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
.row.split{justify-content:space-between;gap:12px;margin-top:4px}
input[type=time],input[type=number]{min-height:var(--tap);min-width:0;width:9.5rem;padding:8px 12px;border:1.5px solid var(--ctl);border-radius:10px;background:var(--surface);color:var(--ink);font:inherit;font-variant-numeric:tabular-nums}
input:hover{border-color:var(--ink-2)}
input:focus-visible{outline-offset:0;border-color:var(--focus)}
.btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;min-height:var(--tap);padding:0 18px;border:1.5px solid var(--rim);border-radius:10px;background:transparent;color:var(--ink);font:inherit;font-weight:650;cursor:pointer;transition:background-color .15s,border-color .15s,opacity .15s}
.btn:hover{border-color:var(--ink-2)}
.btn:active{transform:translateY(1px)}
.btn.primary{background:var(--btn-bg);border-color:var(--btn-bg);color:var(--btn-fg)}
.btn.primary:hover{opacity:.9}
.btn.danger{border-color:var(--missed);color:var(--missed)}
.btn.danger[data-armed]{background:var(--missed);color:var(--surface)}
.btn[aria-busy=true]{opacity:.6;cursor:progress}
.btn:disabled{opacity:.45;cursor:not-allowed}
.stepper{display:inline-flex;align-items:center;gap:0}
.stepper .btn{min-width:var(--tap);padding:0;font-size:var(--fs-l);border-color:var(--ctl)}
.stepper .btn:first-child{border-radius:10px 0 0 10px}
.stepper .btn:last-child{border-radius:0 10px 10px 0}
.stepper input{width:5.5rem;border-radius:0;border-left:0;border-right:0;text-align:center}

/* Settings */
.settings{margin-top:32px}
.settings h2{font-size:1.375rem;font-weight:650;margin-bottom:12px}
.settings h2 [lang=hi]{font-weight:500;color:var(--ink-2);margin-left:6px}
.sgrid{display:grid;gap:12px}
.panel{padding:16px}
.panel h3{margin-bottom:4px}
.panel p{font-size:var(--fs-s);color:var(--ink-2);margin-bottom:12px}
.panel p [lang=hi]{display:block}
.drift{color:var(--warn)!important;font-weight:650}
.looks{display:grid;gap:8px;border:0;padding:0;margin:0}
.looks label{display:flex;align-items:center;gap:12px;min-height:var(--tap);padding:8px 12px;border:1.5px solid var(--ctl);border-radius:10px;cursor:pointer;font-weight:600}
.looks label:has(input:checked){border-color:var(--ink);box-shadow:inset 0 0 0 1px var(--ink)}
.looks input{width:20px;height:20px;accent-color:var(--focus);margin:0}
.looks [lang=hi]{font-weight:500;color:var(--ink-2)}

/* Toast */
.toast{position:fixed;left:50%;bottom:calc(16px + env(safe-area-inset-bottom));transform:translate(-50%,8px);width:max-content;max-width:min(560px,calc(100% - 32px));padding:12px 16px;border-radius:12px;background:var(--ink);color:var(--on-ink);font-weight:600;opacity:0;pointer-events:none;transition:opacity .18s var(--ease),transform .18s var(--ease);box-shadow:0 6px 20px rgba(0,0,0,.18)}
.toast.show{opacity:1;transform:translate(-50%,0)}
.toast.err{background:var(--surface);color:var(--ink);border:2px solid var(--missed)}
.toast [lang=hi]{display:block;font-weight:500;opacity:.9}

/* ---- Dabba look: one steel tray, katoris, hairline seams ---- */
[data-look=dabba] .tray{gap:0;background:var(--surface);border:1px solid var(--hair);border-radius:var(--tray-radius);overflow:hidden}
[data-look=dabba] .slot+.slot{border-top:1px solid var(--hair)}
[data-look=dabba] .panel{background:var(--surface);border:1px solid var(--hair);border-radius:var(--tray-radius)}
[data-look=dabba] .slot[data-spice=haldi]{--spice:var(--haldi)}
[data-look=dabba] .slot[data-spice=jeera]{--spice:var(--jeera)}
[data-look=dabba] .slot[data-spice=elaichi]{--spice:var(--elaichi)}
.b-rim{fill:var(--rim-outer)}.b-lip{fill:var(--rim-lip)}
.b-fill{fill:var(--spice)}
.b-empty{fill:var(--well);stroke:var(--rim);stroke-width:1;stroke-dasharray:3 3}
.b-pill{fill:var(--pill);stroke:var(--pill-edge);stroke-width:.7}
.b-pill.out{animation:pillout .28s var(--ease) forwards}
.b-ring{fill:none;stroke:transparent;stroke-width:3.5}
.slot[data-state=due] .b-ring{stroke:var(--ink);animation:pulse 1.6s ease-in-out infinite}
@keyframes pillout{to{opacity:0}}
@keyframes pulse{50%{stroke-opacity:.25}}

/* ---- Classic look: cards and coloured chips ---- */
[data-look=classic] .bowl{display:none}
[data-look=classic] .slot{grid-template-columns:1fr;background:var(--surface);border-radius:16px;box-shadow:0 1px 2px rgba(0,0,0,.06),0 4px 14px rgba(0,0,0,.05)}
[data-look=classic] .panel{background:var(--surface);border-radius:16px;box-shadow:0 1px 2px rgba(0,0,0,.06),0 4px 14px rgba(0,0,0,.05)}
[data-look=classic] .stamp{border:0;border-radius:999px;padding:6px 12px;background:var(--c-up-bg);color:var(--c-up)}
[data-look=classic] .slot[data-state=due] .stamp{background:var(--c-due-bg);color:var(--c-due)}
[data-look=classic] .slot[data-state=early] .stamp{background:var(--c-due-bg);color:var(--c-due)}
[data-look=classic] .slot[data-state=taken] .stamp{background:var(--c-taken-bg);color:var(--c-taken)}
[data-look=classic] .slot[data-state=missed] .stamp{background:var(--c-missed-bg);color:var(--c-missed)}
[data-look=classic] .slot[data-state=tomorrow] .stamp{background:var(--c-tom-bg);color:var(--c-tom)}
[data-look=classic] .slot[data-state=due]{box-shadow:0 0 0 2px var(--c-due),0 4px 14px rgba(0,0,0,.08)}
[data-look=classic] .looks label:has(input:checked){border-color:var(--focus);box-shadow:inset 0 0 0 1px var(--focus)}

@media (min-width:880px){
  .wrap{padding-left:32px;padding-right:32px}
  .tray{grid-template-columns:repeat(3,1fr)}
  .strip{display:none}
  .slot{grid-template-columns:1fr;padding:20px}
  .bowl{width:132px;justify-self:center;margin-bottom:8px}
  [data-look=dabba] .slot+.slot{border-top:0;border-left:1px solid var(--hair)}
  .sgrid{grid-template-columns:repeat(3,1fr)}
}
@media (max-width:479px){
  .slot{grid-template-columns:84px 1fr;gap:4px 14px}
  .bowl{width:84px}
  h3{font-size:var(--fs-m)}
}
@media (max-width:360px){
  .strip a{padding:8px}
  .strip .s-top{font-size:var(--fs-s)}
  .strip .s-st .ic{display:none}
  .slot{grid-template-columns:72px 1fr;gap:4px 12px;padding:14px 12px}
  .bowl{width:72px}
  .time{font-size:var(--fs-xl)}
}
@media (prefers-reduced-motion:reduce){
  .b-pill.out{animation:none}
  .slot[data-state=due] .b-ring{animation:none;stroke-width:5}
  .toast,.setup summary .ic,.btn{transition:none}
}
</style>
</head>
<body>
<svg width="0" height="0" style="position:absolute" aria-hidden="true">
  <symbol id="i-check" viewBox="0 0 24 24"><path d="M5 12.5l4.5 4.5L19 7.5"/></symbol>
  <symbol id="i-bell" viewBox="0 0 24 24"><path d="M6 16V11a6 6 0 0112 0v5l1.5 2h-15L6 16zM10 20.5a2 2 0 004 0"/></symbol>
  <symbol id="i-x" viewBox="0 0 24 24"><path d="M6.5 6.5l11 11M17.5 6.5l-11 11"/></symbol>
  <symbol id="i-clock" viewBox="0 0 24 24"><circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3 2"/></symbol>
  <symbol id="i-cal" viewBox="0 0 24 24"><rect x="4" y="5.5" width="16" height="14" rx="2"/><path d="M4 10h16M8.5 3.5v4M15.5 3.5v4"/></symbol>
  <symbol id="i-minus" viewBox="0 0 24 24"><path d="M6.5 12h11"/></symbol>
  <symbol id="i-chev" viewBox="0 0 24 24"><path d="M9.5 6l6 6-6 6"/></symbol>
  <symbol id="i-alert" viewBox="0 0 24 24"><path d="M12 4l9 16H3L12 4zM12 10v4.5M12 17.5v.01"/></symbol>
</svg>

<header class="wrap top">
  <div class="brandrow">
    <h1>Smart IoT Pillbox</h1>
    <span class="conn" id="conn" data-s="wait" role="status"><span class="dot" aria-hidden="true"></span><span id="conn-t">Connecting…</span></span>
  </div>
  <div class="clockrow">
    <p><span class="vh">Pillbox time </span><span class="clock num" id="clock">--:--</span></p>
    <p class="date" id="date">&nbsp;</p>
  </div>
  <div class="banner" id="banner" role="alert" hidden>
    <svg class="ic" aria-hidden="true"><use href="#i-alert"/></svg>
    <div><b id="banner-en"></b><span lang="hi" id="banner-hi"></span><span class="ago" id="banner-ago"></span></div>
  </div>
</header>

<main class="wrap">
  <section aria-labelledby="summary">
    <h2 class="summary" id="summary" aria-live="polite"><span class="en">Connecting to the pillbox…</span><span lang="hi">पिलबॉक्स से जुड़ रहा है…</span></h2>
    <ol class="strip" id="strip" aria-label="Today at a glance"></ol>
    <ol class="tray" id="slots"></ol>
  </section>

  <section class="settings" aria-labelledby="set-h">
    <h2 id="set-h">Settings <span lang="hi">सेटिंग्स</span></h2>
    <div class="sgrid">
      <div class="panel">
        <h3><label for="window">Dose window</label> <span lang="hi">समय सीमा</span></h3>
        <p>Opening a tray up to this many minutes before or after its time counts as taken. <span lang="hi">इतने मिनट पहले या बाद में ट्रे खोलना खुराक माना जाएगा।</span></p>
        <div class="row">
          <span class="stepper">
            <button type="button" class="btn" data-step="-5" aria-label="5 minutes less">−</button>
            <input type="number" id="window" inputmode="numeric" min="5" max="180" step="5" aria-describedby="window-unit">
            <button type="button" class="btn" data-step="5" aria-label="5 minutes more">+</button>
          </span>
          <span id="window-unit" class="part">± min</span>
          <button type="button" class="btn primary" id="save-window">Save</button>
        </div>
      </div>
      <div class="panel">
        <h3>Clock <span lang="hi">घड़ी</span></h3>
        <p id="drift">Set the pillbox clock from this phone. <span lang="hi">इस फ़ोन से पिलबॉक्स की घड़ी मिलाएँ।</span></p>
        <button type="button" class="btn primary" id="sync">Sync clock to this phone</button>
      </div>
      <div class="panel">
        <h3 id="look-h">Look <span lang="hi">रूप</span></h3>
        <p>Only changes how this page looks on this phone. <span lang="hi">सिर्फ़ इस फ़ोन पर दिखावट बदलेगी।</span></p>
        <fieldset class="looks" aria-labelledby="look-h">
          <label><input type="radio" name="look" value="dabba"> Masala dabba <span lang="hi">मसाला डिब्बा</span></label>
          <label><input type="radio" name="look" value="classic"> Classic <span lang="hi">सादा</span></label>
        </fieldset>
      </div>
    </div>
  </section>
</main>

<div class="toast" id="toast" role="status" aria-live="polite"></div>

<script>
const $ = id => document.getElementById(id);
const SPICE = ['haldi', 'jeera', 'elaichi'];
const BEEPS = ['1 beep', '2 beeps', '3 beeps'];
const STATES = {
  loading:  ['Loading…', 'लोड हो रहा है', 'i-clock'],
  unset:    ['No time set', 'समय तय नहीं', 'i-minus'],
  upcoming: ['Upcoming', 'आने वाली', 'i-clock'],
  early:    ['Can take now', 'अभी ले सकते हैं', 'i-clock'],
  due:      ['Take now', 'अभी लें', 'i-bell'],
  taken:    ['Taken', 'ली गई', 'i-check'],
  missed:   ['Missed', 'छूट गई', 'i-x'],
  tomorrow: ['Starts tomorrow', 'कल से', 'i-cal']
};
const DAYS_HI = ['रविवार', 'सोमवार', 'मंगलवार', 'बुधवार', 'गुरुवार', 'शुक्रवार', 'शनिवार'];
const reduceMotion = matchMedia('(prefers-reduced-motion: reduce)');
const dirty = {};
const last = {};          // last rendered values, to touch the DOM only on change
let data = null, lastOk = 0, failures = 0;

/* ---------- small helpers ---------- */
const pad = n => String(n).padStart(2, '0');
const toMin = t => { const [h, m] = t.split(':').map(Number); return h * 60 + m; };
const hhmm = m => { m = Math.max(0, Math.min(1440, m)); return pad(Math.floor(m / 60) % 24) + ':' + pad(m % 60); };
function dayPart(h) {
  if (h >= 4 && h < 12) return ['Morning', 'सुबह'];
  if (h >= 12 && h < 16) return ['Afternoon', 'दोपहर'];
  if (h >= 16 && h < 20) return ['Evening', 'शाम'];
  return ['Night', 'रात'];
}
function set(el, prop, value) {           // write only when changed
  const key = el.id + '.' + prop;
  if (last[key] === value) return false;
  last[key] = value;
  el[prop] = value;
  return true;
}
const bi = (en, hi) => `<span class="en">${en}</span> <span lang="hi">${hi}</span>`;

/* ---------- compartments ---------- */
function slotHTML(i) {
  const n = i + 1;
  return `<li class="slot" id="slot-${n}" data-spice="${SPICE[i]}" data-state="loading" aria-labelledby="h-${n}">
  <div class="bowl" aria-hidden="true"><svg viewBox="0 0 100 100" id="bowl-${n}">${bowlSVG(0, 0)}</svg></div>
  <div class="info">
    <div class="head">
      <h3 id="h-${n}">Compartment ${n} <span lang="hi">खाना ${n}</span></h3>
      <span class="pips" title="Sound: ${BEEPS[i]}">${'<i></i>'.repeat(n)}<span class="vh">Sound: ${BEEPS[i]}</span></span>
    </div>
    <p class="when"><span class="time num" id="time-${n}">--:--</span><span class="part" id="part-${n}"></span></p>
    <p class="stamp" id="stamp-${n}"><svg class="ic" aria-hidden="true"><use href="#i-clock"/></svg>${bi('Loading…', 'लोड हो रहा है')}</p>
    <ul class="facts">
      <li id="left-${n}"></li>
      <li id="win-${n}"></li>
      <li id="tray-${n}"></li>
    </ul>
  </div>
  <details class="setup" id="setup-${n}">
    <summary><svg class="ic" aria-hidden="true"><use href="#i-chev"/></svg>Change compartment ${n} <span lang="hi">बदलें</span></summary>
    <div class="form">
      <div class="field">
        <label for="time-in-${n}">Dose time <span lang="hi">खुराक का समय</span></label>
        <div class="row"><input type="time" id="time-in-${n}"><button type="button" class="btn primary" data-act="time" data-n="${n}">Save time</button></div>
      </div>
      <div class="field">
        <label for="tabs-in-${n}">Tablets in compartment <span lang="hi">गोलियों की संख्या</span></label>
        <div class="row"><input type="number" id="tabs-in-${n}" inputmode="numeric" min="0" max="999"><button type="button" class="btn primary" data-act="tabs" data-n="${n}">Save count</button></div>
      </div>
      <div class="row split">
        <button type="button" class="btn" data-act="test" data-n="${n}">Test sound &amp; sensor</button>
        <button type="button" class="btn danger" data-act="clear" data-n="${n}">Clear time</button>
      </div>
    </div>
  </details>
</li>`;
}

function stripHTML(i) {
  const n = i + 1;
  return `<li id="strip-${n}" data-state="loading"><a href="#slot-${n}"><span class="s-top"><i aria-hidden="true"></i><span class="vh">Compartment </span>${n} · <span class="num" id="strip-t-${n}">--:--</span></span><span class="s-st" id="strip-s-${n}">…</span><span lang="hi" id="strip-h-${n}"></span></a></li>`;
}

/* Tablets are drawn as pills in the katori, center outwards (max 36 shown). */
const PILL_POS = [];
[[0, 1], [12, 6], [23, 12], [33, 17]].forEach(([r, c], k) => {
  for (let j = 0; j < c; j++) {
    const a = (j / c) * Math.PI * 2 + k * 0.45;
    PILL_POS.push([50 + r * Math.cos(a), 50 + r * Math.sin(a), a * 180 / Math.PI + 90]);
  }
});
function bowlSVG(count, leaving) {
  let s = '<circle class="b-rim" cx="50" cy="50" r="49"/><circle class="b-lip" cx="50" cy="50" r="45"/>';
  s += `<circle class="${count > 0 ? 'b-fill' : 'b-empty'}" cx="50" cy="50" r="41.5"/>`;
  const shown = Math.min(count, PILL_POS.length);
  for (let k = 0; k < shown; k++) {
    const [x, y, rot] = PILL_POS[k];
    const out = k >= shown - leaving ? ' out' : '';
    s += `<ellipse class="b-pill${out}" cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" rx="5.2" ry="3.2" transform="rotate(${rot.toFixed(0)} ${x.toFixed(1)} ${y.toFixed(1)})"/>`;
  }
  return s + '<circle class="b-ring" cx="50" cy="50" r="47.5"/>';
}
function drawBowl(n, count) {
  const svg = $('bowl-' + n), prev = last['bowl' + n];
  if (prev === count) return;
  last['bowl' + n] = count;
  if (prev !== undefined && count < prev && !reduceMotion.matches && document.documentElement.dataset.look === 'dabba') {
    svg.innerHTML = bowlSVG(prev, Math.min(prev - count, PILL_POS.length));
    setTimeout(() => { if (last['bowl' + n] === count) svg.innerHTML = bowlSVG(count, 0); }, 300);
  } else {
    svg.innerHTML = bowlSVG(count, 0);
  }
}

function slotState(s, nowMin, win) {
  if (!s.time) return 'unset';
  if (s.alarm) return 'due';
  if (s.status === 1) return 'taken';
  if (s.status === 2) return 'missed';
  if (s.status === 3) return 'tomorrow';
  const t = toMin(s.time);
  if (nowMin !== null && nowMin >= Math.max(0, t - win) && nowMin < t) return 'early';
  return 'upcoming';
}

/* ---------- render ---------- */
function render(d) {
  const ok = d.rtcOk;
  const nowMin = ok ? toMin(d.time) : null;

  // Header clock
  if (ok) {
    const [h, m, s] = d.time.split(':');
    set($('clock'), 'innerHTML', `${h}:${m}<small>:${s}</small>`);
    const dt = new Date(d.date + 'T' + d.time);
    set($('date'), 'innerHTML', dt.toLocaleDateString('en-GB', {weekday: 'short', day: 'numeric', month: 'short', year: 'numeric'}) + ` · <span lang="hi">${DAYS_HI[dt.getDay()]}</span>`);
  } else {
    set($('clock'), 'innerHTML', '--:--');
    set($('date'), 'innerHTML', '&nbsp;');
  }
  showBanner(ok ? null : 'rtc');

  fill('window', d.window);

  // Compartments
  let setCount = 0, taken = 0, missed = 0, due = [], next = null;
  d.slots.forEach((s, k) => {
    const n = k + 1, slot = $('slot-' + n);
    const st = slotState(s, nowMin, d.window);
    if (s.time) {
      setCount++;
      if (st === 'taken') taken++;
      if (st === 'missed') missed++;
      if (st === 'due') due.push(n);
      if ((st === 'upcoming' || st === 'early') && (next === null || toMin(s.time) < next.t)) next = {t: toMin(s.time), n};
    }
    if (slot.dataset.state !== st) { slot.dataset.state = st; $('strip-' + n).dataset.state = st; }

    const [en, hi, icon] = STATES[st];
    set($('stamp-' + n), 'innerHTML', `<svg class="ic" aria-hidden="true"><use href="#${icon}"/></svg>${bi(en, hi)}`);
    set($('time-' + n), 'textContent', s.time || '--:--');
    set($('strip-t-' + n), 'textContent', s.time || '--:--');
    set($('strip-s-' + n), 'innerHTML', `<svg class="ic" aria-hidden="true"><use href="#${icon}"/></svg>${en}`);
    set($('strip-h-' + n), 'textContent', hi);
    const part = s.time ? dayPart(+s.time.slice(0, 2)) : null;
    set($('part-' + n), 'innerHTML', part ? bi(part[0], part[1]) : '');

    const low = s.time && s.tablets <= d.lowStock;
    const tag = low ? ` <span class="tag">${s.tablets === 0 ? 'Empty · खाली' : 'Refill soon · जल्द भरें'}</span>` : '';
    set($('left-' + n), 'innerHTML', (!s.time && !s.tablets) ? '' : `<b class="num">${s.tablets}</b> ${s.tablets === 1 ? 'tablet' : 'tablets'} left · <span lang="hi">${s.tablets} बाकी</span>${tag}`);
    if (s.time) {
      const t = toMin(s.time);
      const range = `<span class="num">${hhmm(t - d.window)}–${hhmm(t + d.window)}</span>`;
      set($('win-' + n), 'innerHTML', `Take ${range} · <span lang="hi">लेने का समय</span>`);
    } else {
      set($('win-' + n), 'innerHTML', '');
    }
    set($('tray-' + n), 'innerHTML', s.tray ? 'Tray open · <span lang="hi">ट्रे खुली</span>' : 'Tray closed · <span lang="hi">ट्रे बंद</span>');

    drawBowl(n, s.tablets);
    fill('time-in-' + n, s.time);
    fill('tabs-in-' + n, s.tablets);
  });

  // Today summary
  let en, hi;
  if (!ok) { en = 'Pillbox clock needs attention'; hi = 'पिलबॉक्स की घड़ी ठीक करें'; }
  else if (!setCount) { en = 'No dose times set yet'; hi = 'अभी कोई समय तय नहीं'; }
  else if (due.length) { en = `Take compartment ${due.join(' & ')} now`; hi = `खाना ${due.join(' और ')} की खुराक अभी लें`; }
  else {
    en = `${taken} of ${setCount} taken today`;
    hi = `आज ${setCount} में से ${taken} ली गईं`;
    if (missed) { en += ` · ${missed} missed`; hi += ` · ${missed} छूट गईं`; }
    if (next) { en += ` · next at ${hhmm(next.t)}`; hi += ` · अगली ${hhmm(next.t)} बजे`; }
    else if (taken + missed === setCount) { en += ' · all done'; hi += ' · आज के लिए पूरा'; }
  }
  const hint = ok && !setCount ? '<span class="hint">Tap “Change compartment” below to add a time.</span>' : '';
  set($('summary'), 'innerHTML', `<span class="en">${en}</span><span lang="hi">${hi}</span>${hint}`);

  // Clock drift vs this phone
  if (ok) {
    const diff = Math.round((new Date(d.date + 'T' + d.time) - Date.now()) / 60000);
    const p = $('drift');
    if (Math.abs(diff) >= 2) {
      const mins = Math.abs(diff), dir = diff < 0 ? 'behind' : 'ahead of', dirHi = diff < 0 ? 'पीछे' : 'आगे';
      set(p, 'innerHTML', `Pillbox clock is ${mins} min ${dir} this phone. Sync it so doses ring on time. <span lang="hi">पिलबॉक्स की घड़ी ${mins} मिनट ${dirHi} है।</span>`);
      p.classList.add('drift');
    } else {
      set(p, 'innerHTML', 'Pillbox clock matches this phone. <span lang="hi">घड़ी सही है।</span>');
      p.classList.remove('drift');
    }
  }
}

function fill(id, value) {          // device value -> input, unless the user is editing it
  const el = $(id);
  if (!dirty[id] && document.activeElement !== el && el.value !== String(value)) el.value = value;
}

function showBanner(kind) {
  const b = $('banner');
  if (!kind) { if (!b.hidden) b.hidden = true; return; }
  const msg = kind === 'off'
    ? ['Can’t reach the pillbox. Check this phone is on the “Smart_Medicine_Reminder” Wi-Fi.', 'पिलबॉक्स से संपर्क नहीं। फ़ोन का Wi-Fi जाँचें।']
    : ['Pillbox clock error. Check the clock (RTC) wiring, then tap “Sync clock”.', 'घड़ी में खराबी। RTC की वायरिंग जाँचें, फिर घड़ी मिलाएँ।'];
  set($('banner-en'), 'textContent', msg[0]);
  set($('banner-hi'), 'textContent', msg[1]);
  set($('banner-ago'), 'textContent', kind === 'off' && data && data.rtcOk ? `Showing the last update from ${data.time}.` : '');
  b.hidden = false;
}

function setConn(state) {
  const c = $('conn');
  if (c.dataset.s === state) return;
  c.dataset.s = state;
  $('conn-t').innerHTML = state === 'live' ? 'Live · <span lang="hi">जुड़ा है</span>' : 'Offline · <span lang="hi">संपर्क नहीं</span>';
}

/* ---------- polling ---------- */
function poll() {
  fetch('/status', {cache: 'no-store'})
    .then(r => { if (!r.ok) throw new Error(); return r.json(); })
    .then(d => {
      data = d; lastOk = Date.now(); failures = 0;
      setConn('live');
      render(d);
      setTimeout(poll, 1000);
    })
    .catch(() => {
      failures++;
      if (failures >= 2 || !data) {
        setConn('off'); showBanner('off');
        if (!data) set($('summary'), 'innerHTML', '<span class="en">Waiting for the pillbox</span><span lang="hi">पिलबॉक्स का इंतज़ार</span>');
      }
      setTimeout(poll, 2000);
    });
}

/* ---------- actions ---------- */
let toastTimer;
function toast(en, hi, err) {
  const t = $('toast');
  t.innerHTML = `${en}${hi ? `<span lang="hi">${hi}</span>` : ''}`;
  t.className = 'toast show' + (err ? ' err' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.className = 'toast' + (err ? ' err' : ''); }, err ? 5000 : 3200);
}

function post(btn, path, params, done) {
  if (btn.getAttribute('aria-busy') === 'true') return;
  btn.setAttribute('aria-busy', 'true');
  fetch(path, {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: new URLSearchParams(params)})
    .then(r => r.text().then(t => { if (!r.ok) throw new Error(t || 'Error ' + r.status); }))
    .then(done)
    .catch(e => e instanceof TypeError
      ? toast('Couldn’t reach the pillbox. Nothing was changed.', 'पिलबॉक्स से संपर्क नहीं। कुछ नहीं बदला।', true)
      : toast(e.message + '. Nothing was changed.', 'कुछ नहीं बदला।', true))
    .finally(() => btn.removeAttribute('aria-busy'));
}

const armed = {};
function onAction(btn) {
  const n = +btn.dataset.n, act = btn.dataset.act;
  if (act === 'time') {
    const v = $('time-in-' + n).value;
    if (!v) return toast('Pick a time first.', 'पहले समय चुनें।', true);
    post(btn, '/setReminder', {id: n, time: v}, () => { dirty['time-in-' + n] = false; toast(`Compartment ${n} set to ${v}.`, `खाना ${n}: ${v} बजे`); });
  } else if (act === 'tabs') {
    const v = $('tabs-in-' + n).value.trim();
    if (!/^\d{1,3}$/.test(v)) return toast('Enter a number from 0 to 999.', '0 से 999 के बीच संख्या लिखें।', true);
    post(btn, '/setTablets', {id: n, count: v}, () => { dirty['tabs-in-' + n] = false; toast(`Compartment ${n}: ${v} tablets.`, `खाना ${n}: ${v} गोलियाँ`); });
  } else if (act === 'test') {
    post(btn, '/test', {id: n}, () => toast(`Listen for ${BEEPS[n - 1]}, then open tray ${n}.`, `${n} बीप सुनें, फिर ट्रे ${n} खोलें।`));
  } else if (act === 'clear') {
    // Destructive: needs a second tap within 3 seconds.
    if (!armed[n]) {
      armed[n] = setTimeout(() => { delete armed[n]; btn.removeAttribute('data-armed'); btn.textContent = 'Clear time'; }, 3000);
      btn.setAttribute('data-armed', '');
      btn.textContent = 'Tap again to clear';
      return;
    }
    clearTimeout(armed[n]); delete armed[n];
    btn.removeAttribute('data-armed'); btn.textContent = 'Clear time';
    post(btn, '/setReminder', {id: n, time: 'off'}, () => { dirty['time-in-' + n] = false; toast(`Compartment ${n} time cleared.`, `खाना ${n} का समय हटाया।`); });
  }
}

/* ---------- setup ---------- */
$('slots').innerHTML = [0, 1, 2].map(slotHTML).join('');
$('strip').innerHTML = [0, 1, 2].map(stripHTML).join('');
document.querySelectorAll('input').forEach(el => el.addEventListener('input', () => { dirty[el.id] = true; }));
$('slots').addEventListener('click', e => { const b = e.target.closest('button[data-act]'); if (b) onAction(b); });

document.querySelectorAll('[data-step]').forEach(b => b.addEventListener('click', () => {
  const w = $('window'), v = Math.max(5, Math.min(180, (+w.value || 30) + +b.dataset.step));
  w.value = v; dirty.window = true;
}));
$('save-window').addEventListener('click', e => {
  const v = $('window').value.trim();
  if (!/^\d+$/.test(v) || +v < 5 || +v > 180) return toast('Window must be 5 to 180 minutes.', 'समय सीमा 5 से 180 मिनट होनी चाहिए।', true);
  post(e.currentTarget, '/setWindow', {min: v}, () => { dirty.window = false; toast(`Dose window set to ±${v} min.`, `समय सीमा ±${v} मिनट`); });
});
$('sync').addEventListener('click', e => {
  const t = new Date();
  post(e.currentTarget, '/setTime', {y: t.getFullYear(), mo: t.getMonth() + 1, d: t.getDate(), h: t.getHours(), mi: t.getMinutes(), s: t.getSeconds()},
    () => toast('Clock synced to this phone.', 'घड़ी मिल गई।'));
});

const look = document.documentElement.dataset.look;
document.querySelector(`input[name=look][value=${look}]`).checked = true;
document.querySelectorAll('input[name=look]').forEach(r => r.addEventListener('change', () => {
  document.documentElement.dataset.look = r.value;
  try { localStorage.setItem('look', r.value); } catch (e) {}
}));

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
