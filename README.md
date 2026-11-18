# VibeGuard

VibeGuard runs facial-expression detection on a laptop and sends mood commands to an ESP32 camera/light board and a voice watch.

## Files

- `emotion_tester.py`: laptop expression detector and device integration.
- `requirements.txt`: Python dependencies.
- `VibeGuard_FacialFeature/VibeGuard/VibeGuard.ino`: camera streaming and mood light firmware.
- `VibeGuard_Voice/VibeGuard_Voice.ino`: voice watch firmware.

## Laptop setup

Install dependencies with `python -m pip install -r requirements.txt`, then run `python emotion_tester.py`. The program downloads `face_landmarker.task` if missing. Press C to create personal calibration. Run `python emotion_tester.py --help` for camera, board, watch and bulb options.

## Local credentials

The shared sketches contain placeholders only. Configure Wi-Fi, hotspot and API credentials locally before flashing. Do not commit those local edits. Keep `wiz_key.txt`, `.env` files and personal calibration files out of Git. The `.gitignore` does not protect secrets placed inside tracked sketches: review every diff before pushing.

## Hardware setup

See the sketch comments for board configuration. Install the Arduino libraries referenced by each sketch. Exact tested dependency versions and complete hardware setup instructions still need to be documented. This source package has not been compiled or tested on the devices as part of repository preparation.

## Project documents

- [VibeGuard Gate 1](docs/VibeGuard_Gate1.pdf)
- [VibeGuard project document](docs/VibeGuard%20%285%29.pdf)
