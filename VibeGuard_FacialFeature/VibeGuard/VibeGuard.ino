/*
  VibeGuard for the Freenove ESP32-S3 WROOM camera board.

  The board does two jobs:
    1. streams its camera over WiFi      http://<board-ip>:81/stream
    2. changes its light when told to    http://<board-ip>/mood?m=happy

  The laptop runs emotion_tester.py, which watches the stream, works out the
  mood and calls /mood. By default the board makes its own WiFi network
  ("MoodLight") and its address is 192.168.4.1, so connect the laptop to
  that network and start the tester with:

    python emotion_tester.py --source http://192.168.4.1:81/stream --esp http://192.168.4.1

  Arduino IDE settings (Tools menu):
    Board:            ESP32S3 Dev Module
    Flash Size:       8MB (64Mb)
    Partition Scheme: 8M with spiffs (3MB APP/1.5MB SPIFFS)
    PSRAM:            OPI PSRAM
  Plug the cable into the USB-C port marked UART.
*/

#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include <DFRobotDFPlayerMini.h>

// ---- network ---------------------------------------------------------------
// true  = the board creates its own WiFi network (works anywhere, e.g. school).
//         Connect the laptop to it; the board is always at 192.168.4.1.
// false = the board joins an existing home-style WiFi network instead.
const bool OWN_WIFI = false;

// Used when OWN_WIFI is true. The password must be at least 8 characters.
const char *OWN_WIFI_NAME = "VibeGuard";
const char *OWN_WIFI_PASSWORD = "YOUR_HOTSPOT_PASSWORD";

// Used when OWN_WIFI is false.
const char *WIFI_NAME = "YOUR_WIFI_NAME";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
// ----------------------------------------------------------------------------

// The small RGB light built into the board. To drive an external WS2812
// strip instead, change this to the pin its data wire is on.
const int LED_PIN = 48;
const int LED_BRIGHTNESS = 60;   // 0-255; the onboard light is very bright
const int FADE_STEP_MS = 15;     // lower = faster colour change

// Camera wiring on the Freenove ESP32-S3 WROOM board.
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM    8
#define Y3_GPIO_NUM    9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

struct Colour { uint8_t r, g, b; };

// Same colours the laptop preview uses.
const Colour NEUTRAL = {235, 235, 235};
const Colour HAPPY   = {255, 200, 0};
const Colour SAD     = {30, 110, 255};
const Colour MAD     = {255, 30, 30};

volatile Colour target = NEUTRAL;
Colour shown = {0, 0, 0};
char currentMood[12] = "neutral";

// ---- music (DFPlayer Mini) -------------------------------------------------
// The music tries to move the user toward happy or neutral:
//   mad     -> calming music
//   sad     -> cheerful music
//   happy   -> upbeat music that matches the mood
//   neutral -> no music (whatever was playing stops)
// Files on the microSD card, in a folder named "mp3":
const int TRACK_CALM     = 1;   // mp3/0001.mp3, played when mad
const int TRACK_CHEERFUL = 2;   // mp3/0002.mp3, played when sad
const int TRACK_UPBEAT   = 3;   // mp3/0003.mp3, played when happy

// Volume for each kind of music, 0-30. Calming music is kept softer.
const int VOLUME_CALM     = 12;
const int VOLUME_CHEERFUL = 18;
const int VOLUME_UPBEAT   = 16;

// Same pins as the working test sketch: ESP32 RX = 2, TX = 1.
const int MUSIC_RX_PIN = 2;
const int MUSIC_TX_PIN = 1;

HardwareSerial musicSerial(1);
DFRobotDFPlayerMini player;
bool musicReady = false;

// Set by the web server when the mood changes; handled in loop() so the
// player is only ever talked to from one place.
volatile bool musicChangePending = false;
volatile int pendingTrack = 0;          // 0 = silence
int playingTrack = 0;
unsigned long trackStartedAt = 0;

