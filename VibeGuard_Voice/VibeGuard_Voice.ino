/*
  VibeGuard watch (Waveshare ESP32-S3 2.06" AMOLED touch watch).

  Two things happen on this watch:
    1. Hold the talk button, ask something, release: Gemini answers out loud.
    2. The laptop tells the watch the user's mood. If the mood stays sad or
       mad for a few seconds, the watch asks about it ("Why are you sad?")
       and the user can hold the talk button to reply.

  The laptop sends the mood with:   http://<watch-address>/mood?m=sad
  Start the laptop program with:    --watch http://vibeguard-watch.local

  The watch needs a WiFi network WITH internet (for Gemini), so use the same
  hotspot or home network as the laptop and the camera board.
*/
#define ARDUINOJSON_STRING_LENGTH_SIZE 4
#include <ArduinoJson.h>
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ESP_I2S.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <sys/time.h>

#include <AudioDriver.h>
#include <Codecs/es7210/ES7210.h>
#include <Codecs/es8311/ES8311.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include "vg_fonts.h"   // smooth fonts, made by make_fonts.py
// ENTER YOUR DETAILS HER

const char *WIFI_SSID = "YOUR_WIFI_NAME";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char *GEMINI_API_KEY = "YOUR_GEMINI_API_KEY";

// Use a 2.4 GHz Wi-Fi network.
// Keep your API key private.
// MODELS AND WATCH CONFIGURATIO

const char *ANSWER_MODEL = "gemini-3.5-flash-lite";
const char *VOICE_MODEL = "gemini-2.5-flash-preview-tts";

// The button you hold to talk. Set this to the pin your sketch uses.
constexpr int WATCH_TALK_PIN = 0;

constexpr int AMP_PIN = 46;
constexpr int MIC_CHANNEL = 0;

constexpr size_t MAX_SAMPLES = 16000 * 8;

// Colours. VG_RGB(r, g, b) turns normal 0-255 red, green, blue into the screen's format.
#define VG_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

constexpr uint16_t BLACK = 0x0000;
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t DIM = VG_RGB(120, 120, 135);      // hints and secondary text
constexpr uint16_t CYAN = VG_RGB(0, 255, 255);       // status page headings
constexpr uint16_t LOGO_PURPLE = VG_RGB(170, 90, 255);
constexpr uint16_t LOGO_PINK = VG_RGB(255, 90, 200);

// One colour per mood, as in the design.
constexpr uint16_t HAPPY_COLOUR = VG_RGB(255, 150, 30);    // orange
constexpr uint16_t NEUTRAL_COLOUR = VG_RGB(225, 228, 240); // silver
constexpr uint16_t SAD_COLOUR = VG_RGB(40, 140, 255);      // blue
constexpr uint16_t MAD_COLOUR = VG_RGB(255, 45, 45);       // red

// Screen layout (410 x 502).
constexpr int FACE_X = 205;             // centre of the face
constexpr int FACE_Y = 196;
constexpr int FACE_R = 60;
constexpr int WAVE_TOP = 418;           // the glowing wave fills below this
constexpr int BUTTON_X = 324;           // round button, bottom right
constexpr int BUTTON_Y = 464;
constexpr int MIC_X = 86;               // microphone icon, bottom left
constexpr int MIC_Y = 464;

// Animation speed on the home screen.
constexpr unsigned long WAVE_FRAME_MS = 120;  // lower = wave moves faster
constexpr unsigned long PULSE_MS = 160;       // glow breathing frame

// How long an error stays up before the watch goes back home.
constexpr unsigned long ERROR_SHOW_MS = 6000;
// MOOD CHECK-IN SETTING

// Name the laptop uses to find the watch: http://vibeguard-watch.local
const char *WATCH_NAME = "vibeguard-watch";

// The mood must last this long before the watch asks about it,
// so a passing expression does not trigger a question.
// (The laptop already waits 3 s before reporting a new mood.)
constexpr unsigned long CHECK_IN_AFTER_MS = 5000;

// Still sad or mad after a check-in: ask again after this long.
constexpr unsigned long CHECK_IN_REPEAT_MS = 120000;

// Minimum time between any two questions, even if the mood changes.
constexpr unsigned long CHECK_IN_COOLDOWN_MS = 45000;

// How long the watch waits for a reply before going back home.
constexpr unsigned long ANSWER_WAIT_MS = 30000;

// true = the watch also says the question out loud (one extra Gemini call).
const bool SPEAK_CHECK_IN = true;

// true = answers are spoken. false = answers are shown as text only, which
// uses no speech requests (the free speech limit is only a few per day).
const bool SPEAK_ANSWERS = true;

// How long a text answer stays on screen.
constexpr unsigned long READ_ANSWER_MS = 12000;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  12, 11, 4, 5, 6, 7
);

Arduino_GFX *display = new Arduino_CO5300(
  bus, 8, 0, 410, 502, 22, 0, 0, 0
);

// Everything is drawn into this full-screen picture first, then sent to
// the screen in one go: smooth edges and no flicker.
Arduino_Canvas *gfx = new Arduino_Canvas(410, 502, display);

I2SClass audio;
audio_driver::ES7210 microphone;
audio_driver::ES8311 speaker;
XPowersPMU power;
WebServer server(80);

bool audioStarted = false;
bool powerReady = false;
bool statusPage = false;
bool homeVisible = false;
bool previousTouch = false;
bool buttonArmed = false;
bool pulseOn = false;
bool serverStarted = false;

unsigned long releaseTime = 0;
unsigned long statusTime = 0;
unsigned long waveTime = 0;
unsigned long pulseTime = 0;
unsigned long statusBarTime = 0;
float wavePhase = 0;             // moves the bottom wave along

String lastError;

// Mood state, updated by the laptop through /mood.
String currentMood = "neutral";
unsigned long moodSince = 0;
bool checkInHandled = true;      // false while a new mood is waiting to be asked about
unsigned long lastCheckIn = 0;
bool awaitingAnswer = false;     // true after the watch has asked a question
unsigned long askedAt = 0;
String lastQuestion;
unsigned long readingUntil = 0;  // when a text answer should leave the screen
// PSRAM MEMORY FOR AUDIO AND API RESPONSE

