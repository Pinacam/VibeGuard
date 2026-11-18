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
  API KEY: [REMOVED: configure locally]
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

// Colours (16-bit). Each mood has a bright accent and a dark core.
constexpr uint16_t BLACK = 0x0000;
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t DIM = 0x0294;        // faint teal for guide lines
constexpr uint16_t CYAN = 0x07FF;       // neutral
constexpr uint16_t GOLD = 0xFE60;       // happy
constexpr uint16_t BLUE = 0x5D1F;       // sad
constexpr uint16_t RED = 0xF9A6;        // mad
constexpr uint16_t CORE_CYAN = 0x0127;
constexpr uint16_t CORE_GOLD = 0x3180;
constexpr uint16_t CORE_BLUE = 0x0869;
constexpr uint16_t CORE_RED = 0x3021;

// Home screen layout.
constexpr int HUD_X = 205;              // centre of the rings
constexpr int HUD_Y = 238;
constexpr int RING_INNER = 98;
constexpr int RING_OUTER = 107;
constexpr int RING_SEGMENT = 320;       // 80 degrees, in quarter degrees
constexpr int RING_STEP = 12;           // 3 degrees per frame
constexpr unsigned long RING_FRAME_MS = 90;   // lower = spins faster
constexpr unsigned long PULSE_MS = 800;
constexpr int BUTTON_X = 335;
constexpr int BUTTON_Y = 440;

// How long an error stays up before the watch goes back home.
constexpr unsigned long ERROR_SHOW_MS = 6000;
// MOOD CHECK-IN SETTING

// Name the laptop uses to find the watch: http://vibeguard-watch.local
const char *WATCH_NAME = "vibeguard-watch";

// The mood must last this long before the watch asks about it,
// so a passing expression does not trigger a question.
constexpr unsigned long CHECK_IN_AFTER_MS = 4000;

// Minimum time between two questions.
constexpr unsigned long CHECK_IN_COOLDOWN_MS = 60000;

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

Arduino_GFX *gfx = new Arduino_CO5300(
  bus, 8, 0, 410, 502, 22, 0, 0, 0
);

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
unsigned long ringTime = 0;
unsigned long pulseTime = 0;
int ringPos = 0;                 // ring rotation, in quarter degrees

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
// SCREEN DESIG

void drawText(
  int x, int y,
  const String &message,
  int size,
  uint16_t color
) {
  gfx->setCursor(x, y);
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->print(message);
}

// Text is 6 x 8 pixels per character, times the size.
void drawCentered(
  int centerX, int y,
  const String &message,
  int size,
  uint16_t color
) {
  drawText(
    centerX - (int)message.length() * 3 * size,
    y, message, size, color
  );
}

// The whole interface takes its accent colour from the mood.
uint16_t moodAccent() {
  if (currentMood == "happy") return GOLD;
  if (currentMood == "sad") return BLUE;
  if (currentMood == "mad") return RED;
  return CYAN;
}

uint16_t moodCore() {
  if (currentMood == "happy") return CORE_GOLD;
  if (currentMood == "sad") return CORE_BLUE;
  if (currentMood == "mad") return CORE_RED;
  return CORE_CYAN;
}

// Small header used on every screen.
void drawHeader() {
  uint16_t accent = moodAccent();

  drawCentered(205, 30, "VIBEGUARD", 3, accent);
  drawCentered(205, 62, "THERAPY AI", 2, DIM);

  gfx->drawFastHLine(40, 90, 110, DIM);
  gfx->drawFastHLine(260, 90, 110, DIM);
  gfx->fillRect(150, 89, 110, 3, accent);
}

// Round button, bottom right: "i" on home, "<" on the status page.
void drawCircleButton() {
  uint16_t accent = moodAccent();

  gfx->fillCircle(BUTTON_X, BUTTON_Y, 30, BLACK);
  gfx->drawCircle(BUTTON_X, BUTTON_Y, 30, accent);
  gfx->drawCircle(BUTTON_X, BUTTON_Y, 29, accent);
  gfx->drawCircle(BUTTON_X, BUTTON_Y, 24, DIM);

  drawCentered(
    BUTTON_X + 1, BUTTON_Y - 11,
    statusPage ? "<" : "i", 3, accent
  );
}

// ---- The rotating ring -------------------------------------
// Angles are counted in quarter degrees (1440 per turn) so the
// exact same lines can be drawn and later erased.