void startMusic() {
  musicSerial.begin(9600, SERIAL_8N1, MUSIC_RX_PIN, MUSIC_TX_PIN);
  delay(1000);
  if (!player.begin(musicSerial)) {
    Serial.println("DFPlayer not detected - check SD card, power and RX/TX. "
                   "Camera and lights still work.");
    return;
  }
  musicReady = true;
  Serial.println("DFPlayer ready.");
}

void playTrack(int track) {
  int volume = track == TRACK_CALM ? VOLUME_CALM
             : track == TRACK_CHEERFUL ? VOLUME_CHEERFUL
             : VOLUME_UPBEAT;
  player.volume(volume);
  delay(100);
  player.playMp3Folder(track);
  playingTrack = track;
  trackStartedAt = millis();
  Serial.printf("music -> track %d (volume %d)\n", track, volume);
}

void updateMusic() {
  if (!musicReady) return;

  if (musicChangePending) {
    musicChangePending = false;
    int track = pendingTrack;
    if (track == 0) {
      player.stop();
      playingTrack = 0;
      Serial.println("music -> off (neutral)");
    } else if (track != playingTrack) {
      playTrack(track);
    }
  }

  // When a track ends and the mood is still the same, play it again.
  if (player.available()) {
    uint8_t type = player.readType();
    int value = player.read();
    if (type == DFPlayerPlayFinished && playingTrack != 0 &&
        millis() - trackStartedAt > 2000) {
      playTrack(playingTrack);
    } else if (type == DFPlayerError) {
      Serial.printf("DFPlayer error: %d\n", value);
    }
  }
}
// ----------------------------------------------------------------------------

void showColour(Colour c) {
  uint8_t r = c.r * LED_BRIGHTNESS / 255;
  uint8_t g = c.g * LED_BRIGHTNESS / 255;
  uint8_t b = c.b * LED_BRIGHTNESS / 255;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(LED_PIN, r, g, b);
#else
  neopixelWrite(LED_PIN, r, g, b);
#endif
}

uint8_t stepToward(uint8_t from, uint8_t to) {
  int diff = (int)to - (int)from;
  if (abs(diff) <= 4) return to;
  return from + (diff > 0 ? 4 : -4);
}

// This is where a mood turns into "lighting and environment". Add anything
// else the mood should control here (a fan, a buzzer, a second light...).
bool applyMood(const char *mood) {
  Colour c;
  if (strcmp(mood, "happy") == 0) c = HAPPY;
  else if (strcmp(mood, "sad") == 0) c = SAD;
  else if (strcmp(mood, "mad") == 0) c = MAD;
  else if (strcmp(mood, "neutral") == 0) c = NEUTRAL;
  else return false;

  target.r = c.r; target.g = c.g; target.b = c.b;

  // The laptop repeats the mood every few seconds; only a change
  // affects the music.
  if (strcmp(mood, currentMood) != 0) {
    pendingTrack = strcmp(mood, "mad") == 0 ? TRACK_CALM
                 : strcmp(mood, "sad") == 0 ? TRACK_CHEERFUL
                 : strcmp(mood, "happy") == 0 ? TRACK_UPBEAT
                 : 0;
    musicChangePending = true;
  }

  strlcpy(currentMood, mood, sizeof(currentMood));
  Serial.printf("mood -> %s\n", mood);
  return true;
}

// GET /mood?m=happy
esp_err_t moodHandler(httpd_req_t *req) {
  char query[48], mood[12];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "m", mood, sizeof(mood)) != ESP_OK ||
      !applyMood(mood)) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "use /mood?m=happy|sad|mad|neutral");
  }
  return httpd_resp_sendstr(req, mood);
}

