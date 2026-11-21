/*
  VibeGuard music test.

  Type a mood in the Serial Monitor (115200 baud, "Newline" line ending):
    mad      -> calming music   (mp3/0001.mp3)
    sad      -> cheerful music  (mp3/0002.mp3)
    happy    -> upbeat music    (mp3/0003.mp3)
    neutral  -> music stops

  Same rules as the real VibeGuard sketch: the music only changes when the
  mood changes, and a track repeats while the mood stays the same.
*/

#include <Arduino.h>
#include <DFRobotDFPlayerMini.h>

const int TRACK_CALM     = 1;   // mad
const int TRACK_CHEERFUL = 2;   // sad
const int TRACK_UPBEAT   = 3;   // happy

const int VOLUME_CALM     = 12; // 0-30
const int VOLUME_CHEERFUL = 18;
const int VOLUME_UPBEAT   = 16;

const int MUSIC_RX_PIN = 2;     // ESP32 RX  <- DFPlayer TX
const int MUSIC_TX_PIN = 1;     // ESP32 TX  -> 1k resistor -> DFPlayer RX

HardwareSerial musicSerial(1);
DFRobotDFPlayerMini player;

String currentMood = "neutral";
int playingTrack = 0;
unsigned long trackStartedAt = 0;

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

void applyMood(String mood) {
  int track;
  if (mood == "mad") track = TRACK_CALM;
  else if (mood == "sad") track = TRACK_CHEERFUL;
  else if (mood == "happy") track = TRACK_UPBEAT;
  else if (mood == "neutral") track = 0;
  else {
    Serial.println("Type one of: happy, sad, mad, neutral");
    return;
  }

  Serial.println("mood -> " + mood);

  if (mood == currentMood) {
    Serial.println("(same mood - music keeps going)");
    return;
  }
  currentMood = mood;

  if (track == 0) {
    player.stop();
    playingTrack = 0;
    Serial.println("music -> off (neutral)");
  } else {
    playTrack(track);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  musicSerial.begin(9600, SERIAL_8N1, MUSIC_RX_PIN, MUSIC_TX_PIN);
  delay(1000);

  Serial.println("Connecting to DFPlayer...");
  if (!player.begin(musicSerial)) {
    Serial.println("DFPlayer not detected. Check SD card, power and RX/TX wiring.");
    while (true) delay(1000);
  }

  Serial.println("DFPlayer ready.");
  Serial.println("Type a mood: happy, sad, mad or neutral");
}

void loop() {
  // Read a mood typed in the Serial Monitor.
  if (Serial.available()) {
    String mood = Serial.readStringUntil('\n');
    mood.trim();
    mood.toLowerCase();
    if (mood.length()) applyMood(mood);
  }

  // When a track ends and the mood has not changed, play it again.
  if (player.available()) {
    uint8_t type = player.readType();
    int value = player.read();
    if (type == DFPlayerPlayFinished && playingTrack != 0 &&
        millis() - trackStartedAt > 2000) {
      Serial.println("(track ended - repeating)");
      playTrack(playingTrack);
    } else if (type == DFPlayerError) {
      Serial.printf("DFPlayer error: %d\n", value);
    }
  }
}