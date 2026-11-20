# VibeGuard

**Project I (247-519-VA) — Pina Cammisano, Vanier College**

VibeGuard is a prototype that classifies four posed facial expressions on a laptop and sends matching mood commands to an ESP32-S3 camera/light board, a Waveshare voice watch and a WiZ lamp. It does not infer a person's actual feelings or establish therapeutic benefit.

## Gate 1 submission

- [Gate 1 report — IEEEtran PDF](docs/VibeGuard_Gate1.pdf)
- [Report LaTeX source](docs/gate1-source/main.tex)
- [Original project proposal](docs/VibeGuard.pdf)
- Tagged submission: [`gate1-v1.0`](https://github.com/Pinacam/VibeGuard/tree/gate1-v1.0)

The Gate 1 snapshot documents the prototype and its remaining work; it is not a claim that all acceptance requirements have passed.

## System

```text
ESP32-S3 camera --Wi-Fi video--> Laptop classifier
                                     |-- mood --> Camera board LED
                                     |-- mood --> WiZ lamp
                                     `-- mood --> Watch --internet--> Gemini voice services
DFPlayer music: planned, not integrated in this release.
```

## Source files

| File | Purpose |
| --- | --- |
| `emotion_tester.py` | MediaPipe features, personal calibration, smoothing and device commands |
| `requirements.txt` | Python dependencies |
| `VibeGuard_FacialFeature/VibeGuard/VibeGuard.ino` | Freenove ESP32-S3 camera streaming and RGB LED |
| `VibeGuard_Voice/VibeGuard_Voice.ino` | Waveshare ESP32-S3 watch display and voice interaction |

## Laptop setup

Use a Python installation supported by MediaPipe and OpenCV on your operating system. The project has previously used Python 3.12; a clean-machine rebuild remains an acceptance test.

```powershell
git clone https://github.com/Pinacam/VibeGuard.git
cd VibeGuard
git checkout gate1-v1.0
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe emotion_tester.py --selftest
.\.venv\Scripts\python.exe emotion_tester.py
```

The program downloads `face_landmarker.task` on first use, requiring internet access. Press **C** to calibrate, **R** to forget calibration, and **Q** or **Esc** to quit. Calibration is stored locally in `calibration.json` and excluded from Git.

For the complete setup, replace `CAMERA_IP` and `WATCH_IP` with the addresses of your devices:

```powershell
.\.venv\Scripts\python.exe emotion_tester.py --source http://CAMERA_IP:81/stream --esp http://CAMERA_IP --watch http://WATCH_IP --bulb auto
```

Use `--help` for all options. `--camtest --source http://CAMERA_IP:81/stream` measures the stream for 20 seconds. `--bulbtest --bulb auto` sends test colours to the lamp. All devices must be reachable from the laptop. Configure the WiZ bulb on the same network and enable its local communication; some firmware requires a local `wiz_key.txt` file.

## Firmware setup

Open each `.ino` in its matching folder with Arduino IDE. Configure credentials **only in a separate local working copy**, not in files you plan to commit.

### Camera board

- Hardware: Freenove ESP32-S3 WROOM camera kit.
- Board: **ESP32S3 Dev Module**; flash: **8 MB**; partition: **8M with spiffs (3MB APP/1.5MB SPIFFS)**; PSRAM: **OPI PSRAM**.
- Use the UART USB-C port. The source includes the camera pin mapping and uses GPIO 48 for the RGB LED.
- `OWN_WIFI = false` joins your existing Wi-Fi/hotspot. Set `WIFI_NAME` and `WIFI_PASSWORD` locally and use the IP printed in Serial Monitor.
- Optional `OWN_WIFI = true` creates an access point using your local hotspot settings, normally at `192.168.4.1`. This mode alone does not provide the internet connection needed by the voice watch.

### Voice watch

- Hardware: Waveshare ESP32-S3 2.06-inch AMOLED touch watch.
- The sketch uses the Espressif Arduino core, ArduinoJson, Arduino_GFX_Library, AudioDriver (ES7210 and ES8311 codecs), and XPowersLib (AXP2101).
- Configure Wi-Fi and a Gemini API key locally. The watch requires internet access and an account authorized for the configured models.
- Follow the board manufacturer's setup for the exact hardware revision. Tested Arduino core/library versions and full watch build settings have not yet been captured; a reproducible hardware build remains outstanding.

## Credentials and personal data

Published sketches contain placeholders. Do not commit Wi-Fi/hotspot passwords, API keys, `wiz_key.txt`, `.env` files or personal calibration. `.gitignore` excludes local secret and calibration files, but cannot protect secrets typed into tracked source: inspect your staged diff before pushing. Recordings are sent to Gemini when using voice interaction.

## Validation and limitations

- The Python `--selftest` passed during submission preparation. It checks synthetic classification and smoothing cases, not real-person accuracy.
- The report records informal camera/watch/lamp and voice trials. Counted expression, response-time and hold-to-talk acceptance tests remain open.
- Camera networking reliability, DFPlayer integration and persistent spoken correction remain unfinished.
- This release was not flashed or hardware-tested during submission preparation. Dependency versions are not locked.

## Rebuilding the report

Compile `docs/gate1-source/main.tex` with a TeX distribution containing `IEEEtran`, TikZ, tabularx and hyperref (for example, `latexmk -pdf main.tex` from that directory). The submitted PDF uses the actual `IEEEtran` conference class and its two-column layout.
