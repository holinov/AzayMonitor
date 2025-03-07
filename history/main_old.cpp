/**
 * AzayMonitor
 *
 * A medication and feeding schedule monitoring system with timer functionality.
 * Features:
 * - Displays current task and time on LCD
 * - Supports multiple task types: simple tasks, timed tasks, and visual separators
 * - Configurable relative timers for feeding and medication intervals
 * - Buzzer alerts for timed events
 * - EEPROM state persistence
 * - Debug mode support for testing
 *
 * Hardware:
 * - LCD Display (I2C)
 * - DS3231 RTC
 * - Passive Buzzer
 * - Push Button
 */
#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DS3231.h>
#include <EEPROM.h>
#include "pitches.h"

#define BUTTON_PIN 8
#define PASSIVE_BUZZER_PIN 9

LiquidCrystal_I2C lcd(0x27, 16, 2);  // LCD at address 0x27, 16 chars, 2 lines
DS3231 rtclock;

#define HAS_NO_TIMER       0b00000000
#define HAS_REALTIVE_TIMER 0b00000001
#define HAS_ABSOLUTE_TIMER 0b00000010

// #define DEBUG 1

#ifndef DEBUG
#define HALF_HOUR 1800
#define TEN_MINUTES 600
#else
#define HALF_HOUR 2
#define TEN_MINUTES 2
#endif

#define NOTE_DURATION 250

// Medication names
#define MED_ANTEPSIN                   "Antepsin 1/4"
#define MED_KVAMATEL                   "Kvamatel 1/6"
#define MED_VETMEDIN                   "Vetmedin 1"
#define MED_FEED                       "Feed"
#define MED_TRIGRIM                    "Trigrim 1/4"
#define MED_GABA                       "Gaba 1ml"
#define MED_AMLODIPIN                  "Amlodipin 1/15"
#define MED_VIAGRA                     "Viagra50/14 1ml"
#define MED_VEROSHPIRON                "Veroshpiron 1/4"
#define MED_URSOSAN                    "Ursosan 1/6"
#define MED_SLEEP                      "Sleep"
#define MED_WALK                       "Walk"

// Task declaration macros
#define SIMPLE_TASK(msg)               { msg, HAS_NO_TIMER, 0 }
#define RELATIVE_TIMER_TASK(msg, time) { msg, HAS_REALTIVE_TIMER, time }
#define ABSOLUTE_TIMER_TASK(msg, time) { msg, HAS_ABSOLUTE_TIMER, time }
#define SPACER_TASK()                  { "Spacer", HAS_REALTIVE_TIMER, TEN_MINUTES }
#define BLOCK_SEPARATOR()              { "---------------", HAS_NO_TIMER, 0 }

// Task block macros
#define MEDICATION_BLOCK(var_med)      \
    /* Common morning/evening meds */  \
    SIMPLE_TASK(MED_ANTEPSIN),        \
    SIMPLE_TASK(MED_KVAMATEL),        \
    SIMPLE_TASK(MED_VETMEDIN),        \
    RELATIVE_TIMER_TASK(MED_FEED, HALF_HOUR), \
    /* Variable medication */         \
    SIMPLE_TASK(var_med),             \
    /* Rest of common meds */         \
    SIMPLE_TASK(MED_GABA),            \
    SIMPLE_TASK(MED_AMLODIPIN),       \
    SIMPLE_TASK(MED_VIAGRA),          \
    SPACER_TASK()

// --------------------- Types ---------------------
struct TimeRecord {
  uint8_t hours;
  uint8_t minutes;
  uint8_t seconds;
};

struct TaskEntry {
  char message[16];
  byte flags;
  unsigned long time;  // For relative timer - offset in seconds
};

struct TaskEntry tasks[] = {
    SIMPLE_TASK(MED_WALK),
    MEDICATION_BLOCK(MED_TRIGRIM),
    BLOCK_SEPARATOR(),

    SIMPLE_TASK(MED_WALK),

    MEDICATION_BLOCK(MED_VEROSHPIRON),
    BLOCK_SEPARATOR(),

    SIMPLE_TASK(MED_WALK),


    SIMPLE_TASK(MED_VETMEDIN),
    RELATIVE_TIMER_TASK(MED_FEED, HALF_HOUR),
    SIMPLE_TASK(MED_WALK),
    RELATIVE_TIMER_TASK(MED_URSOSAN, TEN_MINUTES),
    SIMPLE_TASK(MED_SLEEP)
};

#define ALL_MSGS (sizeof(tasks) / sizeof(tasks[0]))
#define STATE_EEPROM_ADDRESS 0x00

// --------------------- Global State ---------------------
struct CurrentState {
  byte curStep;
  byte nextAlarmFlags;
  unsigned long nextAlarm; // Time when timer should trigger (in seconds from start of day)
};

CurrentState globalState = { 0, 0, 0 };

// --------------------- System States ---------------------
enum SystemState { NORMAL, ALARM_RINGING, ALARM_STOPPED };
SystemState sysState = NORMAL;

// --------------------- Melody ---------------------
int melody[] = {
  NOTE_C5, NOTE_D5, NOTE_E5, NOTE_F5, NOTE_G5, NOTE_A5, NOTE_B5, NOTE_C6
};
unsigned long duration = NOTE_DURATION;  // Note duration in milliseconds

void playMelody(unsigned long duration) {
  for (int i = 0; i < 8; i++) {
    tone(PASSIVE_BUZZER_PIN, melody[i], duration);

    // Check button during the note duration
    unsigned long startTime = millis();
    while (millis() - startTime < duration) {
      if (digitalRead(BUTTON_PIN) == HIGH) {
        noTone(PASSIVE_BUZZER_PIN);
        return;
      }
      delay(10);  // Small delay to prevent too frequent checks
    }
  }
  noTone(PASSIVE_BUZZER_PIN);
}