class PSAllocator : public ArduinoJson::Allocator {
 public:
  void *allocate(size_t size) override {
    return heap_caps_malloc(
      size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
  }

  void deallocate(void *pointer) override {
    heap_caps_free(pointer);
  }

  void *reallocate(void *pointer, size_t size) override {
    return heap_caps_realloc(
      pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
  }
};

PSAllocator jsonMemory;

class ResponseBuffer : public Stream {
 public:
  char *data = nullptr;
  size_t length = 0;
  size_t capacity;
  bool overflow = false;

  explicit ResponseBuffer(size_t limit) : capacity(limit) {
    data = (char *)ps_malloc(limit + 1);
    if (data) data[0] = '\0';
  }

  ~ResponseBuffer() {
    free(data);
  }

  size_t write(uint8_t value) override {
    return write(&value, 1);
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    if (!data || size > capacity - length) {
      overflow = true;
      return 0;
    }

    memcpy(data + length, buffer, size);
    length += size;
    data[length] = '\0';
    return size;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// Explicit declarations for Arduino's sketch preprocessor.
bool apiCall(
  const char *model,
  JsonDocument &request,
  ResponseBuffer &response
);
void showHome();
bool speakAnswer(const String &answer);
// SCREEN DESIGN
//
// Everything is drawn into a full-screen picture in memory (fb), with
// smooth (anti-aliased) edges, then sent to the screen in one go.
// Text uses the smooth fonts in vg_fonts.h.

uint16_t *fb = nullptr;          // the picture in memory
uint16_t *flushBuf = nullptr;    // used to send part of the screen
constexpr int SCREEN_W = 410;
constexpr int SCREEN_H = 502;

// Declarations with default values (the "alpha" strength is 1 = solid).
void aaDisc(float cx, float cy, float r, uint16_t c, float alpha = 1);
void aaRing(float cx, float cy, float r, float w, uint16_t c, float alpha = 1);
void aaArc(float cx, float cy, float r, float w, float a0, float a1,
           uint16_t c, float alpha = 1);
void aaLine(float x0, float y0, float x1, float y1, float w, uint16_t c,
            float alpha = 1);
void aaRoundBox(float cx, float cy, float hw, float hh, float r, float w,
                uint16_t c, float alpha = 1);
void drawTextAA(const VGFont &f, int x, int y, const String &s,
                uint16_t c, float alpha = 1);
void drawTextCentered(const VGFont &f, int cx, int y, const String &s,
                      uint16_t c, float alpha = 1);
int drawWrappedAA(const VGFont &f, const String &message, int x, int y,
                  int maxWidth, int lastBaseline, int lineGap, uint16_t c,
                  bool centred = false);
// ---- Colours for the current mood ----------------------------

uint16_t moodAccent() {
  if (currentMood == "happy") return HAPPY_COLOUR;
  if (currentMood == "sad") return SAD_COLOUR;
  if (currentMood == "mad") return MAD_COLOUR;
  return NEUTRAL_COLOUR;
}

String moodLabel() {
  if (currentMood == "happy") return "Happy";
  if (currentMood == "sad") return "Sad";
  if (currentMood == "mad") return "Mad";
  return "Neutral";
}

// Same colour, darker. f = 0 is black, f = 1 is unchanged.
uint16_t scaleColour(uint16_t c, float f) {
  if (f <= 0) return BLACK;
  if (f > 1) f = 1;
  int r = ((c >> 11) & 31) * f;
  int g = ((c >> 5) & 63) * f;
  int b = (c & 31) * f;
  return (r << 11) | (g << 5) | b;
}

// Mix two colours. a = 0 gives bg, a = 32 gives fg.
static inline uint16_t mixColour(uint16_t bg, uint16_t fg, uint32_t a) {
  uint32_t f = (fg | ((uint32_t)fg << 16)) & 0x07E0F81F;
  uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81F;
  b += ((f - b) * a) >> 5;
  b &= 0x07E0F81F;
  return (uint16_t)(b | (b >> 16));
}

static inline float clamp01(float v) {
  return v < 0 ? 0 : (v > 1 ? 1 : v);
}

// Paint one pixel with a colour at a given strength (0 to 1).
static inline void blendPixel(int x, int y, uint16_t c, float a) {
  if (a <= 0.01f || x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
  uint16_t &p = fb[y * SCREEN_W + x];
  p = a >= 0.99f ? c : mixColour(p, c, (uint32_t)(a * 32 + 0.5f));
}

void clearArea(int x, int y, int w, int h) {
  for (int row = max(0, y); row < min(SCREEN_H, y + h); row++) {
    for (int col = max(0, x); col < min(SCREEN_W, x + w); col++) {
      fb[row * SCREEN_W + col] = BLACK;
    }
  }
}

// Send the whole picture to the screen.
void flushAll() {
  gfx->flush(true);
}

// Send just one rectangle of the picture to the screen (faster).
void flushArea(int x, int y, int w, int h) {
  x = max(0, x) & ~1;                // the screen wants even edges
  y = max(0, y) & ~1;
  w = min(SCREEN_W - x, (w + 3) & ~1);
  h = min(SCREEN_H - y, (h + 3) & ~1);
  if (w <= 0 || h <= 0) return;

  for (int row = 0; row < h; row++) {
    memcpy(flushBuf + row * w, fb + (y + row) * SCREEN_W + x, w * 2);
  }
  display->draw16bitRGBBitmap(x, y, flushBuf, w, h);
}

// ---- Smooth shapes ------------------------------------------------

// Filled circle.
void aaDisc(float cx, float cy, float r, uint16_t c, float alpha) {
  for (int y = floorf(cy - r - 1); y <= ceilf(cy + r + 1); y++) {
    for (int x = floorf(cx - r - 1); x <= ceilf(cx + r + 1); x++) {
      float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
      blendPixel(x, y, c, clamp01(r - d + 0.5f) * alpha);
    }
  }
}

// Circle outline of width w.
void aaRing(float cx, float cy, float r, float w, uint16_t c,
            float alpha) {
  float out = r + w;
  for (int y = floorf(cy - out); y <= ceilf(cy + out); y++) {
    for (int x = floorf(cx - out); x <= ceilf(cx + out); x++) {
      float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
      blendPixel(x, y, c, clamp01(w * 0.5f - fabsf(d - r) + 0.5f) * alpha);
    }
  }
}

// Part of a circle with rounded ends. Angles in degrees:
// 0 = right, 90 = down, 270 = up.
void aaArc(float cx, float cy, float r, float w, float a0, float a1,
           uint16_t c, float alpha) {
  const float rad = PI / 180.0f;
  float e0x = cx + r * cosf(a0 * rad), e0y = cy + r * sinf(a0 * rad);
  float e1x = cx + r * cosf(a1 * rad), e1y = cy + r * sinf(a1 * rad);
  float out = r + w;

  for (int y = floorf(cy - out); y <= ceilf(cy + out); y++) {
    for (int x = floorf(cx - out); x <= ceilf(cx + out); x++) {
      float dx = x - cx, dy = y - cy;
      float d = sqrtf(dx * dx + dy * dy);
      if (fabsf(d - r) > w) continue;

      float a = atan2f(dy, dx) / rad;
      while (a < a0) a += 360;
      while (a >= a0 + 360) a -= 360;

      float dist;
      if (a <= a1) {
        dist = fabsf(d - r);
      } else {
        float d0 = sqrtf((x - e0x) * (x - e0x) + (y - e0y) * (y - e0y));
        float d1 = sqrtf((x - e1x) * (x - e1x) + (y - e1y) * (y - e1y));
        dist = min(d0, d1);
      }
      blendPixel(x, y, c, clamp01(w * 0.5f - dist + 0.5f) * alpha);
    }
  }
}

// Straight line with rounded ends.
void aaLine(float x0, float y0, float x1, float y1, float w, uint16_t c,
            float alpha) {
  float vx = x1 - x0, vy = y1 - y0;
  float len2 = vx * vx + vy * vy;
  if (len2 < 0.0001f) len2 = 0.0001f;

  for (int y = floorf(min(y0, y1) - w); y <= ceilf(max(y0, y1) + w); y++) {
    for (int x = floorf(min(x0, x1) - w); x <= ceilf(max(x0, x1) + w); x++) {
      float t = clamp01(((x - x0) * vx + (y - y0) * vy) / len2);
      float px = x0 + t * vx - x, py = y0 + t * vy - y;
      float dist = sqrtf(px * px + py * py);
      blendPixel(x, y, c, clamp01(w * 0.5f - dist + 0.5f) * alpha);
    }
  }
}

// Distance to a rounded rectangle (negative inside).
static inline float roundBoxDistance(float px, float py, float hw, float hh,
                                     float r) {
  float qx = fabsf(px) - hw + r, qy = fabsf(py) - hh + r;
  float ox = max(qx, 0.0f), oy = max(qy, 0.0f);
  return sqrtf(ox * ox + oy * oy) + min(max(qx, qy), 0.0f) - r;
}

// Rounded rectangle, filled (w = 0) or as an outline of width w.
void aaRoundBox(float cx, float cy, float hw, float hh, float r, float w,
                uint16_t c, float alpha) {
  for (int y = floorf(cy - hh - w - 1); y <= ceilf(cy + hh + w + 1); y++) {
    for (int x = floorf(cx - hw - w - 1); x <= ceilf(cx + hw + w + 1); x++) {
      float sd = roundBoxDistance(x - cx, y - cy, hw, hh, r);
      float cov = w > 0 ? clamp01(w * 0.5f - fabsf(sd) + 0.5f)
                        : clamp01(0.5f - sd);
      blendPixel(x, y, c, cov * alpha);
    }
  }
}

// Soft light around a circle of radius r.
void aaGlow(float cx, float cy, float r, float strength, uint16_t c) {
  const float reach = 30;
  for (int y = floorf(cy - r - reach); y <= ceilf(cy + r + reach); y++) {
    for (int x = floorf(cx - r - reach); x <= ceilf(cx + r + reach); x++) {
      float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r;
      if (d < 0 || d > reach) continue;
      blendPixel(x, y, c, strength * expf(-d / 8.0f));
    }
  }
}

// ---- Smooth text ----------------------------------------------------

int textWidth(const VGFont &f, const String &s) {
  int w = 0;
  for (unsigned int i = 0; i < s.length(); i++) {
    char ch = s[i];
    if (ch < 32 || ch > 126) ch = '?';
    w += f.glyphs[ch - 32].advance;
  }
  return w;
}

// Draws text with its baseline at y.
void drawTextAA(const VGFont &f, int x, int y, const String &s,
                uint16_t c, float alpha) {
  for (unsigned int i = 0; i < s.length(); i++) {
    char ch = s[i];
    if (ch < 32 || ch > 126) ch = '?';
    const VGGlyph &g = f.glyphs[ch - 32];

    for (int row = 0; row < g.height; row++) {
      for (int col = 0; col < g.width; col++) {
        int idx = row * g.width + col;
        uint8_t b = f.bitmap[g.offset + idx / 2];
        uint8_t shade = (idx & 1) ? (b & 15) : (b >> 4);
        if (shade) {
          blendPixel(x + g.xOffset + col, y + g.yOffset + row, c,
                     shade / 15.0f * alpha);
        }
      }
    }
    x += g.advance;
  }
}

void drawTextCentered(const VGFont &f, int cx, int y, const String &s,
                      uint16_t c, float alpha) {
  drawTextAA(f, cx - textWidth(f, s) / 2, y, s, c, alpha);
}

// Wraps text at whole words within maxWidth. Returns the next baseline.
int drawWrappedAA(const VGFont &f, const String &message, int x, int y,
                  int maxWidth, int lastBaseline, int lineGap, uint16_t c,
                  bool centred) {
  String line;
  unsigned int i = 0;

  while (i <= message.length() && y <= lastBaseline) {
    int next = message.indexOf(' ', i);
    if (next < 0) next = message.length();
    String word = message.substring(i, next);
    String trial = line.length() ? line + " " + word : word;

    if (line.length() && textWidth(f, trial) > maxWidth) {
      if (centred) drawTextCentered(f, x + maxWidth / 2, y, line, c);
      else drawTextAA(f, x, y, line, c);
      y += lineGap;
      line = word;
    } else {
      line = trial;
    }
    i = next + 1;
  }

  if (line.length() && y <= lastBaseline) {
    if (centred) drawTextCentered(f, x + maxWidth / 2, y, line, c);
    else drawTextAA(f, x, y, line, c);
    y += lineGap;
  }
  return y;
}

// ---- Top of the screen: clock, Wi-Fi, battery and logo ----------

constexpr int STATUS_H = 48;

void renderStatusBar() {
  // Clock (shows --:-- until the time is known).
  String clock = "--:--";
  struct tm now;

  if (time(nullptr) > 1700000000 && getLocalTime(&now, 0)) {
    char text[8];
    strftime(text, sizeof(text), "%I:%M", &now);
    clock = text[0] == '0' ? String(text + 1) : String(text);
  }
  drawTextAA(FONT_SMALL, 70, 36, clock, WHITE);

  // Wi-Fi symbol, faded when disconnected.
  float on = WiFi.status() == WL_CONNECTED ? 1.0f : 0.35f;
  aaDisc(300, 35, 2.2f, WHITE, on);
  for (int r = 6; r <= 14; r += 4) {
    aaArc(300, 35, r, 2.2f, 228, 312, WHITE, on);
  }

  // Battery symbol.
  aaRoundBox(330, 29, 12, 6.5f, 3, 1.8f, WHITE);
  aaRoundBox(345, 29, 1.2f, 2.5f, 1, 0, WHITE);

  if (powerReady && power.isBatteryConnect()) {
    int percent = power.getBatteryPercent();
    if (percent >= 0 && percent <= 100) {
      float fill = 9.5f * percent / 100.0f;
      if (fill > 0.5f) {
        aaRoundBox(320.5f + fill, 29, fill, 4, 1.5f, 0,
                   percent < 20 ? MAD_COLOUR : WHITE);
      }
    }
  }
}

void renderLogo() {
  // Heartbeat line in purple to pink, then the name.
  const float xs[] = {0, 10, 15, 22, 29, 34, 44};
  const float ys[] = {0, 0, -12, 12, -6, 0, 0};
  int titleW = textWidth(FONT_TITLE, "VibeGuard");
  int startX = SCREEN_W / 2 - (44 + 10 + titleW) / 2;
  int baseY = 84;

  for (int i = 0; i < 6; i++) {
    uint16_t c = mixColour(LOGO_PURPLE, LOGO_PINK, i * 32 / 5);
    aaLine(startX + xs[i], baseY - 12 + ys[i],
           startX + xs[i + 1], baseY - 12 + ys[i + 1], 3.5f, c);
  }

  drawTextAA(FONT_TITLE, startX + 54, baseY, "VibeGuard", WHITE);
}

void renderHeader() {
  renderStatusBar();
  renderLogo();
}

// ---- The face ---------------------------------------------------

enum FaceState { FACE_IDLE, FACE_LISTENING, FACE_THINKING, FACE_SPEAKING };

constexpr int FACE_BOX = 92;     // half-size of the area the face uses

void renderFace(int state, float glow) {
  uint16_t a = moodAccent();
  uint16_t dark = scaleColour(a, 0.10f);

  aaGlow(FACE_X, FACE_Y, FACE_R + 1, glow, a);
  aaDisc(FACE_X, FACE_Y, FACE_R, dark);
  aaRing(FACE_X, FACE_Y, FACE_R - 2.5f, 5.5f, a);

  float ey = FACE_Y - 12;

  // Eyes: happy ones are curved, the others are round.
  if (currentMood == "happy") {
    aaArc(FACE_X - 22, ey + 5, 10, 4.5f, 200, 340, a);
    aaArc(FACE_X + 22, ey + 5, 10, 4.5f, 200, 340, a);
  } else {
    aaDisc(FACE_X - 22, ey, 6.5f, a);
    aaDisc(FACE_X + 22, ey, 6.5f, a);
  }

  // Eyebrows: worried for sad, angry for mad.
  if (currentMood == "sad") {
    aaLine(FACE_X - 36, ey - 13, FACE_X - 14, ey - 21, 4, a);
    aaLine(FACE_X + 14, ey - 21, FACE_X + 36, ey - 13, 4, a);
  } else if (currentMood == "mad") {
    aaLine(FACE_X - 36, ey - 22, FACE_X - 12, ey - 13, 4.5f, a);
    aaLine(FACE_X + 12, ey - 13, FACE_X + 36, ey - 22, 4.5f, a);
  }

  // Mouth. While speaking it is open.
  float my = FACE_Y + 20;

  if (state == FACE_SPEAKING) {
    if (currentMood == "happy") {
      // Open smile: the lower half of a circle.
      float cy = my - 6, r = 17;
      for (int y = cy; y <= cy + r + 1; y++) {
        for (int x = FACE_X - r - 1; x <= FACE_X + r + 1; x++) {
          float d = sqrtf((x - FACE_X) * (x - FACE_X) + (y - cy) * (y - cy));
          blendPixel(x, y, a, min(clamp01(r - d + 0.5f),
                                  clamp01(y - cy + 0.5f)));
        }
      }
      aaDisc(FACE_X, my + 6, 6, scaleColour(a, 0.45f));
    } else {
      aaRoundBox(FACE_X, my + 3, 12, 9, 8, 0, a);
      aaRoundBox(FACE_X, my + 5, 6, 4, 3.5f, 0, scaleColour(a, 0.35f));
    }
  } else if (currentMood == "happy") {
    aaArc(FACE_X, my - 12, 24, 5, 25, 155, a);
  } else if (currentMood == "neutral") {
    aaLine(FACE_X - 18, my, FACE_X + 18, my, 5, a);
  } else if (currentMood == "sad") {
    aaArc(FACE_X, my + 22, 22, 5, 210, 330, a);
  } else {
    aaArc(FACE_X, my + 24, 22, 5, 215, 325, a);
  }
}

// The "(( ))" sound waves beside the face.
void renderSoundArcs(bool left, bool right) {
  uint16_t a = moodAccent();

  for (int k = 0; k < 2; k++) {
    float r = FACE_R + 18 + k * 13;
    float alpha = k ? 0.5f : 1.0f;
    if (right) aaArc(FACE_X, FACE_Y, r, 4.5f, -32, 32, a, alpha);
    if (left) aaArc(FACE_X, FACE_Y, r, 4.5f, 148, 212, a, alpha);
  }
}

// ---- The glowing wave at the bottom ------------------------------

void renderWave(int x0, int x1) {
  uint16_t a = moodAccent();

  for (int x = max(0, x0); x <= min(SCREEN_W - 1, x1); x++) {
    float wy = 446 + 9 * sinf(x * 0.022f + wavePhase)
                   + 5 * sinf(x * 0.051f - wavePhase * 1.3f);

    for (int y = WAVE_TOP; y < SCREEN_H; y++) {
      float t = y - wy;
      float line = clamp01(1.6f - fabsf(t));
      float body = t > 0 ? 0.45f * expf(-t / 18.0f) + 0.10f : 0;
      float halo = t < 0 ? 0.35f * expf(t / 4.0f) : 0;
      float v = max(line, max(body, halo));

      uint16_t c = scaleColour(a, v);
      if (line > 0) c = mixColour(c, WHITE, (uint32_t)(line * 10));
      fb[y * SCREEN_W + x] = c;
    }
  }
}

void renderMicIcon() {
  aaDisc(MIC_X, MIC_Y, 18, BLACK, 0.55f);
  aaRing(MIC_X, MIC_Y, 18, 1.8f, WHITE, 0.9f);
  aaLine(MIC_X, MIC_Y - 9, MIC_X, MIC_Y - 2, 7, WHITE);
  aaArc(MIC_X, MIC_Y - 4, 8.5f, 1.8f, 15, 165, WHITE);
  aaLine(MIC_X, MIC_Y + 5, MIC_X, MIC_Y + 9, 1.8f, WHITE);
}

// Round button, bottom right: a chat bubble opens the status page,
// "<" goes back.
void renderButton() {
  aaDisc(BUTTON_X, BUTTON_Y, 18, BLACK, 0.55f);
  aaRing(BUTTON_X, BUTTON_Y, 18, 1.8f, WHITE, 0.9f);

  if (statusPage) {
    aaLine(BUTTON_X + 3, BUTTON_Y - 7, BUTTON_X - 4, BUTTON_Y, 2.5f, WHITE);
    aaLine(BUTTON_X - 4, BUTTON_Y, BUTTON_X + 3, BUTTON_Y + 7, 2.5f, WHITE);
  } else {
    aaRoundBox(BUTTON_X, BUTTON_Y - 2, 9, 6.5f, 4, 1.8f, WHITE);
    aaLine(BUTTON_X - 4, BUTTON_Y + 4.5f, BUTTON_X - 7, BUTTON_Y + 9,
           1.8f, WHITE);
  }
}

// Sound bars in the middle of the wave. level: 0 (quiet) to 1000 (loud).
void drawBars(int level) {
  const int count = 17, gap = 10, x0 = 125, cy = 462;
  uint16_t a = moodAccent();

  renderWave(x0 - 8, x0 + count * gap);
  level = constrain(level, 0, 1000);

  for (int i = 0; i < count; i++) {
    float shape = 1.0f - fabsf(i - (count - 1) / 2.0f) / (count / 2.0f);
    float wobble = 0.6f + random(40) / 100.0f;
    float h = 3 + shape * level / 1000.0f * 40 * wobble;
    uint16_t c = i % 2 ? mixColour(a, WHITE, 12) : a;
    aaLine(x0 + i * gap, cy - h / 2, x0 + i * gap, cy + h / 2, 4, c);
  }

  flushArea(x0 - 8, WAVE_TOP, count * gap + 16, SCREEN_H - WAVE_TOP);
}

// ---- Whole screens -------------------------------------------------

// The face screen used for idle, listening, thinking and speaking.
void showFaceScreen(
  int state, const String &label, const String &caption
) {
  homeVisible = false;
  statusPage = false;

  clearArea(0, 0, SCREEN_W, SCREEN_H);
  renderHeader();
  renderFace(state, 0.45f);

  if (state == FACE_LISTENING) renderSoundArcs(true, true);
  if (state == FACE_SPEAKING) renderSoundArcs(false, true);

  int ly = FACE_Y + FACE_R + 58;

  if (state == FACE_THINKING) {
    for (int i = -1; i <= 1; i++) {
      aaDisc(FACE_X + i * 15, ly - 34, 4.5f, moodAccent());
    }
  }

  drawTextCentered(FONT_LABEL, SCREEN_W / 2, ly, label,
                   state == FACE_IDLE ? moodAccent() : WHITE);

  if (caption.length()) {
    drawWrappedAA(FONT_TEXT, caption, 40, ly + 36, SCREEN_W - 80,
                  WAVE_TOP - 6, 26, mixColour(BLACK, WHITE, 24), true);
  }

  renderWave(0, SCREEN_W - 1);
  renderMicIcon();
  renderButton();
  flushAll();
}

void showHome() {
  showFaceScreen(FACE_IDLE, moodLabel(), "Hold the button to talk");
  homeVisible = true;
  waveTime = millis();
  pulseTime = millis();
}

void showListening() {
  showFaceScreen(
    FACE_LISTENING, "Listening...", "Release the button to send."
  );
}

void showThinking() {
  showFaceScreen(FACE_THINKING, "Thinking...", "");
}

void showSpeaking(const String &text) {
  showFaceScreen(FACE_SPEAKING, "Speaking...", text);
}

// Home screen animation: moving wave and a slowly breathing glow.
void animateHome() {
  unsigned long now = millis();

  if (now - waveTime >= WAVE_FRAME_MS) {
    wavePhase += 0.12f;
    renderWave(0, SCREEN_W - 1);
    renderMicIcon();
    renderButton();
    flushArea(0, WAVE_TOP, SCREEN_W, SCREEN_H - WAVE_TOP);
    waveTime = now;
  }

  if (now - pulseTime >= PULSE_MS) {
    float glow = 0.42f + 0.18f * sinf(now / 900.0f);
    clearArea(FACE_X - FACE_BOX, FACE_Y - FACE_BOX, FACE_BOX * 2,
              FACE_BOX * 2);
    renderFace(FACE_IDLE, glow);
    flushArea(FACE_X - FACE_BOX, FACE_Y - FACE_BOX, FACE_BOX * 2,
              FACE_BOX * 2);
    pulseTime = now;
  }

  if (now - statusBarTime >= 20000) {
    clearArea(0, 0, SCREEN_W, STATUS_H);
    renderStatusBar();
    flushArea(0, 0, SCREEN_W, STATUS_H);
    statusBarTime = now;
  }
}

// Text screens: questions, answers, errors.
void showMessage(
  const String &heading,
  const String &message
) {
  homeVisible = false;
  statusPage = false;

  clearArea(0, 0, SCREEN_W, SCREEN_H);
  renderHeader();

  uint16_t a = moodAccent();
  aaRoundBox(40, 136, 2, 15, 2, 0, a);
  drawTextAA(FONT_LABEL, 54, 148, heading, a);

  drawWrappedAA(FONT_TEXT, message, 40, 196, SCREEN_W - 80, 430, 28, WHITE);
  flushAll();
}

// Shown on screens the user can leave by tapping.
void showExitHint() {
  drawTextCentered(FONT_SMALL, SCREEN_W / 2, 470, "Tap the screen for home",
                   DIM);
  flushArea(0, 450, SCREEN_W, 30);
}

// The status page: battery, Wi-Fi and the watch's address.
void updateStatus() {
  clearArea(0, 0, SCREEN_W, SCREEN_H);
  renderHeader();

  drawTextAA(FONT_LABEL, 40, 150, "Status", NEUTRAL_COLOUR);

  String battery = "Battery: unavailable";
  if (powerReady && power.isBatteryConnect()) {
    int percent = power.getBatteryPercent();
    if (percent >= 0 && percent <= 100) {
      battery = "Battery: " + String(percent) + "%";
      if (power.isCharging()) battery += " (charging)";
    }
  } else if (powerReady) {
    battery = "Battery: none (USB power)";
  }

  drawTextAA(FONT_TEXT, 40, 200, battery, WHITE);
  drawTextAA(FONT_TEXT, 40, 236,
             WiFi.status() == WL_CONNECTED ? "Wi-Fi: connected"
                                           : "Wi-Fi: not connected",
             WHITE);

  if (WiFi.status() == WL_CONNECTED) {
    drawTextAA(FONT_TEXT, 40, 272,
               "Signal: " + String(WiFi.RSSI()) + " dBm", WHITE);
    // The address the laptop sends the mood to.
    drawTextAA(FONT_TEXT, 40, 308,
               "Address: " + WiFi.localIP().toString(), WHITE);
  }

  drawTextAA(FONT_TEXT, 40, 344, "Mood: " + moodLabel(), moodAccent());

  renderButton();
  flushAll();
  statusTime = millis();
}

void showStatusPage() {
  homeVisible = false;
  statusPage = true;
  updateStatus();
}

void goHome() {
  awaitingAnswer = false;
  readingUntil = 0;
  showHome();
}

// Show the time in Montreal (Eastern time, with daylight saving).
void setLocalTimezone() {
  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
  tzset();
}

bool fail(const String &reason) {
  lastError = reason;
  return false;
}

void showError() {
  digitalWrite(AMP_PIN, LOW);

  Serial.println(lastError);

  showMessage(
    "Could not finish",
    lastError + " Hold button to retry."
  );
  showExitHint();

  // Errors do not trap the user: tap, or wait, to go home.
  readingUntil = millis() + ERROR_SHOW_MS;
}
// AUDIO DRIVER

bool devicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool startAudio(bool recording) {
  digitalWrite(AMP_PIN, LOW);

  if (audioStarted) {
    audio.end();
    audioStarted = false;
  }

  // BCLK, LRCLK, speaker data, microphone data, MCLK
  audio.setPins(41, 45, 40, 42, 16);
  audio.setTimeout(300);

  uint32_t sampleRate = recording ? 16000 : 24000;

  if (!audio.begin(
        I2S_MODE_STD,
        sampleRate,
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO,
        I2S_STD_SLOT_BOTH
      )) {
    return fail("I2S initialization failed.");
  }

  audioStarted = true;

  audio_driver::codec_config_t config = {};

  config.input_device = audio_driver::ADC_INPUT_ALL;
  config.output_device = audio_driver::DAC_OUTPUT_ALL;

  config.i2s.mode = audio_driver::MODE_SLAVE;
  config.i2s.fmt = audio_driver::I2S_NORMAL;

  config.i2s.rate = recording
    ? audio_driver::RATE_16K
    : audio_driver::RATE_24K;

  config.i2s.bits = audio_driver::BIT_LENGTH_16BITS;
  config.i2s.channels = audio_driver::CHANNELS2;
  config.i2s.signal_type = audio_driver::SIGNAL_DIGITAL;

  if (recording) {
    if (!devicePresent(0x40)) {
      return fail("Microphone chip not found.");
    }

    microphone.setWire(&Wire);
    microphone.setAddress(0x40);

    if (
      microphone.init(&config) != RESULT_OK ||
      microphone.configI2S(
        audio_driver::CODEC_MODE_ENCODE,
        &config.i2s
      ) != RESULT_OK ||
      microphone.ctrlStateActive(
        audio_driver::CODEC_MODE_ENCODE, true
      ) != RESULT_OK ||
      microphone.setGain(
        audio_driver::ES7210_GAIN_24DB
      ) != RESULT_OK
    ) {
      return fail("Microphone initialization failed.");
    }
  } else {
    if (!devicePresent(0x18)) {
      return fail("Speaker chip not found.");
    }

    speaker.setWire(&Wire);
    speaker.setAddress(0x18);

    if (
      speaker.init(&config) != RESULT_OK ||
      speaker.configI2S(
        audio_driver::CODEC_MODE_DECODE,
        &config.i2s
      ) != RESULT_OK ||
      speaker.ctrlStateActive(
        audio_driver::CODEC_MODE_DECODE, true
      ) != RESULT_OK ||
      speaker.setVoiceVolume(45) != RESULT_OK
    ) {
      return fail("Speaker initialization failed.");
    }
  }

  return true;
}

void stopAudio() {
  if (audioStarted) {
    audio.end();
    audioStarted = false;
  }

  digitalWrite(AMP_PIN, LOW);
}
// MOOD FROM THE LAPTO

// GET /mood?m=happy|sad|mad|neutral
void handleMood() {
  String mood = server.arg("m");

  if (
    mood != "happy" && mood != "sad" &&
    mood != "mad" && mood != "neutral"
  ) {
    server.send(
      400, "text/plain",
      "use /mood?m=happy|sad|mad|neutral"
    );
    return;
  }

  // The laptop repeats the mood every few seconds;
  // only a real change counts.
  if (mood != currentMood) {
    currentMood = mood;
    moodSince = millis();
    checkInHandled = false;

    Serial.println("mood -> " + mood);

    if (homeVisible) showHome();
  }

  server.send(200, "text/plain", mood);
}

void handleRoot() {
  server.send(
    200, "text/plain",
    "VibeGuard watch. Current mood: " + currentMood
  );
}

void startMoodServer() {
  if (serverStarted) return;

  MDNS.begin(WATCH_NAME);

  server.on("/", handleRoot);
  server.on("/mood", handleMood);
  server.begin();

  serverStarted = true;

  Serial.println(
    "Watch address: http://" + WiFi.localIP().toString() +
    "  or  http://" + String(WATCH_NAME) + ".local"
  );
}

// Short, clean jokes for cheering up a sad user. A different one is
// used each time; add or change lines freely.
const char *JOKES[] = {
  "Why did the scarecrow win an award? Because he was outstanding "
  "in his field.",
  "What do you call a fake noodle? An impasta.",
  "Why don't eggs tell jokes? They would crack each other up.",
  "What do you call a sleeping dinosaur? A dino-snore.",
  "Why did the math book look sad? It had too many problems.",
  "What do you call cheese that isn't yours? Nacho cheese.",
  "Why can't you trust stairs? They're always up to something.",
  "What did the ocean say to the beach? Nothing, it just waved.",
};
constexpr int JOKE_COUNT = sizeof(JOKES) / sizeof(JOKES[0]);
int nextJokeIndex = 0;

String nextJoke() {
  String joke = JOKES[nextJokeIndex];
  nextJokeIndex = (nextJokeIndex + 1) % JOKE_COUNT;
  return joke;
}

// What the watch says for each mood. {JOKE} is replaced by a joke
// when the check-in actually happens.
String checkInQuestion() {
  if (currentMood == "sad") {
    return "You seem down, so here's a joke: {JOKE} "
           "Want to talk about it?";
  }

  if (currentMood == "mad") {
    return "Let's calm down together. Follow the light and "
           "breathe with it: in as it gets brighter, out as "
           "it fades.";
  }

  if (currentMood == "happy") {
    return "I'm glad you're happy! If you want to share "
           "the good news, I'm here.";
  }

  return "";   // neutral: nothing to ask
}

void runCheckIn(const String &message) {
  String question = message;
  question.replace("{JOKE}", nextJoke());

  lastQuestion = question;

  if (SPEAK_CHECK_IN) {
    // If speaking fails, the question is still shown on screen.
    if (!speakAnswer(question)) {
      Serial.println("Check-in speech failed: " + lastError);
    }

    stopAudio();
  }

  showMessage(
    "Checking in",
    question + " Hold the button to answer."
  );
  showExitHint();

  awaitingAnswer = true;
  askedAt = millis();
  readingUntil = 0;
}
// WI-FI AND SECURE API CONNECTIO

// Sets the clock from the "Date" line every web server sends back.
// Works on networks that block the usual time service (NTP).
bool syncClockFromWeb() {
  NetworkClient plain;
  HTTPClient http;

  http.setConnectTimeout(8000);
  http.setTimeout(8000);

  if (!http.begin(plain, "http://www.google.com/generate_204")) {
    return false;
  }

  const char *wanted[] = {"Date"};
  http.collectHeaders(wanted, 1);

  int code = http.GET();
  String date = http.header("Date");
  http.end();

  Serial.println(
    "Web time: HTTP " + String(code) + " Date: " + date
  );

  if (code <= 0 || !date.length()) {
    return false;
  }

  // Example: "Fri, 13 Nov 2026 15:04:05 GMT"
  struct tm parts = {};

  if (
    !strptime(
      date.c_str(), "%a, %d %b %Y %H:%M:%S", &parts
    )
  ) {
    return false;
  }

  // The header is in UTC, so read it as UTC.
  setenv("TZ", "UTC0", 1);
  tzset();

  time_t now = mktime(&parts);

  if (now < 1700000000) {
    return false;
  }

  struct timeval value = {};
  value.tv_sec = now;

  settimeofday(&value, nullptr);

  return true;
}

bool connectNetwork() {
  if (
    !strlen(WIFI_SSID) ||
    !strlen(GEMINI_API_KEY) ||
    String(WIFI_SSID).startsWith("YOUR_") ||
    String(GEMINI_API_KEY).startsWith("YOUR_")
  ) {
    return fail("Enter Wi-Fi and API key in the sketch.");
  }

  if (WiFi.status() != WL_CONNECTED) {
    showMessage("Connecting...", "Connecting to Wi-Fi.");

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long started = millis();

    while (
      WiFi.status() != WL_CONNECTED &&
      millis() - started < 20000
    ) {
      delay(50);
    }

    if (WiFi.status() != WL_CONNECTED) {
      return fail("Wi-Fi failed. Check network and password.");
    }
  }

  startMoodServer();

  // TLS certificate verification needs the correct time.
  if (time(nullptr) < 1700000000) {
    // Say what is happening: this wait can take up to 15 seconds,
    // and fails if the network has no internet.
    showMessage(
      "Connecting...",
      "Checking the internet connection."
    );

    configTime(
      0, 0,
      "time.google.com",
      "pool.ntp.org"
    );

    unsigned long started = millis();

    while (
      time(nullptr) < 1700000000 &&
      millis() - started < 6000
    ) {
      delay(50);
    }

    // Phone hotspots often block the normal time service.
    // If so, read the time from an ordinary web request instead.
    if (time(nullptr) < 1700000000 && !syncClockFromWeb()) {
      return fail(
        "No internet on this Wi-Fi. Check the hotspot has "
        "mobile data."
      );
    }
  }

  setLocalTimezone();
  return true;
}

bool apiCall(
  const char *model,
  JsonDocument &request,
  ResponseBuffer &response
) {
  if (request.overflowed() || !response.data) {
    return fail("Not enough PSRAM for API request.");
  }

  size_t bodyLength = measureJson(request);
  char *body = (char *)ps_malloc(bodyLength + 1);

  if (!body) {
    return fail("Not enough PSRAM for request body.");
  }

  if (
    serializeJson(request, body, bodyLength + 1)
      != bodyLength
  ) {
    free(body);
    return fail("Could not prepare API request.");
  }

  NetworkClientSecure tls;
  tls.useBuiltinCACertBundle();

  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(60000);

  String url =
    String(
      "https://generativelanguage.googleapis.com/"
      "v1beta/models/"
    ) + model + ":generateContent";

  if (!http.begin(tls, url)) {
    free(body);
    return fail("Could not start HTTPS.");
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", GEMINI_API_KEY);

  int code = http.POST(
    (uint8_t *)body, bodyLength
  );

  free(body);

  if (code < 0) {
    String reason = HTTPClient::errorToString(code);
    http.end();
    return fail("Network/TLS: " + reason);
  }

  int received = http.writeToStream(&response);
  http.end();

  if (response.overflow || received < 0) {
    return fail("API response too large or interrupted.");
  }

  if (code != 200) {
    if (code == 429) {
      return fail("API quota reached. Try again later.");
    }

    return fail(
      "API HTTP " + String(code) +
      ". Check key, model and project access."
    );
  }

  return true;
}
// WAV RECORDIN

void put16(uint8_t *p, uint16_t value) {
  p[0] = value;
  p[1] = value >> 8;
}

void put32(uint8_t *p, uint32_t value) {
  for (int i = 0; i < 4; i++) {
    p[i] = value >> (8 * i);
  }
}

void makeWavHeader(uint8_t *p, uint32_t audioBytes) {
  memcpy(p, "RIFF", 4);
  put32(p + 4, audioBytes + 36);

  memcpy(p + 8, "WAVEfmt ", 8);
  put32(p + 16, 16);

  put16(p + 20, 1);       // PCM
  put16(p + 22, 1);       // Mono
  put32(p + 24, 16000);   // Sample rate
  put32(p + 28, 32000);   // Bytes per second
  put16(p + 32, 2);
  put16(p + 34, 16);      // Bits per sample

  memcpy(p + 36, "data", 4);
  put32(p + 40, audioBytes);
}

bool recordQuestion(uint8_t *wav, size_t &wavLength) {
  if (!startAudio(true)) {
    return false;
  }

  int16_t stereo[512];

  // Discard the initial audio transients.
  for (int i = 0; i < 6; i++) {
    if (
      audio.readBytes(
        (char *)stereo, sizeof(stereo)
      ) != sizeof(stereo)
    ) {
      return fail("Microphone read failed.");
    }
  }

  showListening();

  size_t sampleCount = 0;
  int peak = 0;

  unsigned long started = millis();
  unsigned long meterTime = 0;

  while (
    digitalRead(WATCH_TALK_PIN) == LOW &&
    sampleCount < MAX_SAMPLES &&
    millis() - started < 8500
  ) {
    size_t received = audio.readBytes(
      (char *)stereo, sizeof(stereo)
    );

    if (!received || received % 4) {
      return fail("Microphone audio read failed.");
    }

    int blockPeak = 0;

    for (
      size_t i = 0;
      i < received / 4 && sampleCount < MAX_SAMPLES;
      i++
    ) {
      int16_t sample = stereo[i * 2 + MIC_CHANNEL];

      put16(
        wav + 44 + sampleCount * 2,
        (uint16_t)sample
      );

      sampleCount++;

      int level = abs((int)sample);
      if (level > peak) peak = level;
      if (level > blockPeak) blockPeak = level;
    }

    if (millis() - meterTime > 100) {
      drawBars(blockPeak / 12);
      meterTime = millis();
    }
  }

  microphone.ctrlStateActive(
    audio_driver::CODEC_MODE_ENCODE, false
  );

  if (sampleCount < 4000) {
    return fail("Too short. Hold the button while speaking.");
  }

  if (peak < 20) {
    return fail("No microphone signal detected.");
  }

  makeWavHeader(wav, sampleCount * 2);
  wavLength = 44 + sampleCount * 2;

  return true;
}
// SEND RECORDING TO GEMIN

bool askAI(
  uint8_t *wav,
  size_t wavLength,
  String &answer
) {
  showThinking();

  size_t encodedCapacity =
    4 * ((wavLength + 2) / 3) + 1;

  size_t encodedLength = 0;

  unsigned char *encoded =
    (unsigned char *)ps_malloc(encodedCapacity);

  if (!encoded) {
    return fail("Not enough PSRAM to encode audio.");
  }

  int result = mbedtls_base64_encode(
    encoded,
    encodedCapacity,
    &encodedLength,
    wav,
    wavLength
  );

  if (result != 0) {
    free(encoded);
    return fail("Audio encoding failed.");
  }

  encoded[encodedLength] = '\0';

  // The instructions now include the mood the camera sees, and the
  // question the watch just asked (if any), so the reply fits.
  String prompt =
    "You are VibeGuard, Pina's helpful watch assistant. "
    "Listen to the recording and answer in English in "
    "one or two short sentences, at most 35 words. "
    "Use plain text without markdown. "
    "If speech is unclear, ask the user to repeat. "
    "You cannot control devices. "
    "A camera currently reads the user's facial "
    "expression as " + currentMood + ". ";

  if (awaitingAnswer) {
    prompt +=
      "You just asked them: \"" + lastQuestion + "\" "
      "The recording is their reply. ";

    if (currentMood == "happy") {
      prompt +=
        "They are sharing good news: celebrate it with them "
        "warmly and ask one short follow-up question.";
    } else {
      prompt +=
        "Respond with warmth and offer one small, practical "
        "suggestion.";
    }
  }

  JsonDocument request(&jsonMemory);

  request["contents"][0]["role"] = "user";

  request["contents"][0]["parts"][0]["text"] = prompt;

  request["contents"][0]["parts"][1]
         ["inlineData"]["mimeType"] = "audio/wav";

  request["contents"][0]["parts"][1]
         ["inlineData"]["data"] = (const char *)encoded;

  request["generationConfig"]["maxOutputTokens"] = 1024;

  ResponseBuffer response(64 * 1024);

  bool ok = apiCall(
    ANSWER_MODEL, request, response
  );

  request.clear();
  free(encoded);

  if (!ok) return false;

  JsonDocument reply(&jsonMemory);

  if (
    deserializeJson(
      reply, response.data, response.length
    )
  ) {
    return fail("Invalid answer response.");
  }

  JsonArray parts =
    reply["candidates"][0]["content"]["parts"]
      .as<JsonArray>();

  for (JsonObject part : parts) {
    bool thought = part["thought"] | false;

    if (!thought && part["text"].is<const char *>()) {
      answer += part["text"].as<const char *>();
    }
  }

  answer.trim();

  if (!answer.length()) {
    return fail("No answer returned. Try another question.");
  }

  if (answer.length() > 300) {
    answer = answer.substring(0, 297) + "...";
  }

  return true;
}
// GENERATE AND PLAY THE SPOKEN ANSWE

bool speakAnswer(const String &answer) {
  showThinking();

  JsonDocument request(&jsonMemory);

  request["contents"][0]["parts"][0]["text"] =
    "Read aloud naturally: " + answer;

  request["generationConfig"]
         ["responseModalities"][0] = "AUDIO";

  request["generationConfig"]["speechConfig"]
         ["voiceConfig"]["prebuiltVoiceConfig"]
         ["voiceName"] = "Kore";

  ResponseBuffer response(2 * 1024 * 1024);

  if (!apiCall(VOICE_MODEL, request, response)) {
    return false;
  }

  JsonDocument reply(&jsonMemory);

  if (
    deserializeJson(
      reply, response.data, response.length
    )
  ) {
    return fail("Invalid speech response.");
  }

  const char *encoded = nullptr;
  String mimeType;

  JsonArray parts =
    reply["candidates"][0]["content"]["parts"]
      .as<JsonArray>();

  for (JsonObject part : parts) {
    if (part["inlineData"]["data"].is<const char *>()) {
      encoded =
        part["inlineData"]["data"].as<const char *>();

      mimeType =
        part["inlineData"]["mimeType"] | "";

      break;
    }
  }

  if (!encoded) {
    return fail("No speech audio returned.");
  }

  // Expected: mono, 24 kHz, signed 16-bit little-endian PCM.
  if (
    !mimeType.startsWith("audio/L16") &&
    !mimeType.startsWith("audio/l16")
  ) {
    return fail("Unexpected speech audio format.");
  }

  if (mimeType.indexOf("rate=24000") < 0) {
    return fail("Unexpected speech sample rate.");
  }

  size_t capacity = strlen(encoded) / 4 * 3 + 3;
  size_t audioBytes = 0;

  uint8_t *pcm = (uint8_t *)ps_malloc(capacity);

  if (!pcm) {
    return fail("Not enough PSRAM for speech.");
  }

  int decoded = mbedtls_base64_decode(
    pcm,
    capacity,
    &audioBytes,
    (const unsigned char *)encoded,
    strlen(encoded)
  );

  if (decoded != 0 || !audioBytes || audioBytes % 2) {
    free(pcm);
    return fail("Speech decoding failed.");
  }

  if (!startAudio(false)) {
    free(pcm);
    return false;
  }

  showSpeaking(answer);
  digitalWrite(AMP_PIN, HIGH);

  int16_t stereo[512];
  bool ok = true;
  int speechPeak = 0;
  unsigned long barsTime = 0;

  for (size_t position = 0; position < audioBytes;) {
    size_t frames = min(
      (size_t)256,
      (audioBytes - position) / 2
    );

    for (size_t i = 0; i < frames; i++) {
      int16_t sample = (int16_t)(
        (uint16_t)pcm[position] |
        ((uint16_t)pcm[position + 1] << 8)
      );

      position += 2;

      // Duplicate mono to both I2S channels.
      // Reduce amplitude for a moderate initial volume.
      stereo[2 * i] = sample / 2;
      stereo[2 * i + 1] = sample / 2;

      int level = abs((int)sample);
      if (level > speechPeak) speechPeak = level;
    }

    // Move the sound bars with the voice.
    if (millis() - barsTime > 90) {
      drawBars(speechPeak / 12);
      speechPeak = 0;
      barsTime = millis();
    }

    size_t bytesToWrite = frames * 4;

    if (
      audio.write(
        (const uint8_t *)stereo, bytesToWrite
      ) != bytesToWrite
    ) {
      ok = false;
      break;
    }

    delay(1);
  }

  // Let the final audio samples leave the DMA buffer.
  memset(stereo, 0, sizeof(stereo));

  for (int i = 0; i < 12 && ok; i++) {
    audio.write(
      (const uint8_t *)stereo, sizeof(stereo)
    );
  }

  delay(100);
  digitalWrite(AMP_PIN, LOW);
  free(pcm);

  if (!ok) {
    return fail("Speaker audio write failed.");
  }

  return true;
}
// TOUCH, BATTERY AND WI-FI STATU

bool readTouch(int &x, int &y) {
  Wire.beginTransmission(0x38);
  Wire.write(0x02);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (
    Wire.requestFrom(
      (uint8_t)0x38, (uint8_t)5
    ) != 5
  ) {
    return false;
  }

  int fingers = Wire.read() & 15;
  int xHigh = Wire.read();
  int xLow = Wire.read();
  int yHigh = Wire.read();
  int yLow = Wire.read();

  x = ((xHigh & 15) << 8) | xLow;
  y = ((yHigh & 15) << 8) | yLow;

  return fingers > 0 && x < 410 && y < 502;
}

// STARTU

void setup() {
  Serial.begin(115200);

  pinMode(WATCH_TALK_PIN, INPUT_PULLUP);

  pinMode(AMP_PIN, OUTPUT);
  digitalWrite(AMP_PIN, LOW);

  if (!gfx->begin()) {
    while (true) delay(1000);
  }

  fb = gfx->getFramebuffer();
  flushBuf = (uint16_t *)ps_malloc(SCREEN_W * 200 * 2);

  if (!fb || !flushBuf) {
    Serial.println("Not enough PSRAM for the screen. Select OPI PSRAM in Tools.");
    while (true) delay(1000);
  }

  gfx->setTextWrap(false);

  Wire.begin(15, 14);
  Wire.setTimeOut(50);

  powerReady = power.begin(
    Wire, 0x34, 15, 14
  );

  if (powerReady) {
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
  }

  // Reset touch controller.
  pinMode(9, OUTPUT);
  digitalWrite(9, LOW);
  delay(10);
  digitalWrite(9, HIGH);
  delay(200);

  // Enable active touch polling.
  Wire.beginTransmission(0x38);
  Wire.write(0xA5);
  Wire.write(0x00);
  Wire.endTransmission();

  if (!psramFound()) {
    showMessage(
      "PSRAM required",
      "Select OPI PSRAM in Tools, then upload again."
    );

    while (true) delay(1000);
  }

  WiFi.mode(WIFI_STA);

  if (connectNetwork()) {
    showHome();
  } else {
    showError();
  }
}
// MAIN LOO

void loop() {
  // Receive mood updates from the laptop.
  if (serverStarted) {
    server.handleClient();
  }

  bool pressed = digitalRead(WATCH_TALK_PIN) == LOW;

  // Require a release before starting another question.
  if (!pressed) {
    if (!releaseTime) releaseTime = millis();

    if (millis() - releaseTime >= 80) {
      buttonArmed = true;
    }
  } else {
    releaseTime = 0;

    if (buttonArmed) {
      delay(30);

      if (digitalRead(WATCH_TALK_PIN) == LOW) {
        buttonArmed = false;
        homeVisible = false;
        statusPage = false;
        readingUntil = 0;

        if (!connectNetwork()) {
          awaitingAnswer = false;
          showError();
          return;
        }

        uint8_t *wav = (uint8_t *)ps_malloc(
          44 + MAX_SAMPLES * 2
        );

        if (!wav) {
          awaitingAnswer = false;
          fail("Not enough PSRAM to record.");
          showError();
          return;
        }

        size_t wavLength = 0;
        String answer;

        bool ok = recordQuestion(wav, wavLength);

        if (ok) {
          ok = askAI(wav, wavLength, answer);
        }

        free(wav);

        // The reply to a check-in has been handled.
        awaitingAnswer = false;

        if (!ok) {
          stopAudio();
          showError();
        } else if (SPEAK_ANSWERS && speakAnswer(answer)) {
          stopAudio();
          delay(600);
          showHome();
        } else {
          // Speech is off, or it failed (usually the free daily speech
          // limit). The answer is still good, so show it as text.
          stopAudio();
          Serial.println("Showing answer as text: " + lastError);
          showMessage("VibeGuard says", answer);
          showExitHint();
          readingUntil = millis() + READ_ANSWER_MS;
        }

        releaseTime = 0;
        return;
      }
    }
  }

  // Check in on a sad or mad mood:
  //  - first time: once the mood has lasted CHECK_IN_AFTER_MS
  //  - still sad or mad later: again every CHECK_IN_REPEAT_MS
  //  - never sooner than CHECK_IN_COOLDOWN_MS after the last question;
  //    if that blocks it, the watch waits and asks when allowed
  //    (it no longer gives up on that mood).
  // It also waits while a question or answer is on screen.
  String question = checkInQuestion();

  if (
    !pressed &&
    question.length() &&
    !awaitingAnswer &&
    readingUntil == 0
  ) {
    unsigned long now = millis();

    bool askedThisMood =
      lastCheckIn != 0 && lastCheckIn >= moodSince;

    // Happy gets one friendly message per happy spell, not repeats.
    bool due = askedThisMood
      ? currentMood != "happy" &&
        now - lastCheckIn >= CHECK_IN_REPEAT_MS
      : now - moodSince >= CHECK_IN_AFTER_MS;

    bool cooledDown =
      lastCheckIn == 0 ||
      now - lastCheckIn >= CHECK_IN_COOLDOWN_MS;

    if (due && cooledDown) {
      lastCheckIn = now;
      runCheckIn(question);
    }
  }

  // A text answer has been on screen long enough: go back home.
  if (readingUntil && millis() >= readingUntil) {
    readingUntil = 0;
    if (!awaitingAnswer) showHome();
  }

  // No reply to the question: go back to the home screen.
  if (
    awaitingAnswer &&
    millis() - askedAt >= ANSWER_WAIT_MS
  ) {
    awaitingAnswer = false;
    showHome();
  }

  // Touch works on every screen.
  int x = 0;
  int y = 0;

  bool touched = readTouch(x, y);

  if (touched && !previousTouch) {
    if (homeVisible || statusPage) {
      // Home and status: only the round button does something.
      int dx = x - BUTTON_X;
      int dy = y - BUTTON_Y;

      if (dx * dx + dy * dy <= 40 * 40) {
        if (statusPage) {
          showHome();
        } else {
          showStatusPage();
        }
      }
    } else {
      // Any other screen (answer, question, error):
      // a tap anywhere goes back home.
      goHome();
    }
  }

  previousTouch = touched;

  if (statusPage) {
    if (millis() - statusTime > 2000) {
      updateStatus();
    }
  } else if (homeVisible) {
    // Home screen animation: moving wave and pulsing glow.
    animateHome();
  }

  delay(15);
}