// Three input producers feeding one event queue:
//
//   1. A keyboard over USB-CDC serial. A terminal sends no key-up events, so
//      each press is auto-released after AUTOREPEAT_MS unless it is pressed
//      again -- crude, but entirely playable and available from the first boot.
//   2. The onboard buttons: A and B sit behind the XL9535 I2C expander, BOOT is
//      a plain GPIO0. (Note: A is NOT GPIO12 -- that label in pins_arduino.h
//      refers to the edge connector, and GPIO12 is SPI SCLK.)
//   3. An optional I2C joystick controller at 0x5A on the shared Wire bus.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

extern "C" {
#include "initBoard.h"
#include "doomkeys.h"
}
#include "k10_input.h"

#ifndef K10_GAME_CONTROLLER
#define K10_GAME_CONTROLLER 1
#endif
#if K10_GAME_CONTROLLER
#include <GameController.h>
#endif

#define BOOT_GPIO      0
#define AUTOREPEAT_MS  140

// Persistent since-boot press counters, so presses can be made at any time and
// read back later without having to synchronise with a capture window.
volatile uint32_t k10_pressA = 0, k10_pressB = 0, k10_pressBoot = 0;
static bool s_pollButtonsEnabled = true;
// BOOT (GPIO0) is also the I2S bit clock -- k10_sound.cpp disables this once
// it owns the pin. See k10_input_set_boot_enabled().
static bool s_bootButtonEnabled = true;
static uint8_t s_lastA = 1, s_lastB = 1, s_lastBoot = 1;

typedef struct { uint8_t pressed; uint8_t key; } KeyEvent;

static QueueHandle_t s_queue = nullptr;

// Set false once input is trusted; the logging is noisy during play.
bool k10_input_debug = false;   // toggle at runtime with "="

static const char *keyName(uint8_t k)
{
  switch (k) {
    case KEY_UPARROW:    return "UP";
    case KEY_DOWNARROW:  return "DOWN";
    case KEY_LEFTARROW:  return "LEFT";
    case KEY_RIGHTARROW: return "RIGHT";
    case KEY_STRAFE_L:   return "STRAFE_L";
    case KEY_STRAFE_R:   return "STRAFE_R";
    case KEY_USE:        return "USE";
    case KEY_FIRE:       return "FIRE";
    case KEY_ESCAPE:     return "ESC";
    case KEY_ENTER:      return "ENTER";
    case KEY_TAB:        return "TAB";
    default:             return "?";
  }
}

// Each producer owns its own held keys. A release from one source must not
// cancel a key still held by another source. Queue overflow is retried on the
// next input tick instead of losing a release and leaving a Doom key stuck.
enum InputSource { SERIAL_INPUT, BOARD_INPUT, CONTROLLER_INPUT, INPUT_SOURCE_COUNT };
static bool s_keys[INPUT_SOURCE_COUNT][256] = {};
static bool s_queuedKeys[256] = {};

static void setKey(InputSource source, uint8_t key, bool pressed)
{
  s_keys[source][key] = pressed;
}

static void flushKeys()
{
  for (unsigned key = 0; key < 256; ++key) {
    bool pressed = false;
    for (unsigned source = 0; source < INPUT_SOURCE_COUNT; ++source)
      pressed |= s_keys[source][key];
    if (pressed == s_queuedKeys[key]) continue;
    KeyEvent event = {uint8_t(pressed), uint8_t(key)};
    if (xQueueSend(s_queue, &event, 0) != pdTRUE) return;
    s_queuedKeys[key] = pressed;
    if (k10_input_debug)
      Serial.printf("[key] %s 0x%02X (%s)\n", pressed ? "DOWN" : "up", key, keyName(key));
  }
}

// --- serial keyboard --------------------------------------------------------

// Tracks synthetic key-up deadlines for keys pressed over serial.
#define MAX_HELD 12
static struct { uint8_t key; uint32_t expiry; } s_held[MAX_HELD];

static void serialPress(uint8_t key)
{
  uint32_t now = millis();
  for (int i = 0; i < MAX_HELD; i++) {
    if (s_held[i].key == key) { s_held[i].expiry = now + AUTOREPEAT_MS; return; }  // still held
  }
  for (int i = 0; i < MAX_HELD; i++) {
    if (s_held[i].key == 0) {
      s_held[i].key = key;
      s_held[i].expiry = now + AUTOREPEAT_MS;
      setKey(SERIAL_INPUT, key, true);
      return;
    }
  }
}

static void serialExpire()
{
  uint32_t now = millis();
  for (int i = 0; i < MAX_HELD; i++) {
    if (s_held[i].key && (int32_t)(now - s_held[i].expiry) >= 0) {
      setKey(SERIAL_INPUT, s_held[i].key, false);
      s_held[i].key = 0;
    }
  }
}