// GET /  - a small page for testing the lights from a browser
esp_err_t indexHandler(httpd_req_t *req) {
  char page[700];
  snprintf(page, sizeof(page),
    "<html><body style='font-family:sans-serif'><h2>Mood light</h2>"
    "<p>Current mood: <b>%s</b></p>"
    "<p><a href='/mood?m=happy'>happy</a> | <a href='/mood?m=sad'>sad</a> | "
    "<a href='/mood?m=mad'>mad</a> | <a href='/mood?m=neutral'>neutral</a></p>"
    "<img id='cam' width='480'>"
    "<script>document.getElementById('cam').src="
    "'http://'+location.hostname+':81/stream'</script></body></html>",
    currentMood);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_sendstr(req, page);
}

// GET :81/stream - endless series of JPEG frames (MJPEG)
esp_err_t streamHandler(httpd_req_t *req) {
  char header[64];
  esp_err_t res = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (res == ESP_OK) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("camera frame failed");
      return ESP_FAIL;
    }
    int n = snprintf(header, sizeof(header),
                     "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                     (unsigned)fb->len);
    res = httpd_resp_send_chunk(req, header, n);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    delay(100);
  }
  return res;   // the viewer disconnected
}

bool startCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;         // 10 MHz: less interference with the board's WiFi
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;     // 320x240: half the data of 480x320
  config.jpeg_quality = 15;               // lower number = sharper, bigger frames
  config.grab_mode = CAMERA_GRAB_LATEST;  // always send the newest frame
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.fb_count = 2;

  if (!psramFound()) {
    Serial.println("No PSRAM found - set Tools > PSRAM > OPI PSRAM. Using small frames.");
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera failed to start (error 0x%x). Check the ribbon cable.\n", err);
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);        // set to 0 if the picture is upside down
  s->set_brightness(s, 1);
  return true;
}

void startServers() {
  httpd_handle_t control = NULL, stream = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  httpd_uri_t indexUri = {.uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = NULL};
  httpd_uri_t moodUri = {.uri = "/mood", .method = HTTP_GET, .handler = moodHandler, .user_ctx = NULL};
  httpd_uri_t streamUri = {.uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = NULL};

  if (httpd_start(&control, &config) == ESP_OK) {
    httpd_register_uri_handler(control, &indexUri);
    httpd_register_uri_handler(control, &moodUri);
  }
  // The stream gets its own server so a long video connection never blocks /mood.
  config.server_port = 81;
  config.ctrl_port += 1;
  if (httpd_start(&stream, &config) == ESP_OK) {
    httpd_register_uri_handler(stream, &streamUri);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  showColour({40, 0, 40});   // purple = starting up

  bool cameraOk = startCamera();
  startMusic();

  String ip;
  if (OWN_WIFI) {
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(OWN_WIFI_NAME, OWN_WIFI_PASSWORD)) {
      Serial.println("Could not start WiFi - is the password at least 8 characters?");
    }
    WiFi.setSleep(false);    // keeps the video smooth
    ip = WiFi.softAPIP().toString();
    Serial.printf("Connect the laptop to the WiFi network \"%s\".\n", OWN_WIFI_NAME);
  } else {
    WiFi.begin(WIFI_NAME, WIFI_PASSWORD);
    WiFi.setSleep(false);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
      delay(400);
      Serial.print(".");
    }
    Serial.println();
    ip = WiFi.localIP().toString();
  }

  startServers();

  Serial.println("Ready. On the laptop run:");
  Serial.printf("  python emotion_tester.py --source http://%s:81/stream --esp http://%s\n",
                ip.c_str(), ip.c_str());
  Serial.printf("Or open http://%s in a browser to test the lights by hand.\n", ip.c_str());
  if (!cameraOk) Serial.println("(camera is NOT working - lights only)");
}

void loop() {
  // Fade smoothly toward the colour of the current mood.
  Colour t = {target.r, target.g, target.b};
  if (shown.r != t.r || shown.g != t.g || shown.b != t.b) {
    shown.r = stepToward(shown.r, t.r);
    shown.g = stepToward(shown.g, t.g);
    shown.b = stepToward(shown.b, t.b);
    showColour(shown);
  }
  updateMusic();
  delay(FADE_STEP_MS);
}