void ringLines(int from, int count, uint16_t color) {
  for (int i = 0; i < count; i++) {
    int unit = (from + i) % 1440;
    float angle = unit * (PI / 720.0f);
    float c = cosf(angle);
    float s = sinf(angle);

    gfx->drawLine(
      HUD_X + lroundf(RING_INNER * c),
      HUD_Y + lroundf(RING_INNER * s),
      HUD_X + lroundf(RING_OUTER * c),
      HUD_Y + lroundf(RING_OUTER * s),
      color
    );
  }
}

// Three bright segments with gaps between them.
void drawRing(uint16_t color) {
  for (int k = 0; k < 3; k++) {
    ringLines(ringPos + k * 480, RING_SEGMENT, color);
  }
}

// Turn the ring a little: erase the tail, extend the head.
void stepRing() {
  uint16_t accent = moodAccent();

  for (int k = 0; k < 3; k++) {
    ringLines(ringPos + k * 480, RING_STEP, BLACK);
    ringLines(
      ringPos + k * 480 + RING_SEGMENT, RING_STEP, accent
    );
  }

  ringPos = (ringPos + RING_STEP) % 1440;
}

void drawCoreOutline(bool bright) {
  uint16_t color = bright ? WHITE : moodAccent();

  gfx->drawCircle(HUD_X, HUD_Y, 66, color);
  gfx->drawCircle(HUD_X, HUD_Y, 67, color);
}

void drawCorner(int x, int y, int dx, int dy) {
  gfx->drawFastHLine(dx > 0 ? x : x - 26, y, 26, DIM);
  gfx->drawFastVLine(x, dy > 0 ? y : y - 26, 26, DIM);
}

void showHome() {
  statusPage = false;
  homeVisible = true;

  uint16_t accent = moodAccent();

  gfx->fillScreen(BLACK);
  drawHeader();

  // Frame corners around the display core.
  drawCorner(40, 108, 1, 1);
  drawCorner(370, 108, -1, 1);
  drawCorner(40, 368, 1, -1);
  drawCorner(370, 368, -1, -1);

  // Outer scale: a thin circle with tick marks.
  gfx->drawCircle(HUD_X, HUD_Y, 118, DIM);

  for (int tick = 0; tick < 24; tick++) {
    float angle = tick * (PI / 12.0f);
    int inner = (tick % 6 == 0) ? 120 : 123;

    gfx->drawLine(
      HUD_X + lroundf(inner * cosf(angle)),
      HUD_Y + lroundf(inner * sinf(angle)),
      HUD_X + lroundf(129 * cosf(angle)),
      HUD_Y + lroundf(129 * sinf(angle)),
      (tick % 6 == 0) ? accent : DIM
    );
  }

  // Rotating ring, then the inner guide circle.
  drawRing(accent);
  gfx->drawCircle(HUD_X, HUD_Y, 86, DIM);

  // Glowing core with the current mood in the middle.
  gfx->fillCircle(HUD_X, HUD_Y, 72, BLACK);
  gfx->fillCircle(HUD_X, HUD_Y, 65, moodCore());
  drawCoreOutline(false);

  String mood = currentMood;
  mood.toUpperCase();

  drawCentered(HUD_X, HUD_Y - 30, "MOOD", 2, DIM);
  drawCentered(
    HUD_X, HUD_Y - 6, mood,
    mood.length() > 5 ? 2 : 3, accent
  );

  drawCentered(205, 384, "HOLD BUTTON TO TALK", 2, WHITE);

  // Bottom left: who is wearing it, power and connection.
  drawText(40, 418, "PINA", 3, WHITE);

  String power_line = "PWR USB";

  if (powerReady && power.isBatteryConnect()) {
    int percent = power.getBatteryPercent();

    if (percent >= 0 && percent <= 100) {
      power_line = "PWR " + String(percent) + "%";
    }
  }

  power_line +=
    WiFi.status() == WL_CONNECTED ? "  LINK OK" : "  NO LINK";

  drawText(40, 452, power_line, 2, DIM);

  drawCircleButton();

  ringTime = millis();
  pulseTime = millis();
  pulseOn = false;
}

