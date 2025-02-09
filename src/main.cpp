/**
 * Blink
 *
 * Turns on an LED on for one second,
 * then off for one second, repeatedly.
 */
#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DS3231.h>
#include <EEPROM.h>
#include "pitches.h"

#define BUTTON_PIN 8
#define PASSIVE_BUZZER_PIN 9

LiquidCrystal_I2C lcd(0x27, 16, 2);  // LCD с адресом 0x27, 16 символов, 2 строки
DS3231 rtclock;

#define HAS_NO_TIMER       0b00000000
#define HAS_REALTIVE_TIMER 0b00000001
#define HAS_ABSOLUTE_TIMER 0b00000010

#define HALF_HOUR 1800
// #define HALF_HOUR 3
#define TEN_MINUTES 600



// --------------------- Типы ---------------------
struct TimeRecord {
  uint8_t hours;
  uint8_t minutes;
  uint8_t seconds;
};

struct TaskEntry {
  char message[16];
  byte flags;
  unsigned long time;  // для относительного таймера — смещение в секундах
};

struct TaskEntry tasks[] = {
    { "Antepsin 1/4",    HAS_NO_TIMER,       0          },
    { "Kvamatel 1/6",    HAS_NO_TIMER,       0          },
    { "Vetmedin 1",      HAS_NO_TIMER,       0          },
    { "Feed",            HAS_REALTIVE_TIMER, HALF_HOUR  },
    { "Trigrim 1/4",     HAS_NO_TIMER,       0          },
    { "Gaba 1ml",        HAS_NO_TIMER,       0          },
    { "Amlodipin 1/15",  HAS_NO_TIMER,       0          },
    { "Toreo 0.13ml",    HAS_NO_TIMER,       0          },
    { "Spacer",          HAS_REALTIVE_TIMER, TEN_MINUTES},

    { "Feed",            HAS_NO_TIMER,       0          },

    { "Antepsin 1/4",    HAS_NO_TIMER,       0          },
    { "Kvamatel 1/6",    HAS_NO_TIMER,       0          },
    { "Vetmedin 1",      HAS_NO_TIMER,       0          },
    { "Feed",            HAS_REALTIVE_TIMER, HALF_HOUR  },
    { "Veroshpiron 1/4", HAS_NO_TIMER,       0          },
    { "Gaba 1ml",        HAS_NO_TIMER,       0          },
    { "Amlodipin 1/15",  HAS_NO_TIMER,       0          },
    { "Toreo 0.13ml",    HAS_NO_TIMER,       0          },
    { "Spacer",          HAS_REALTIVE_TIMER, TEN_MINUTES},

    { "Vetmedin 1",      HAS_NO_TIMER,       0          },
    { "Feed",            HAS_REALTIVE_TIMER, HALF_HOUR  },
    { "Ursosan 1/6",     HAS_REALTIVE_TIMER, TEN_MINUTES},
    { "Sleep",           HAS_NO_TIMER,       0          }
};

#define ALL_MSGS (sizeof(tasks) / sizeof(tasks[0]))
#define STATE_EEPROM_ADDRESS 0x00

// --------------------- Глобальное состояние ---------------------
struct CurrentState {
  byte curStep;
  byte nextAlarmFlags;
  unsigned long nextAlarm; // время срабатывания таймера (в секундах от начала дня)
};

CurrentState globalState = { 0, 0, 0 };

// --------------------- Состояния системы ---------------------
enum SystemState { NORMAL, ALARM_RINGING, ALARM_STOPPED };
SystemState sysState = NORMAL;

// --------------------- Мелодия ---------------------
int melody[] = {
  NOTE_C5, NOTE_D5, NOTE_E5, NOTE_F5, NOTE_G5, NOTE_A5, NOTE_B5, NOTE_C6
};
int duration = 500;  // длительность ноты в миллисекундах

void playMelody(int duration) {
  for (int i = 0; i < 8; i++) {
    tone(PASSIVE_BUZZER_PIN, melody[i], duration);
    delay(duration / 2);
    if(digitalRead(BUTTON_PIN) == HIGH) {
      break;
    }
  }
  noTone(PASSIVE_BUZZER_PIN);
}

// --------------------- Функции работы со временем ---------------------
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

void displayTime(const TimeRecord &t) {
  lcd.setCursor(8, 1);
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

  // Чтение состояния из EEPROM в структуру globalState
  EEPROM.get(STATE_EEPROM_ADDRESS, globalState);

  lcd.init();
  lcd.backlight();

  // При необходимости установки времени раскомментируйте:
  // DateTime curDt = DateTime(__DATE__, __TIME__);
  // rtclock.setEpoch(curDt.unixtime());
  // rtclock.setClockMode(false);
}

// --------------------- loop() ---------------------
void loop() {
  TimeRecord currentTime = getCurrentTime();
  unsigned long currentSeconds = timeToSeconds(currentTime);
  TaskEntry curTask = tasks[globalState.curStep];

  // Отображение текущего задания и состояния на LCD
  lcd.setCursor(0, 0);
  lcd.print(curTask.message);
  lcd.setCursor(0, 1);
  lcd.print(globalState.curStep);

  // Обработка состояний системы
  switch (sysState) {
    case NORMAL: {
      // Если задание с таймером и время срабатывания ещё не установлено, устанавливаем его
      if ((curTask.flags & HAS_REALTIVE_TIMER) && (globalState.nextAlarm == 0)) {
        globalState.nextAlarm = currentSeconds + curTask.time;
        // Логирование в Serial: вывод времени, когда сработает следующий таймер.
        unsigned long alarmSec = globalState.nextAlarm % 86400;  // приводим к диапазону суток
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
      // Если задание с таймером и время срабатывания наступило, переходим в режим тревоги
      if ((curTask.flags & HAS_REALTIVE_TIMER) && (globalState.nextAlarm > 0) && (currentSeconds >= globalState.nextAlarm)) {
        sysState = ALARM_RINGING;
      }
      // В нормальном режиме, если кнопка нажата, переходим к следующему заданию
      if (digitalRead(BUTTON_PIN) == HIGH) {
        lcd.clear();
        globalState.curStep++;
        if (globalState.curStep >= ALL_MSGS) {
          globalState.curStep = 0;
        }
        // Сброс времени тревоги при переходе к новому заданию
        globalState.nextAlarm = 0;
        EEPROM.put(STATE_EEPROM_ADDRESS, globalState);
        delay(300); // антидребезг
      }
      break;
    }
    case ALARM_RINGING: {
      // Режим тревоги: проигрываем мелодию
      playMelody(duration);
      // Если кнопка нажата – прекращаем мелодию и переходим в состояние ALARM_STOPPED
      if (digitalRead(BUTTON_PIN) == HIGH) {
        sysState = ALARM_STOPPED;
        delay(300); // антидребезг
      }
      break;
    }
    case ALARM_STOPPED: {
      // После остановки тревоги ждём следующего нажатия кнопки для перехода к следующему заданию
      if (digitalRead(BUTTON_PIN) == HIGH) {
        sysState = NORMAL;
        lcd.clear();
        globalState.curStep++;
        if (globalState.curStep >= ALL_MSGS) {
          globalState.curStep = 0;
        }
        // Сброс времени тревоги при переходе к новому заданию
        globalState.nextAlarm = 0;
        EEPROM.put(STATE_EEPROM_ADDRESS, globalState);
        delay(300); // антидребезг
      }
      break;
    }
  } // switch

  // Вывод текущего времени на LCD
  displayTime(currentTime);
  delay(150);
}