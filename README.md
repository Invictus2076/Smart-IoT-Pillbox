# Smart IoT Pillbox

ESP32-based medicine reminder with three compartments, a phone-friendly web page,
IR tray sensors, an RTC clock and a 16x2 LCD.

## Features

| Feature | What it does |
|---|---|
| **Survives power cuts** | Reminder times, tablet counts, dose window and today's status are saved in the ESP32's flash (`Preferences`). Nothing is lost on a reset or power cycle. |
| **Different sound per compartment** | Compartment 1 beeps once per cycle, 2 beeps twice, 3 beeps three times, so you can tell which medicine is due just by listening. |
| **Tablet counter** | Set how many tablets you loaded. Every tray opening reduces the count by 1. A **Refill** warning appears on the web page and LCD at 2 or fewer. |
| **Dose window (±30 min)** | For a 12:30 dose the window is 12:00–13:00. Open the tray early (12:00–12:29) and the dose is marked **Taken** and the 12:30 alarm is skipped. Open it late (12:30–12:59) and the alarm stops. Not opened by 13:00 means **Missed**. The window is settable from 5 to 180 minutes. |
| **Repeating alarm** | When a dose is due, the LED stays on and the buzzer rings in 40-second bursts every 5 minutes until the tray is opened or the window ends. |
| **Test mode** | A "Test sound & sensor" button plays a compartment's beep pattern for 8 seconds. Opening that tray during the test shows **Sensor OK!** on the LCD and doesn't change the tablet count. |
| **Live web dashboard** | A status-first page in **English and Hindi** that follows the phone's **light/dark** setting. A summary line and a today strip show every compartment's status at a glance; each compartment shows its time, status, tablets left, dose window and tray state, with setup tucked behind a "Change" panel. Two looks under Settings: **Masala dabba** (each compartment is a steel katori showing its real tablets) and **Classic** (standard cards). Shows clear banners when the pillbox is offline or its clock fails. |
| **Clock sync** | One button sets the RTC's date and time from your phone. |

Status values: **Pending**, **Take now** (alarm active), **Taken**, **Missed**,
**From tomorrow** (the time was set after today's window had already passed).
Statuses reset to Pending each day.

## Hardware / Pins

| Part | ESP32 GPIO |
|---|---|
| LCD RS, EN, D4–D7 | 15, 2, 4, 16, 17, 5 |
| Buzzer (active) | 18 |
| LEDs 1–3 | 32, 33, 25 |
| IR sensors 1–3 | 23, 19, 26 |
| RTC (I2C) | SDA 21, SCL 22 |

GPIO 2, 5 and 15 are ESP32 strapping pins. If the board sometimes fails to boot
or flash, check these first.

## Power backup

Saving data in flash is handled in software. Keeping the device **running**
through a power cut needs a battery. A common low-cost setup:

```
USB 5V ──► TP4056 charger (with protection, OUT+/OUT-) ──► MT3608 boost (set to 5.0 V) ──► ESP32 VIN / 5V rail
                      │
                  18650 Li-ion cell
```

- The TP4056 charges the 18650 from USB, and the boost converter supplies 5 V
  to the ESP32, the LCD and the IR sensors.
- A USB power bank with pass-through charging, which doesn't switch off at low
  current, is a simpler alternative.
- The RTC keeps time on its own CR2032 coin cell.
- A **DS3231** RTC is recommended over the DS1307: it runs at 3.3 V and drifts
  far less. Set `USE_DS3231` to `true` in the sketch after swapping it.

## Libraries

- RTClib (Adafruit), which also installs Adafruit BusIO
- LiquidCrystal
- WiFi, WebServer, Wire, Preferences (included with the ESP32 core)

Tested to compile with ESP32 Arduino core 3.3.

## Usage

1. Open `SmartMedicineReminder/SmartMedicineReminder.ino` in the Arduino IDE and
   flash it to the ESP32.
2. Connect to Wi-Fi `Smart_Medicine_Reminder` (password `12345678`).
3. Open the IP shown on the LCD (usually `192.168.4.1`).
4. Tap **Sync clock to this device**.
5. For each compartment, set the dose time and the number of tablets loaded.
6. Use **Test sound & sensor** to check each buzzer pattern and IR sensor.

If the IR sensor logic is inverted (the tray shows "open" when it's closed),
change `IR_ACTIVE_LOW` at the top of the sketch.

## Configuration (top of the sketch)

| Setting | Default | Meaning |
|---|---|---|
| `DEFAULT_WINDOW_MIN` | 30 | Dose window in ± minutes (can also be changed from the web page) |
| `RING_SECONDS` / `REALERT_MINUTES` | 40 / 5 | Alarm burst length and how often it repeats |
| `DEBOUNCE_MS` | 300 | How long a tray must stay open to count as an opening |
| `LOW_STOCK` | 2 | Refill warning threshold |
| `BEEP_ON_MS`, `BEEP_GAP_MS`, `PATTERN_PAUSE_MS` | 200, 200, 1200 | Beep pattern timing |

## Known limitations

- Dose windows don't cross midnight. A dose at 00:10 can't be taken early at
  23:50 the previous day, and a dose at 23:50 stays "Take now" until midnight
  rather than being marked Missed.
- Every tray opening reduces the count, even outside a dose window, so peeking
  in uses up a tablet in the count.
- The web page uses the ESP32's own hotspot and has no login beyond the Wi-Fi
  password.