// Shows a heading and a block of text, wrapped at whole words.
void showMessage(
  const String &heading,
  const String &message
) {
  homeVisible = false;
  statusPage = false;

  gfx->fillScreen(BLACK);
  drawHeader();

  gfx->fillRect(30, 122, 4, 26, moodAccent());
  drawText(44, 124, heading, 3, moodAccent());

  const unsigned int perLine = 28;
  int y = 178;
  unsigned int start = 0;

  while (start < message.length() && y < 400) {
    unsigned int end = start + perLine;

    if (end >= message.length()) {
      end = message.length();
    } else {
      // Step back to the last space so words are not cut.
      int space = message.lastIndexOf(' ', end);

      if (space > (int)start) end = space;
    }

    String line = message.substring(start, end);
    line.trim();

    drawText(32, y, line, 2, WHITE);
    y += 24;

    start = end;

    while (
      start < message.length() && message[start] == ' '
    ) {
      start++;
    }
  }
}

// Shown on screens the user can leave by tapping.
void showExitHint() {
  drawCentered(205, 452, "TAP SCREEN FOR HOME", 2, DIM);
}

void goHome() {
  awaitingAnswer = false;
  readingUntil = 0;
  showHome();
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

String checkInQuestion() {
  if (currentMood == "sad") {
    return "You look sad. Why are you sad?";
  }

  if (currentMood == "mad") {
    return "You seem upset. What is bothering you?";
  }

  return "";   // happy and neutral: nothing to ask
}

void runCheckIn(const String &question) {
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

  showMessage(
    "AI mode activated",
    "Listening... Hold the button. Release to send. "
    "Maximum 8 seconds."
  );

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
      gfx->fillRect(35, 365, 340, 15, BLACK);
      gfx->fillRect(
        35, 365, min(340, blockPeak / 32), 15, CYAN
      );
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
  showMessage(
    "Thinking...",
    "Sending your recording to Gemini."
  );

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
      "The recording is their reply. Respond with warmth "
      "and offer one small, practical suggestion.";
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
  showMessage("Making speech...", answer);

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

  showMessage("Speaking...", answer);
  digitalWrite(AMP_PIN, HIGH);

  int16_t stereo[512];
  bool ok = true;

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

void updateStatus() {
  gfx->fillRect(25, 125, 360, 260, BLACK);

  drawText(30, 135, "BATTERY", 2, CYAN);

  if (!powerReady) {
    drawText(30, 170, "Sensor unavailable", 2, WHITE);
  } else if (!power.isBatteryConnect()) {
    drawText(30, 170, "No battery detected", 2, WHITE);
  } else {
    int percent = power.getBatteryPercent();

    String value =
      percent >= 0 && percent <= 100
        ? String(percent) + "%"
        : "Unavailable";

    drawText(30, 170, value, 3, WHITE);

    drawText(
      30, 210,
      power.isCharging() ? "Charging" : "Not charging",
      2, WHITE
    );
  }

  drawText(30, 265, "WI-FI", 2, CYAN);

  drawText(
    30, 300,
    WiFi.status() == WL_CONNECTED
      ? "Connected"
      : "Not connected",
    2, WHITE
  );

  if (WiFi.status() == WL_CONNECTED) {
    drawText(
      30, 325,
      "Signal: " + String(WiFi.RSSI()) + " dBm",
      2, WHITE
    );

    // The address the laptop sends the mood to.
    drawText(
      30, 350,
      "IP: " + WiFi.localIP().toString(),
      2, WHITE
    );
  }

  statusTime = millis();
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

  // Ask about a sad or mad mood once it has lasted a few seconds.
  if (
    !pressed &&
    !checkInHandled &&
    millis() - moodSince >= CHECK_IN_AFTER_MS
  ) {
    checkInHandled = true;

    String question = checkInQuestion();

    bool cooledDown =
      lastCheckIn == 0 ||
      millis() - lastCheckIn >= CHECK_IN_COOLDOWN_MS;

    if (question.length() && cooledDown) {
      lastCheckIn = millis();
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
          statusPage = true;
          homeVisible = false;

          gfx->fillScreen(BLACK);
          drawCentered(205, 40, "STATUS", 4, moodAccent());
          gfx->fillRect(150, 89, 110, 3, moodAccent());

          drawCircleButton();
          updateStatus();
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
    // Home screen animation: turning ring and pulsing core.
    if (millis() - ringTime >= RING_FRAME_MS) {
      stepRing();
      ringTime = millis();
    }

    if (millis() - pulseTime >= PULSE_MS) {
      pulseOn = !pulseOn;
      drawCoreOutline(pulseOn);
      pulseTime = millis();
    }
  }

  delay(15);
}