// --------------------- Time Functions ---------------------
TimeRecord getCurrentTime() {
  TimeRecord t;
  bool is24, ampm;
  t.hours = rtclock.getHour(is24, ampm);
  t.minutes = rtclock.getMinute();
  t.seconds = rtclock.getSecond();
  return t;
}

unsigned long timeToSeconds(const TimeRecord &t) {
  return t.hours * 3600UL + t.minutes * 60UL + t.seconds;
}

TimeRecord secondsToTime(unsigned long totalSeconds) {
  TimeRecord t;
  t.hours = totalSeconds / 3600;
  totalSeconds %= 3600;
  t.minutes = totalSeconds / 60;
  t.seconds = totalSeconds % 60;
  return t;
}

void print2digits(byte num) {
  if (num < 10) {
    lcd.print("0");
  }
  lcd.print(num);
}

void displayTime(const bool isCountdown, const TimeRecord &t) {
  lcd.setCursor(7, 1);
  if(isCountdown) {
    lcd.print("T");
  } else {
    lcd.print(" ");
  }
  print2digits(t.hours);
  lcd.print(":");
  print2digits(t.minutes);
  lcd.print(":");
  print2digits(t.seconds);

}

// --------------------- setup() ---------------------
void setup() {
  pinMode(BUTTON_PIN, INPUT);
  pinMode(PASSIVE_BUZZER_PIN, OUTPUT);

  Serial.begin(9600);
  while (!Serial) {};

  // Read state from EEPROM into globalState structure
  EEPROM.get(STATE_EEPROM_ADDRESS, globalState);

  lcd.init();
  lcd.backlight();

  // Uncomment to set time if needed:
  // DateTime curDt = DateTime(__DATE__, __TIME__);
  // rtclock.setEpoch(curDt.unixtime());
  // rtclock.setClockMode(false);
}

// --------------------- loop() ---------------------
void loop() {
  TimeRecord currentTime = getCurrentTime();
  unsigned long currentSeconds = timeToSeconds(currentTime);
  TaskEntry curTask = tasks[globalState.curStep];

  // Display current task and state on LCD
  lcd.setCursor(0, 0);
  lcd.print(curTask.message);
  lcd.setCursor(0, 1);
  lcd.print(globalState.curStep);

  // System state processing
  switch (sysState) {
    case NORMAL: {
      // If task has timer and trigger time is not set, set it
      if ((curTask.flags & HAS_REALTIVE_TIMER) && (globalState.nextAlarm == 0)) {
        globalState.nextAlarm = currentSeconds + curTask.time;
        // Log to Serial: output time when next alarm will trigger
        unsigned long alarmSec = globalState.nextAlarm % 86400;  // Convert to day range
        TimeRecord alarmTime = secondsToTime(alarmSec);
        Serial.print("Next alarm will be activated at: ");
        Serial.print(alarmTime.hours);
        Serial.print(":");
        if (alarmTime.minutes < 10) Serial.print("0");
        Serial.print(alarmTime.minutes);
        Serial.print(":");
        if (alarmTime.seconds < 10) Serial.print("0");
        Serial.println(alarmTime.seconds);

        EEPROM.put(STATE_EEPROM_ADDRESS, globalState);
      }
      // If task has timer and trigger time has come, switch to alarm mode
      if ((curTask.flags & HAS_REALTIVE_TIMER) && (globalState.nextAlarm > 0) && (currentSeconds >= globalState.nextAlarm)) {
        sysState = ALARM_RINGING;
      }
      // In normal mode, if button is pressed, move to next task
      if (digitalRead(BUTTON_PIN) == HIGH) {
        lcd.clear();
        globalState.curStep++;
        if (globalState.curStep >= ALL_MSGS) {
          globalState.curStep = 0;
        }
        // Reset alarm time when moving to new task
        globalState.nextAlarm = 0;
        EEPROM.put(STATE_EEPROM_ADDRESS, globalState);
        delay(300); // Debounce
      }
      break;
    }
    case ALARM_RINGING: {
      // Alarm mode: play melody
      playMelody(duration);
      // If button pressed - stop melody and switch to ALARM_STOPPED state
      if (digitalRead(BUTTON_PIN) == HIGH) {
        sysState = ALARM_STOPPED;
        delay(300); // Debounce
      }
      break;
    }
    case ALARM_STOPPED: {
      // After alarm is stopped, wait for next button press to move to next task
      if (digitalRead(BUTTON_PIN) == HIGH) {
        sysState = NORMAL;
        lcd.clear();
        globalState.curStep++;
        if (globalState.curStep >= ALL_MSGS) {
          globalState.curStep = 0;
        }
        // Reset alarm time when moving to new task
        globalState.nextAlarm = 0;
        EEPROM.put(STATE_EEPROM_ADDRESS, globalState);
        delay(300); // Debounce
      }
      break;
    }
  } // switch

  // Display current time or countdown on LCD
  if (globalState.nextAlarm > 0) {
    // If alarm is set, show countdown
    if (currentSeconds < globalState.nextAlarm) {
      unsigned long timeLeft = globalState.nextAlarm - currentSeconds;
      displayTime(true, secondsToTime(timeLeft));
    } else {
      displayTime(false, currentTime);  // Show current time if countdown finished
    }

  } else {
    // No alarm set, show current time
    displayTime(false, currentTime);
  }
  delay(150);
}