static uint8_t mapSerial(int c)
{
  switch (c) {
    case 'w': case 'W': return KEY_UPARROW;
    case 's': case 'S': return KEY_DOWNARROW;
    case 'a': case 'A': return KEY_LEFTARROW;
    case 'd': case 'D': return KEY_RIGHTARROW;
    case ',': case '<': return KEY_STRAFE_L;
    case '.': case '>': return KEY_STRAFE_R;
    case ' ':           return KEY_USE;
    case 'f': case 'F': return KEY_FIRE;
    case '\r': case '\n': return KEY_ENTER;
    case 27:            return KEY_ESCAPE;
    case '\t':          return KEY_TAB;
    case 0x7f: case 8:  return KEY_BACKSPACE;
    case 'y': case 'Y': return 'y';   // menu confirmations
    case 'n': case 'N': return 'n';
    case '+':           return KEY_EQUALS;
    case '-':           return KEY_MINUS;
    default:
      if (c >= '1' && c <= '9') return (uint8_t)c;   // weapon select
      return 0;
  }
}

#if K10_GAME_CONTROLLER
static void dumpControllerStatus();
#endif

// Returns true if the byte was a debug command and should not reach Doom.
static bool debugCommand(int c)
{
  switch (c) {
    case '/':
      Serial.printf("[dbg] presses since boot: A=%lu B=%lu BOOT=%lu | raw A=%u B=%u BOOT=%u | poll=%s\n",
                    (unsigned long)k10_pressA, (unsigned long)k10_pressB,
                    (unsigned long)k10_pressBoot,
                    digital_read(eP5_KeyA), digital_read(eP11_KeyB), digitalRead(BOOT_GPIO),
                    s_pollButtonsEnabled ? "on" : "off");
#if K10_GAME_CONTROLLER
      dumpControllerStatus();
#endif
      return true;
    case '[':
      digital_write(eAmp_Gain, 0);
      digital_write(eLCD_BLK, 1);   // re-assert: the helper is not bit-safe
      Serial.println("[dbg] amp OFF (eAmp_Gain=0), backlight re-asserted");
      return true;
    case ']':
      digital_write(eAmp_Gain, 1);
      digital_write(eLCD_BLK, 1);
      Serial.println("[dbg] amp ON (eAmp_Gain=1), backlight re-asserted");
      return true;
    case ';':
      s_pollButtonsEnabled = !s_pollButtonsEnabled;
      if (!s_pollButtonsEnabled) {
        memset(s_keys[BOARD_INPUT], 0, sizeof(s_keys[BOARD_INPUT]));
        s_lastA = s_lastB = s_lastBoot = 1;
      }
      Serial.printf("[dbg] button polling %s\n", s_pollButtonsEnabled ? "ENABLED" : "DISABLED");
      return true;
    case '=':
      k10_input_debug = !k10_input_debug;
      Serial.printf("[dbg] key logging %s\n", k10_input_debug ? "on" : "off");
      return true;
    default:
      return false;
  }
}

static void pollSerial()
{
  while (Serial.available()) {
    int c = Serial.read();
    if (debugCommand(c)) continue;
    // Arrow keys arrive as ESC [ A/B/C/D. Peek rather than treating ESC as menu.
    if (c == 27 && Serial.available() >= 2 && Serial.peek() == '[') {
      Serial.read();
      int d = Serial.read();
      switch (d) {
        case 'A': serialPress(KEY_UPARROW);    continue;
        case 'B': serialPress(KEY_DOWNARROW);  continue;
        case 'C': serialPress(KEY_RIGHTARROW); continue;
        case 'D': serialPress(KEY_LEFTARROW);  continue;
        default: continue;
      }
    }
    uint8_t k = mapSerial(c);
    if (k) serialPress(k);
  }
  serialExpire();
}

// --- onboard buttons --------------------------------------------------------


static void pollButtons()
{
  uint8_t a = digital_read(eP5_KeyA);     // active low
  uint8_t b = digital_read(eP11_KeyB);

  if (a != s_lastA) { if (!a) k10_pressA++; setKey(BOARD_INPUT, KEY_FIRE, !a); s_lastA = a; }
  if (b != s_lastB) { if (!b) k10_pressB++; setKey(BOARD_INPUT, KEY_USE, !b);  s_lastB = b; }

  if (!s_bootButtonEnabled) return;   // GPIO0 now belongs to I2S BCLK

  uint8_t boot = digitalRead(BOOT_GPIO);
  if (boot != s_lastBoot) { if (!boot) k10_pressBoot++; setKey(BOARD_INPUT, KEY_TAB, !boot); s_lastBoot = boot; }  // TODO: weapon cycle
}

// --- optional I2C controller ------------------------------------------------

