#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <U8glib.h>
#include <avr/wdt.h>
#include "config_secure.h"          // ← секретный PIN (в .gitignore!)

U8GLIB_SH1106_128X64 u8g(U8G_I2C_OPT_NO_ACK);
SoftwareSerial mySerial(2, 3);
DFRobotDFPlayerMini myDFPlayer;

// === Пины ===
const uint8_t BTN_NEXT       = 4;
const uint8_t BTN_VOL_UP     = 5;
const uint8_t BTN_VOL_DOWN   = 6;
const uint8_t BTN_PLAY_PAUSE = 7;
const uint8_t LED_PIN        = 8;
const uint8_t BTN_PREV       = 9;
const uint8_t BTN_EQ         = 10;

// === Роли и PIN-защита (Задание 11) ===
enum Role { ROLE_USER, ROLE_TECHNICIAN };
Role current_role = ROLE_USER;

char entered_pin[5] = "0000";
uint8_t pin_pos = 0;
bool pin_entry_mode = false;
unsigned long eq_press_start = 0;
uint8_t pin_attempts = 0;

// === Эквалайзер (6 пресетов) ===
uint8_t eq_preset = 4;                    // 0=Normal, 1=Pop, 2=Rock, 3=Jazz, 4=Classic, 5=Bass
const uint8_t EQ_TYPES[6] = {
  DFPLAYER_EQ_NORMAL,
  DFPLAYER_EQ_POP,
  DFPLAYER_EQ_ROCK,
  DFPLAYER_EQ_JAZZ,
  DFPLAYER_EQ_CLASSIC,
  DFPLAYER_EQ_BASS
};

// === Состояние плеера ===
int current_track = 1;
unsigned long track_start_time = 0;
unsigned long track_played_seconds = 0;
unsigned long residual_ms = 0;
bool is_playing = false;

int current_volume = 25;
const int VOL_SHORT = 2;
const int VOL_LONG  = 5;

// === Кнопки громкости ===
unsigned long btn_up_time = 0;
unsigned long btn_down_time = 0;
bool btn_up_active = false;
bool btn_down_active = false;

// === LED ===
unsigned long led_timer = 0;
bool led_state = false;

// === Оптимизация ===
bool display_needs_update = true;
bool just_sent_play_command = false;

// ===================================================================
// =========================== ФУНКЦИИ ==============================
// ===================================================================

void updateDisplay() {
  u8g.firstPage();
  do {
    u8g.setFont(u8g_font_6x10);
    u8g.setDefaultForegroundColor();

    u8g.drawStr(0, 10, "MP3 Player");

    char trackStr[32];
    sprintf(trackStr, "Track: %04d %s", current_track, is_playing ? "Play" : "Pause");
    u8g.drawStr(0, 22, trackStr);

    // Таймер
    unsigned long elapsed = track_played_seconds;
    if (is_playing) elapsed += (millis() - track_start_time + residual_ms) / 1000;
    char timeStr[32];
    sprintf(timeStr, "%02lu:%02lu", elapsed / 60, elapsed % 60);
    u8g.drawStr(0, 35, timeStr);

    // EQ индикатор
    const char* eqText = (current_role == ROLE_TECHNICIAN) ? "EQ" : (eq_preset == 4 ? "CL" : "RO");
    u8g.drawStr(110, 35, eqText);

    // Громкость
    char volStr[20];
    sprintf(volStr, "Vol: %02d", current_volume);
    u8g.drawStr(0, 52, volStr);

    u8g.drawFrame(42, 46, 60, 8);
    int vol_bar = map(current_volume, 0, 30, 0, 58);
    u8g.drawBox(43, 47, vol_bar, 6);

  } while (u8g.nextPage());
}

void setVolume(int vol) {
  vol = constrain(vol, 0, 30);
  if (vol != current_volume) {
    current_volume = vol;
    myDFPlayer.volume(current_volume);
    display_needs_update = true;
  }
}

void setEQ() {
  myDFPlayer.EQ(EQ_TYPES[eq_preset]);
  display_needs_update = true;
}

void startNewTrack() {
  while (myDFPlayer.available()) myDFPlayer.read();   // очистка буфера

  Serial.print(F("Playing track: "));
  Serial.println(current_track);

  myDFPlayer.playMp3Folder(current_track);
  just_sent_play_command = true;

  track_start_time = millis();
  residual_ms = 0;
  track_played_seconds = 0;
  is_playing = true;

  setEQ();
  display_needs_update = true;
}

void togglePlayPause() {
  if (is_playing) {
    myDFPlayer.pause();
    unsigned long delta = millis() - track_start_time + residual_ms;
    track_played_seconds += delta / 1000;
    residual_ms = delta % 1000;
    Serial.println(F("Paused"));
  } else {
    if (track_played_seconds == 0 && track_start_time == 0) {
      startNewTrack();
      return;
    }
    myDFPlayer.start();
    track_start_time = millis();
    Serial.println(F("Resumed"));
  }
  is_playing = !is_playing;
  display_needs_update = true;
}

void updateLED() {
  unsigned long now = millis();
  if (is_playing) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    uint16_t period = (track_played_seconds == 0) ? 1000 : 200;
    if (now - led_timer >= period) {
      led_timer = now;
      led_state = !led_state;
      digitalWrite(LED_PIN, led_state);
    }
  }
}

// === Обработка ввода PIN ===
void handlePinEntry(bool vol_up, bool vol_down, bool play, bool eq) {
  if (!pin_entry_mode) return;

  static int current_digit = 0;

  if (vol_up)  current_digit = (current_digit + 1) % 10;
  if (vol_down) current_digit = (current_digit + 9) % 10;

  if (play) {
    entered_pin[pin_pos] = '0' + current_digit;
    pin_pos++;
    if (pin_pos == 4) {
      if (strcmp(entered_pin, SERVICE_PIN) == 0) {
        current_role = ROLE_TECHNICIAN;
        Serial.println(F("Technician mode activated (PIN OK)"));
        pin_entry_mode = false;
      } else {
        pin_attempts++;
        Serial.println(F("Wrong PIN"));
        if (pin_attempts >= 3) {
          Serial.println(F("PIN blocked 10 sec"));
          delay(10000);
          pin_attempts = 0;
        }
        pin_pos = 0;
      }
      display_needs_update = true;
    }
  }

  if (eq) {  // отмена
    pin_entry_mode = false;
    pin_pos = 0;
    Serial.println(F("PIN entry cancelled"));
    display_needs_update = true;
  }
}

// ===================================================================
// =========================== SETUP & LOOP ==========================
// ===================================================================

void setup() {
  Serial.begin(115200);
  mySerial.begin(9600);

  // Настройка пинов
  pinMode(BTN_NEXT,       INPUT_PULLUP);
  pinMode(BTN_VOL_UP,     INPUT_PULLUP);
  pinMode(BTN_VOL_DOWN,   INPUT_PULLUP);
  pinMode(BTN_PLAY_PAUSE, INPUT_PULLUP);
  pinMode(BTN_PREV,       INPUT_PULLUP);
  pinMode(BTN_EQ,         INPUT_PULLUP);
  pinMode(LED_PIN,        OUTPUT);

  wdt_enable(WDTO_2S);                     // Защита от зависаний

  if (!myDFPlayer.begin(mySerial, true, false)) {
    Serial.println(F("DFPlayer не найден!"));
    while (true) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(300);
    }
  }

  myDFPlayer.volume(current_volume);
  setEQ();
  Serial.println(F("MP3 Player готов!"));
  led_timer = millis();
  display_needs_update = true;
  updateDisplay();
}

void loop() {
  wdt_reset();

  bool next = digitalRead(BTN_NEXT) == LOW;
  bool up   = digitalRead(BTN_VOL_UP) == LOW;
  bool down = digitalRead(BTN_VOL_DOWN) == LOW;
  bool play = digitalRead(BTN_PLAY_PAUSE) == LOW;
  bool prev = digitalRead(BTN_PREV) == LOW;
  bool eq   = digitalRead(BTN_EQ) == LOW;

  static bool last_next = false;
  static bool last_play = false;
  static bool last_prev = false;
  static bool last_eq   = false;

  // === Вход в режим ввода PIN (удержание EQ 3 сек) ===
  if (eq && !last_eq) {
    if (eq_press_start == 0) eq_press_start = millis();
  }
  if (eq && (millis() - eq_press_start > 3000) && !pin_entry_mode && current_role == ROLE_USER) {
    pin_entry_mode = true;
    pin_pos = 0;
    Serial.println(F("Enter PIN (4 digits): Vol+/- = digit, Play = confirm, EQ = cancel"));
    display_needs_update = true;
  }
  if (!eq) eq_press_start = 0;
  last_eq = eq;

  // ====================== PIN MODE ======================
  if (pin_entry_mode) {
    handlePinEntry(up, down, play, eq);
  }
  // ====================== NORMAL MODE ======================
  else {
    // Короткое нажатие EQ (разное поведение по роли)
    if (eq && !last_eq) {
      if (current_role == ROLE_USER) {
        eq_preset = (eq_preset == 4) ? 2 : 4;           // только Classic ↔ Rock
      } else {
        eq_preset = (eq_preset + 1) % 6;                // полный цикл в Technician
      }
      setEQ();
    }
    last_eq = eq;   // обновляем для следующего цикла

    // Prev
    if (prev && !last_prev) {
      current_track--;
      if (current_track < 1) current_track = 9999;
      startNewTrack();
    }
    last_prev = prev;

    // Next
    if (next && !last_next) {
      current_track++;
      if (current_track > 9999) current_track = 1;
      startNewTrack();
    }
    last_next = next;

    // Play/Pause
    if (play && !last_play) {
      togglePlayPause();
    }
    last_play = play;

    // Громкость Up
    if (up && !btn_up_active) {
      btn_up_active = true;
      btn_up_time = millis();
      setVolume(current_volume + VOL_SHORT);
    }
    if (up && btn_up_active && (millis() - btn_up_time >= 500)) {
      btn_up_time = millis();
      setVolume(current_volume + VOL_LONG);
    }
    if (!up) btn_up_active = false;

    // Громкость Down
    if (down && !btn_down_active) {
      btn_down_active = true;
      btn_down_time = millis();
      setVolume(current_volume - VOL_SHORT);
    }
    if (down && btn_down_active && (millis() - btn_down_time >= 500)) {
      btn_down_time = millis();
      setVolume(current_volume - VOL_LONG);
    }
    if (!down) btn_down_active = false;
  }

  // Обработка событий DFPlayer
  if (myDFPlayer.available()) {
    if (just_sent_play_command) {
      while (myDFPlayer.available()) myDFPlayer.read();
      just_sent_play_command = false;
    } else {
      uint8_t type = myDFPlayer.readType();
      if (type == DFPlayerPlayFinished && is_playing) {
        Serial.println(F("Track finished -> next"));
        unsigned long delta = millis() - track_start_time + residual_ms;
        track_played_seconds += delta / 1000;
        residual_ms = delta % 1000;

        current_track++;
        if (current_track > 9999) current_track = 1;
        startNewTrack();
      }
    }
  }

  updateLED();

  if (display_needs_update) {
    updateDisplay();
    display_needs_update = false;
  }

  delay(100);   // оптимальный интервал
}