#if K10_GAME_CONTROLLER
static GameController s_controller;
static bool s_controllerOnline = false;
static uint32_t s_controllerNextPoll = 0;

static void dumpControllerStatus()
{
  uint8_t x, y;
  if (s_controller.readAxes(x, y))
    Serial.printf("[controller] 0x5A responding | X=%u Y=%u | online=%s\n",
                  x, y, s_controllerOnline ? "yes" : "no");
  else
    Serial.println("[controller] no response at 0x5A");
}

// Hysteresis around the centre prevents jitter from tapping movement keys.
static void axisKeys(uint8_t value, uint8_t negative, uint8_t positive)
{
  bool low = s_keys[CONTROLLER_INPUT][negative];
  bool high = s_keys[CONTROLLER_INPUT][positive];
  setKey(CONTROLLER_INPUT, negative, value < (low ? 104 : 88));
  setKey(CONTROLLER_INPUT, positive, value > (high ? 152 : 168));
}

static void pollController()
{
  uint32_t now = millis();
  if ((int32_t)(now - s_controllerNextPoll) < 0) return;

  uint8_t x, y;
  uint8_t events[GameController::BUTTON_COUNT];
  bool ok = s_controller.readAxes(x, y);
  for (uint8_t i = 0; ok && i < GameController::BUTTON_COUNT; ++i)
    ok = s_controller.readButtonEvent((GameController::Button)i, events[i]);

  if (ok != s_controllerOnline)
    Serial.println(ok ? "[controller] connected at 0x5A" : "[controller] disconnected");
  s_controllerOnline = ok;
  s_controllerNextPoll = now + (ok ? 20 : 1000);
  if (!ok) {
    memset(s_keys[CONTROLLER_INPUT], 0, sizeof(s_keys[CONTROLLER_INPUT]));
    return;
  }

  axisKeys(x, KEY_LEFTARROW, KEY_RIGHTARROW);
  // The supplied controller demo treats high Y as upward movement.
  axisKeys(y, KEY_DOWNARROW, KEY_UPARROW);
  static const uint8_t keys[GameController::BUTTON_COUNT] = {
    KEY_ESCAPE, KEY_USE, KEY_FIRE, KEY_TAB, KEY_ENTER
  };
  for (uint8_t i = 0; i < GameController::BUTTON_COUNT; ++i) {
    switch (events[i]) {
      case GameController::PRESS_DOWN:
      case GameController::PRESS_REPEAT:
      case GameController::LONG_PRESS_START:
      case GameController::LONG_PRESS_HOLD:
        setKey(CONTROLLER_INPUT, keys[i], true);
        break;
      case GameController::PRESS_UP:
      case GameController::SINGLE_CLICK:
      case GameController::DOUBLE_CLICK:
      case GameController::NONE_PRESS:
        setKey(CONTROLLER_INPUT, keys[i], false);
        break;
    }
  }
}
#endif

// --- public -----------------------------------------------------------------

static void inputTask(void *)
{
  for (;;) {
    pollSerial();
    if (s_pollButtonsEnabled) pollButtons();

#if K10_GAME_CONTROLLER
    pollController();
#endif
    if (!s_bootButtonEnabled) setKey(BOARD_INPUT, KEY_TAB, false);
    flushKeys();
    vTaskDelay(pdMS_TO_TICKS(10));   // 100 Hz
  }
}

void k10_input_set_boot_enabled(int enabled)
{
  s_bootButtonEnabled = enabled;
}

void k10_input_begin(void)
{
  memset(s_held, 0, sizeof(s_held));
  s_queue = xQueueCreate(64, sizeof(KeyEvent));
  if (!s_queue) {
    Serial.println("[input] failed to allocate event queue");
    return;
  }
#if K10_GAME_CONTROLLER
  // Match the vendor K10.begin(): attach Wire after init_board(), using the
  // board's default SDA/SCL pins, without reinitializing the board or camera.
  if (Wire.begin()) {
    s_controller.begin(Wire);
    Serial.println("[controller] auto-detect enabled (0x5A)");
  } else {
    Serial.println("[controller] Wire initialization failed");
  }
#endif
  pinMode(BOOT_GPIO, INPUT_PULLUP);
  xTaskCreatePinnedToCore(inputTask, "k10_input", 4096, nullptr, 4, nullptr, 0);
}

int k10_input_poll(int *pressed, unsigned char *key)
{
  if (!s_queue) return 0;
  KeyEvent e;
  if (xQueueReceive(s_queue, &e, 0) != pdTRUE) return 0;
  *pressed = e.pressed;
  *key = e.key;
  if (k10_input_debug)
    Serial.printf("[key] -> doom: %s 0x%02X\n", e.pressed ? "DOWN" : "up", e.key);
  return 1;
}
