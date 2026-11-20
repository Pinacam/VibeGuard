"""
Facial expression tester: happy / sad / mad / neutral.

Runs on the laptop webcam or an ESP32-S3 camera stream and sends accepted
mood labels to the configured board, watch and WiZ bulb.

    python emotion_tester.py                          laptop webcam
    python emotion_tester.py --source http://IP:81/stream      ESP32 camera
    python emotion_tester.py --esp http://IP          also send mood to the board
    python emotion_tester.py --selftest               check the logic, no camera

Keys:  C = calibrate to your face (recommended)   R = forget calibration
       Q or Esc = quit
"""

import argparse
import json
import sys
import threading
import time
import urllib.request
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
MODEL_PATH = HERE / "face_landmarker.task"
MODEL_URL = ("https://storage.googleapis.com/mediapipe-models/face_landmarker/"
             "face_landmarker/float16/1/face_landmarker.task")
CALIB_PATH = HERE / "calibration.json"
WIZ_KEY_PATH = HERE / "wiz_key.txt"
CALIB_VERSION = 2

EMOTIONS = ["neutral", "happy", "sad", "mad"]

# Lighting each mood maps to (B, G, R) - this is what the ESP32 will show later.
MOOD_COLOURS = {
    "neutral": (235, 235, 235),
    "happy": (0, 200, 255),
    "sad": (255, 110, 30),
    "mad": (30, 30, 255),
}

# ---- tuning -----------------------------------------------------------------
NEUTRAL_THRESHOLD = 0.22   # rules mode: below this, nothing counts as an expression
SMOOTHING = 0.35           # 0..1, higher = reacts faster but flickers more
HOLD_FRAMES = 6            # frames a new mood must win before we switch to it
CALIB_READY_SECONDS = 3.0  # time to pull the face
CALIB_RECORD_SECONDS = 3.0 # time it is recorded for
MIN_SEPARATION = 2.5       # calibration warns if two faces are closer than this
# -----------------------------------------------------------------------------


def avg(bs, *names):
    return sum(bs.get(n, 0.0) for n in names) / len(names)


def rule_scores(bs, baseline=None):
    """Score each emotion from MediaPipe blendshapes (dict name -> 0..1).

    Used only until the user calibrates. baseline is an optional resting face.
    """
    if baseline:
        bs = {k: max(0.0, v - baseline.get(k, 0.0)) for k, v in bs.items()}

    smile = avg(bs, "mouthSmileLeft", "mouthSmileRight")
    frown = avg(bs, "mouthFrownLeft", "mouthFrownRight")
    brow_down = avg(bs, "browDownLeft", "browDownRight")
    brow_outer_up = avg(bs, "browOuterUpLeft", "browOuterUpRight")
    brow_inner_up = bs.get("browInnerUp", 0.0)
    sneer = avg(bs, "noseSneerLeft", "noseSneerRight")
    press = avg(bs, "mouthPressLeft", "mouthPressRight")
    cheek = avg(bs, "cheekSquintLeft", "cheekSquintRight")
    shrug = bs.get("mouthShrugLower", 0.0)

    # Inner brows up with outer brows up is surprise, not sadness.
    sad_brow = max(0.0, brow_inner_up - brow_outer_up)

    happy = smile + 0.3 * cheek
    mad = brow_down + 0.4 * sneer + 0.2 * press
    # The frown blendshape is weak for most people, so it is weighted up.
    sad = 2.0 * frown + 0.7 * sad_brow + 0.3 * shrug
    # A smile rules out sad/mad (smiling squints the eyes and lowers the brows).
    sad = max(0.0, sad - smile)
    mad = max(0.0, mad - smile)

    strongest = max(happy, sad, mad)
    # Below the threshold nothing counts as an expression, so neutral wins.
    neutral = 0.0 if strongest >= NEUTRAL_THRESHOLD else NEUTRAL_THRESHOLD

    raw = np.array([neutral, happy, sad, mad], dtype=float)
    return raw / raw.sum() if raw.sum() > 0 else np.array([1.0, 0, 0, 0])


# Landmark numbers in MediaPipe's 478-point face mesh.
L_EYE_OUT, R_EYE_OUT, L_EYE_IN, R_EYE_IN = 33, 263, 133, 362
L_BROW_IN, R_BROW_IN, L_BROW_OUT, R_BROW_OUT = 107, 336, 70, 300
MOUTH_L, MOUTH_R, LIP_TOP, LIP_BOTTOM = 61, 291, 13, 14
LOWER_LIP_EDGE, CHIN, NOSE_BASE = 17, 152, 2


def extract_features(bs, landmarks=None, matrix=None, width=1, height=1):
    """Everything the personal classifier looks at, as one dict.

    Blendshapes alone miss subtle faces (a quiet sad face barely moves them),
    so this adds direct measurements of the mouth corners and eyebrows, plus
    head pitch and downward gaze, which people often change when sad.
    Measurements are in units of eye-to-eye distance and corrected for head
    tilt, so moving closer or leaning does not change them.
    """
    f = {k: v for k, v in bs.items()
         if not (k.startswith("eyeLook") or k.startswith("eyeBlink") or k == "_neutral")}
    f["gazeDown"] = avg(bs, "eyeLookDownLeft", "eyeLookDownRight")

    if landmarks is not None:
        pts = np.array([[p.x * width, p.y * height] for p in landmarks])
        across = pts[R_EYE_OUT] - pts[L_EYE_OUT]
        if across[0] < 0:
            across = -across
        eye_dist = np.linalg.norm(across) + 1e-6
        ux = across / eye_dist
        uy = np.array([-ux[1], ux[0]])          # "down the face"

        def y(i):
            return float((pts[i] - pts[L_EYE_OUT]) @ uy) / eye_dist

        mouth_mid = (y(LIP_TOP) + y(LIP_BOTTOM)) / 2
        inner = ((y(L_EYE_IN) - y(L_BROW_IN)) + (y(R_EYE_IN) - y(R_BROW_IN))) / 2
        outer = ((y(L_EYE_OUT) - y(L_BROW_OUT)) + (y(R_EYE_OUT) - y(R_BROW_OUT))) / 2
        f["geo:cornerDroop"] = (y(MOUTH_L) + y(MOUTH_R)) / 2 - mouth_mid
        f["geo:browInner"] = inner
        f["geo:browOuter"] = outer
        f["geo:browSlope"] = inner - outer
        f["geo:mouthWidth"] = float(np.linalg.norm(pts[MOUTH_L] - pts[MOUTH_R])) / eye_dist
        f["geo:lipToChin"] = y(CHIN) - y(LOWER_LIP_EDGE)
        f["geo:noseToLip"] = y(LIP_TOP) - y(NOSE_BASE)

    if matrix is not None:
        rot = np.asarray(matrix)[:3, :3]
        rot = rot / (np.linalg.norm(rot[:, 0]) + 1e-9)
        f["pose:pitch"] = float(rot[1, 2])
    return f


def noise_floor(name):
    """Smallest wobble we assume a feature has, so nothing is trusted too much."""
    if name.startswith("geo:"):
        return 0.012
    if name.startswith("pose:"):
        return 0.06
    return 0.03


class PersonalModel:
    """Classifier built from the user's own recorded faces.

    Each feature is measured in units of how much it naturally wobbles for
    this person. That way a small but steady difference (the only thing that
    separates a subtle sad face from neutral) counts as much as a big smile,
    and features that are the same in two faces cancel out.
    """

    def __init__(self, mean, std):
        self.mean, self.std = mean, std
        self.names = sorted(set.intersection(*(set(mean[e]) for e in EMOTIONS)))
        self.mu = np.array([[mean[e][n] for n in self.names] for e in EMOTIONS])
        var = np.mean([[std[e][n] ** 2 for n in self.names] for e in EMOTIONS], axis=0)
        self.scale = np.sqrt(var) + np.array([noise_floor(n) for n in self.names])

    @classmethod
    def from_samples(cls, samples):
        """samples: {emotion: [feature dict per frame]}"""
        mean, std = {}, {}
        for e in EMOTIONS:
            keys = set.intersection(*(set(s) for s in samples[e]))
            mean[e] = {k: float(np.mean([s[k] for s in samples[e]])) for k in keys}
            std[e] = {k: float(np.std([s[k] for s in samples[e]])) for k in keys}
        return cls(mean, std)

    def save(self, path):
        path.write_text(json.dumps(
            {"version": CALIB_VERSION, "mean": self.mean, "std": self.std}))

    @classmethod
    def load(cls, path):
        data = json.loads(path.read_text())
        if data.get("version") != CALIB_VERSION:
            raise ValueError("old calibration format")
        return cls(data["mean"], data["std"])

    def distances(self, feats):
        x = np.array([feats.get(n, 0.0) for n in self.names])
        z2 = ((x - self.mu) / self.scale) ** 2
        return np.minimum(z2, 25.0).sum(axis=1)   # cap so one odd feature can't decide

    def scores(self, feats):
        d = self.distances(feats)
        w = np.exp(-(d - d.min()) / 8.0)
        return w / w.sum()

    def too_similar(self):
        """Pairs of recorded faces the camera can barely tell apart."""
        pairs = []
        for i, a in enumerate(EMOTIONS):
            for j in range(i + 1, len(EMOTIONS)):
                gap = np.abs(self.mu[i] - self.mu[j]) / self.scale
                if np.sqrt((np.minimum(gap, 5.0) ** 2).sum()) < MIN_SEPARATION:
                    pairs.append((a, EMOTIONS[j]))
        return pairs

    def explain(self, a, b, top=4):
        """Which features separate two recorded faces the most."""
        i, j = EMOTIONS.index(a), EMOTIONS.index(b)
        gap = (self.mu[j] - self.mu[i]) / self.scale
        order = np.argsort(-np.abs(gap))[:top]
        return ", ".join(f"{self.names[k]} {gap[k]:+.1f}" for k in order)


class MoodTracker:
    """Smooths per-frame scores and only switches mood when it is held."""

    def __init__(self):
        self.smoothed = np.array([1.0, 0.0, 0.0, 0.0])
        self.mood = "neutral"
        self._candidate = "neutral"
        self._count = 0

    def update(self, scores):
        self.smoothed = (1 - SMOOTHING) * self.smoothed + SMOOTHING * scores
        top = EMOTIONS[int(np.argmax(self.smoothed))]
        if top == self._candidate:
            self._count += 1
        else:
            self._candidate, self._count = top, 1
        if self._candidate != self.mood and self._count >= HOLD_FRAMES:
            self.mood = self._candidate
            return True
        return False


def selftest():
    cases = {
        "neutral": dict(mouthSmileLeft=0.03, browDownLeft=0.05, browDownRight=0.05),
        "happy": dict(mouthSmileLeft=0.8, mouthSmileRight=0.75, cheekSquintLeft=0.4,
                      cheekSquintRight=0.4, browDownLeft=0.2, browDownRight=0.2),
        "sad": dict(mouthFrownLeft=0.2, mouthFrownRight=0.22, browInnerUp=0.5,
                    mouthShrugLower=0.3),
        "mad": dict(browDownLeft=0.6, browDownRight=0.65, noseSneerLeft=0.2,
                    noseSneerRight=0.2, mouthPressLeft=0.3, mouthPressRight=0.3),
    }
    ok = True
    for want, bs in cases.items():
        got = EMOTIONS[int(np.argmax(rule_scores(bs)))]
        print(f"rules      {want:8s} -> {got:8s} {'ok' if got == want else 'WRONG'}")
        ok &= got == want
    got = EMOTIONS[int(np.argmax(rule_scores(
        dict(browInnerUp=0.7, browOuterUpLeft=0.7, browOuterUpRight=0.7))))]
    print(f"rules      surprise -> {got:8s} {'ok' if got != 'sad' else 'WRONG'}")
    ok &= got != "sad"

    # A hard, realistic case: sad is almost the same as neutral on the big
    # blendshapes (which also wobble a lot) and differs only by small steady
    # shifts in the mouth corners, brow slope and pucker.
    rng = np.random.default_rng(1)
    base = {"mouthSmileLeft": 0.0, "mouthSmileRight": 0.0, "browOuterUpLeft": 0.38,
            "browOuterUpRight": 0.38, "mouthPucker": 0.38, "browDownLeft": 0.01,
            "browDownRight": 0.01, "mouthShrugUpper": 0.24, "jawOpen": 0.02,
            "geo:cornerDroop": 0.00, "geo:browSlope": 0.02, "geo:mouthWidth": 0.80,
            "pose:pitch": 0.0, "gazeDown": 0.2}
    shifts = {
        "neutral": {},
        "happy": {"mouthSmileLeft": 0.9, "mouthSmileRight": 0.9, "mouthPucker": -0.37,
                  "geo:mouthWidth": 0.2, "geo:cornerDroop": -0.08},
        "sad": {"mouthPucker": -0.15, "geo:cornerDroop": 0.035, "geo:browSlope": 0.03,
                "gazeDown": 0.15},
        "mad": {"browDownLeft": 0.3, "browDownRight": 0.3, "browOuterUpLeft": -0.25,
                "browOuterUpRight": -0.25, "geo:browSlope": -0.05},
    }
    wobble = {"browOuterUpLeft": 0.08, "browOuterUpRight": 0.08, "mouthShrugUpper": 0.06,
              "jawOpen": 0.03, "pose:pitch": 0.05}

    def sample(e):
        return {k: v + shifts[e].get(k, 0.0)
                + rng.normal(0, wobble.get(k, 0.01 if k.startswith("geo:") else 0.02))
                for k, v in base.items()}

    model = PersonalModel.from_samples({e: [sample(e) for _ in range(60)]
                                        for e in EMOTIONS})
    for want in EMOTIONS:
        hits = sum(EMOTIONS[int(np.argmax(model.scores(sample(want))))] == want
                   for _ in range(300))
        good = hits >= 285
        print(f"personal   {want:8s} -> {hits / 3:.0f}% right {'ok' if good else 'WRONG'}")
        ok &= good
    print(f"personal   sad vs neutral told apart by: {model.explain('neutral', 'sad')}")
    good = not model.too_similar()
    print(f"personal   distinct faces pass the similarity check {'ok' if good else 'WRONG'}")
    ok &= good

    same = PersonalModel.from_samples({e: [sample("neutral" if e == "sad" else e)
                                           for _ in range(60)] for e in EMOTIONS})
    good = same.too_similar() == [("neutral", "sad")]
    print(f"personal   identical sad/neutral gets flagged {'ok' if good else 'WRONG'}")
    ok &= good

    t = MoodTracker()
    flips = sum(t.update(rule_scores(cases["happy" if i % 2 else "neutral"]))
                for i in range(40))
    print(f"smoothing  alternating frames caused {flips} switches "
          f"{'ok' if flips <= 1 else 'WRONG'}")
    ok &= flips <= 1
    for _ in range(30):
        t.update(rule_scores(cases["mad"]))
    print(f"smoothing  held mad -> {t.mood} {'ok' if t.mood == 'mad' else 'WRONG'}")
    ok &= t.mood == "mad"
    print("ALL OK" if ok else "FAILED")
    return 0 if ok else 1


class WizBulb:
    """Controls a WiZ smart bulb directly over the local network.

    WiZ bulbs listen for small JSON messages on UDP port 38899. Nothing goes
    through the internet, but the laptop and the bulb must be on the same
    WiFi, and "local communication" must be allowed in the WiZ app.
    """

    PORT = 38899

    # What the room light does for each mood. "dimming" is brightness 10-100.
    # Neutral uses a warm white ("temp" in kelvin) instead of a colour.
    SCENES = {
        "neutral": {"temp": 3000, "dimming": 70},
        "happy": {"r": 255, "g": 170, "b": 0, "dimming": 100},
        "sad": {"r": 0, "g": 60, "b": 255, "dimming": 60},
        "mad": {"r": 255, "g": 0, "b": 0, "dimming": 60},
    }

    def __init__(self, address, key_path=None):
        import socket
        self.socket_module = socket
        self.key = self.load_key(key_path)
        print("Bulb: signing commands with your home key." if self.key
              else "Bulb: no wiz_key.txt found, sending unsigned commands.")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.settimeout(0.5)
        self.ip = None if address == "auto" else address
        self.searching = False
        self.warned = False
        if self.ip is None:
            self.find_in_background()
        else:
            print(f"Bulb: using {self.ip}")

    def _broadcast_addresses(self):
        """Where to shout "any bulbs here?" - every network the laptop is on."""
        out = {"255.255.255.255"}
        try:
            name = self.socket_module.gethostname()
            for ip in self.socket_module.gethostbyname_ex(name)[2]:
                parts = ip.split(".")
                out.add(".".join(parts[:3] + ["255"]))
                if ip.startswith("172.20.10."):      # iPhone hotspots use a small range
                    out.add("172.20.10.15")
        except OSError:
            pass
        return out

    def find(self, wait=2.0):
        message = json.dumps({"method": "getPilot", "params": {}}).encode()
        for address in self._broadcast_addresses():
            try:
                self.sock.sendto(message, (address, self.PORT))
            except OSError:
                pass
        end = time.monotonic() + wait
        while time.monotonic() < end:
            try:
                data, sender = self.sock.recvfrom(2048)
            except OSError:
                continue
            if b'"result"' in data or b'"getPilot"' in data:
                return sender[0]
        return None

    def find_in_background(self):
        if self.searching:
            return
        self.searching = True

        def go():
            ip = self.find()
            if ip:
                self.ip = ip
                self.warned = False
                print(f"Bulb: found at {ip}")
            elif not self.warned:
                print("Bulb: not found yet. Is it on, set up in the WiZ app on this "
                      "WiFi, with local communication allowed? Still looking...")
                self.warned = True
            self.searching = False
        threading.Thread(target=go, daemon=True).start()

    @staticmethod
    def load_key(path=None):
        """Reads the home's signing key, needed by bulbs with newer firmware.

        The file holds either the 32-character key on its own, or the whole
        "local integration" file exported from the WiZ app.
        """
        path = Path(path) if path else WIZ_KEY_PATH
        if not path.exists():
            return None
        text = path.read_text(encoding="utf-8-sig").strip()
        if text.startswith("{"):
            def search(node):
                if isinstance(node, dict):
                    if isinstance(node.get("udp_signing_key"), str):
                        return node["udp_signing_key"]
                    node = list(node.values())
                if isinstance(node, list):
                    for item in node:
                        found = search(item)
                        if found:
                            return found
                return None
            text = search(json.loads(text)) or ""
        try:
            key = bytes.fromhex(text.strip())
        except ValueError:
            key = b""
        if len(key) != 16:
            print(f"Bulb: {path.name} does not contain a valid 32-character key.")
            return None
        return key

    @staticmethod
    def build_message(params, key=None, timestamp=None):
        """The bytes sent to the bulb; signed if a key is given.

        Signing: add the current time to the settings, then attach an
        HMAC-SHA256 of exactly those settings so the bulb knows the command
        comes from someone holding the home's key.
        """
        if key is None:
            return json.dumps({"method": "setPilot", "params": params}).encode()
        import base64
        import hashlib
        import hmac
        params = dict(params)
        params["sigTs"] = int(time.time()) if timestamp is None else timestamp
        body = json.dumps(params, separators=(",", ":"))
        digest = hmac.new(key, body.encode("utf-8"), hashlib.sha256).digest()
        return json.dumps({"method": "setPilot", "env": "pro", "params": params,
                           "hmac": base64.b64encode(digest).decode("ascii")},
                          separators=(",", ":")).encode()

    def send(self, params, wait_reply=False):
        """Sends one command. With wait_reply, returns the bulb's error or ''."""
        message = self.build_message(params, self.key)
        try:
            self.sock.sendto(message, (self.ip, self.PORT))
            if not wait_reply:
                return ""
            end = time.monotonic() + 2.0
            while time.monotonic() < end:
                try:
                    data, sender = self.sock.recvfrom(4096)
                except OSError:
                    continue
                if sender[0] != self.ip or b"setPilot" not in data:
                    continue
                error = json.loads(data).get("error")
                return "" if not error else f"{error.get('code')} {error.get('message')}"
            return "no reply"
        except OSError as e:
            return str(e)

    @staticmethod
    def explain(error):
        if "-32602" in error:
            return ("The bulb refused the command because it was not signed. Its "
                    "firmware needs your home's key: see wiz_key.txt instructions.")
        if "-32605" in error:
            return ("The bulb rejected the signature. Check wiz_key.txt holds the "
                    "right key and that this laptop's clock is correct.")
        return f"The bulb answered: {error}"

    def set_mood(self, mood):
        if self.ip is None:
            self.find_in_background()
            return
        error = self.send(dict(self.SCENES[mood], state=True))
        if error and not self.warned:
            print(f"Bulb: could not send ({error})")
            self.warned = True


class FrameGrabber:
    """Reads the camera on its own thread and keeps only the newest frame.

    This stops a WiFi stream from falling behind (old frames piling up), and
    reconnects by itself if the stream drops.
    """

    def __init__(self, cv2, source):
        self.cv2, self.source = cv2, source
        self.is_webcam = source.isdigit()
        self.frame, self.count, self.stamp = None, 0, 0.0
        self.error = None
        self.bad_frames = 0      # frames that arrived damaged and were skipped
        self.fps = 0.0
        self._fps_start, self._fps_count = time.monotonic(), 0
        self._lock = threading.Lock()
        self._stop = False
        run = self._run_webcam if self.is_webcam else self._run_stream
        threading.Thread(target=run, daemon=True).start()

    def _store(self, frame):
        now = time.monotonic()
        with self._lock:
            self.frame, self.stamp = frame, now
            self.count += 1
        self._fps_count += 1
        if now - self._fps_start >= 2.0:
            self.fps = self._fps_count / (now - self._fps_start)
            self._fps_start, self._fps_count = now, 0

    def _run_webcam(self):
        while not self._stop:
            cap = self.cv2.VideoCapture(int(self.source), self.cv2.CAP_DSHOW)
            if not cap.isOpened():
                self.error = "cannot open"
                cap.release()
                time.sleep(1.0)
                continue
            self.error = None
            while not self._stop:
                ok, frame = cap.read()
                if not ok:
                    break
                self._store(frame)
            cap.release()
            self.error = "camera stopped"
            time.sleep(0.5)

    def _run_stream(self):
        """Reads the board's MJPEG stream directly, one JPEG picture at a time."""
        while not self._stop:
            try:
                with urllib.request.urlopen(self.source, timeout=4) as resp:
                    self.error = None
                    buf = b""
                    while not self._stop:
                        chunk = resp.read1(65536)
                        if not chunk:
                            break
                        buf += chunk
                        while True:
                            start = buf.find(b"\xff\xd8")       # JPEG start marker
                            if start < 0:
                                buf = buf[-1:]
                                break
                            end = buf.find(b"\xff\xd9", start + 2)   # JPEG end marker
                            if end < 0:
                                buf = buf[start:]
                                break
                            jpg, buf = buf[start:end + 2], buf[end + 2:]
                            frame = self.cv2.imdecode(
                                np.frombuffer(jpg, np.uint8), self.cv2.IMREAD_COLOR)
                            if frame is None:
                                self.bad_frames += 1
                            else:
                                self._store(frame)
                        if len(buf) > 4_000_000:
                            buf = b""
                self.error = "stream ended"
            except Exception as e:
                self.error = f"{type(e).__name__}: {e}"
            time.sleep(0.5)

    def latest(self):
        """Returns (frame, frame number, seconds since it arrived)."""
        with self._lock:
            return self.frame, self.count, time.monotonic() - self.stamp

    def stop(self):
        self._stop = True


def ensure_model():
    if MODEL_PATH.exists():
        return
    print("Downloading face model (about 4 MB, first run only)...")
    urllib.request.urlretrieve(MODEL_URL, MODEL_PATH)


class MoodSender:
    """Tells the board and the watch the mood without blocking the video loop.

    The mood is sent when it changes and repeated every few seconds, so a
    device that was busy or briefly offline still catches up.
    """

    REPEAT_SECONDS = 5.0

    def __init__(self, targets, bulb=None):
        self.targets = {name: url.rstrip("/") for name, url in targets.items() if url}
        self.bulb = bulb
        self.busy = set()
        self.failing = set()
        self.last_sent = 0.0

    def _send_one(self, name, mood):
        try:
            urllib.request.urlopen(f"{self.targets[name]}/mood?m={mood}", timeout=3)
            if name in self.failing:
                print(f"({name} is reachable again)")
                self.failing.discard(name)
        except Exception as e:
            if name not in self.failing:      # say it once, not every 5 seconds
                print(f"(could not reach {name}: {e})")
                self.failing.add(name)
        finally:
            self.busy.discard(name)

    def send(self, mood):
        self.last_sent = time.monotonic()
        if self.bulb:
            self.bulb.set_mood(mood)
        for name in self.targets:
            if name in self.busy:             # previous message still in flight
                continue
            self.busy.add(name)
            threading.Thread(target=self._send_one, args=(name, mood),
                             daemon=True).start()

    def keep_alive(self, mood):
        if ((self.targets or self.bulb)
                and time.monotonic() - self.last_sent >= self.REPEAT_SECONDS):
            self.send(mood)


def draw_ui(cv2, frame, tracker, mode, face_box, banner):
    h, w = frame.shape[:2]
    colour = MOOD_COLOURS[tracker.mood]

    # Border = preview of the room lighting for this mood.
    cv2.rectangle(frame, (0, 0), (w - 1, h - 1), colour, 14)
    if face_box:
        cv2.rectangle(frame, face_box[0], face_box[1], colour, 2)

    panel = frame.copy()
    cv2.rectangle(panel, (14, 14), (270, 180), (20, 20, 20), -1)
    cv2.addWeighted(panel, 0.6, frame, 0.4, 0, frame)

    cv2.putText(frame, tracker.mood.upper(), (26, 52), cv2.FONT_HERSHEY_SIMPLEX,
                1.1, colour, 3, cv2.LINE_AA)
    for i, e in enumerate(EMOTIONS):
        y = 78 + i * 22
        cv2.putText(frame, e, (26, y + 10), cv2.FONT_HERSHEY_SIMPLEX, 0.45,
                    (220, 220, 220), 1, cv2.LINE_AA)
        cv2.rectangle(frame, (95, y), (95 + 160, y + 12), (70, 70, 70), 1)
        cv2.rectangle(frame, (95, y), (95 + int(160 * tracker.smoothed[i]), y + 12),
                      MOOD_COLOURS[e], -1)
    cv2.putText(frame, f"mode: {mode}", (26, 172), cv2.FONT_HERSHEY_SIMPLEX, 0.42,
                (170, 170, 170), 1, cv2.LINE_AA)

    text = banner or "C calibrate   R reset   Q quit"
    size = 0.5
    if banner:
        size = 0.8
        while size > 0.4 and cv2.getTextSize(
                text, cv2.FONT_HERSHEY_SIMPLEX, size, 2)[0][0] > w - 40:
            size -= 0.05
    (tw, _), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, size, 2)
    cv2.rectangle(frame, (14, h - 54), (w - 14, h - 14), (20, 20, 20), -1)
    cv2.putText(frame, text, ((w - tw) // 2, h - 27), cv2.FONT_HERSHEY_SIMPLEX,
                size, (255, 255, 255), 2 if banner else 1, cv2.LINE_AA)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", default="0",
                    help="webcam number (0, 1, ...) or the ESP32 stream URL")
    ap.add_argument("--esp", default=None,
                    help="ESP32 address, e.g. http://192.168.1.50 - mood changes "
                         "are sent to <address>/mood?m=happy")
    ap.add_argument("--watch", default=None,
                    help="watch address, e.g. http://vibeguard-watch.local - mood "
                         "changes are sent to <address>/mood?m=sad")
    ap.add_argument("--bulb", default=None,
                    help="WiZ smart bulb: 'auto' to find it on the WiFi, or its "
                         "address, e.g. 172.20.10.9")
    ap.add_argument("--bulbtest", action="store_true",
                    help="find the bulb and cycle it through the four mood colours")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--camtest", action="store_true",
                    help="measure the camera feed for 20 seconds and print the results")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if args.bulbtest:
        # Checks the bulb on its own: no camera, board or watch needed.
        bulb = WizBulb(args.bulb or "auto")
        for _ in range(5):
            if bulb.ip:
                break
            time.sleep(2.5)
            if not bulb.ip:
                bulb.find_in_background()
        if not bulb.ip:
            print("No bulb answered. Check it is powered, set up in the WiZ app on "
                  "the same WiFi as this laptop, and that local communication is on.")
            return 1
        for mood in ["happy", "sad", "mad", "neutral"]:
            error = bulb.send(dict(bulb.SCENES[mood], state=True), wait_reply=True)
            if error:
                print(f"Bulb -> {mood}: FAILED. {bulb.explain(error)}")
                return 1
            print(f"Bulb -> {mood}: accepted")
            time.sleep(2.5)
        print("Done. The bulb accepted all four colours.")
        return 0

    import cv2

    if args.camtest:
        # Measures the camera feed on its own, with no face detection.
        grabber = FrameGrabber(cv2, args.source)
        print("Measuring the camera for 20 seconds...")
        last_count, last_stamp, worst_gap = 0, None, 0.0
        for second in range(1, 21):
            end = time.monotonic() + 1.0
            while time.monotonic() < end:
                frame, count, _ = grabber.latest()
                if count != last_count and frame is not None:
                    if last_stamp is not None:
                        worst_gap = max(worst_gap, grabber.stamp - last_stamp)
                    last_count, last_stamp = count, grabber.stamp
                time.sleep(0.005)
            size = "-" if frame is None else f"{frame.shape[1]}x{frame.shape[0]}"
            print(f"{second:2d}s  frames so far {grabber.count:4d}  {grabber.fps:4.1f} fps  "
                  f"size {size}  damaged {grabber.bad_frames}  "
                  f"longest freeze {worst_gap:.2f}s  {grabber.error or ''}")
        grabber.stop()
        return 0

    import mediapipe as mp
    from mediapipe.tasks import python as mp_tasks
    from mediapipe.tasks.python import vision

    ensure_model()
    landmarker = vision.FaceLandmarker.create_from_options(vision.FaceLandmarkerOptions(
        base_options=mp_tasks.BaseOptions(model_asset_path=str(MODEL_PATH)),
        running_mode=vision.RunningMode.VIDEO,
        num_faces=1,
        output_face_blendshapes=True,
        output_facial_transformation_matrixes=True,
    ))

    is_webcam = args.source.isdigit()
    grabber = FrameGrabber(cv2, args.source)
    lost_help = ("close other apps using the webcam, or try --source 1" if is_webcam
                 else "is the laptop still on the board's WiFi?")
    seen = 0
    warned = False

    bulb = WizBulb(args.bulb) if args.bulb else None
    sender = MoodSender({"camera board": args.esp, "watch": args.watch}, bulb)
    tracker = MoodTracker()
    calib = None       # {"step": i, "start": t, "samples": {emotion: [...]}}
    last_ts = 0
    notice, notice_until = "", 0.0

    model = None
    if CALIB_PATH.exists():
        try:
            model = PersonalModel.load(CALIB_PATH)
            print("Loaded your saved calibration (press R to discard it).")
        except Exception:
            print("Your saved calibration is from an older version - press C to redo it.")
            notice, notice_until = "Press C to calibrate again", time.monotonic() + 6

    while True:
        frame, number, age = grabber.latest()
        if frame is None or age > 2.0:
            # No picture, or it has frozen: say so instead of glitching.
            if not warned:
                print(f"No picture from the camera ({grabber.error}) - {lost_help}")
                warned = True
            waiting = (np.zeros((480, 640, 3), np.uint8) if frame is None
                       else (frame * 0.35).astype(np.uint8))
            draw_ui(cv2, waiting, tracker, "waiting for camera", None,
                    f"No picture - {lost_help}")
            cv2.imshow("Emotion tester", waiting)
            key = cv2.waitKey(100) & 0xFF
            if key in (ord("q"), 27) or cv2.getWindowProperty(
                    "Emotion tester", cv2.WND_PROP_VISIBLE) < 1:
                break
            continue
        if warned:
            print("Camera picture is back.")
            warned = False
        if number == seen:
            # Same frame as last time; wait for a new one.
            key = cv2.waitKey(5) & 0xFF
            if key in (ord("q"), 27):
                break
            continue
        seen = number
        frame = frame.copy()
        if is_webcam:
            frame = cv2.flip(frame, 1)  # mirror, feels natural

        ts = max(last_ts + 1, int(time.monotonic() * 1000))
        last_ts = ts
        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        result = landmarker.detect_for_video(
            mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb), ts)

        bs, feats, face_box = None, None, None
        if result.face_blendshapes:
            bs = {c.category_name: float(c.score) for c in result.face_blendshapes[0]}
            h, w = frame.shape[:2]
            marks = result.face_landmarks[0]
            matrices = result.facial_transformation_matrixes
            feats = extract_features(bs, marks, matrices[0] if matrices else None, w, h)
            xs = [p.x * w for p in marks]
            ys = [p.y * h for p in marks]
            face_box = ((int(min(xs)), int(min(ys))), (int(max(xs)), int(max(ys))))

        now = time.monotonic()
        banner = notice if now < notice_until else ""

        if calib is not None:
            emotion = EMOTIONS[calib["step"]]
            elapsed = now - calib["start"]
            if elapsed < CALIB_READY_SECONDS:
                banner = f"Make a {emotion.upper()} face... {CALIB_READY_SECONDS - elapsed:.0f}"
            elif elapsed < CALIB_READY_SECONDS + CALIB_RECORD_SECONDS:
                banner = f"HOLD {emotion.upper()} - recording"
                if feats:
                    calib["samples"][emotion].append(feats)
            elif len(calib["samples"][emotion]) < 10:
                notice, notice_until = "No face seen - calibration cancelled", now + 3
                calib = None
            else:
                calib["step"] += 1
                calib["start"] = now
                if calib["step"] == len(EMOTIONS):
                    model = PersonalModel.from_samples(calib["samples"])
                    model.save(CALIB_PATH)
                    calib = None
                    clashes = model.too_similar()
                    for a, b in [(a, b) for i, a in enumerate(EMOTIONS)
                                 for b in EMOTIONS[i + 1:]]:
                        print(f"  {a} vs {b}: {model.explain(a, b)}")
                    if clashes:
                        a, b = clashes[0]
                        notice = (f"{a.upper()} and {b.upper()} looked the same - "
                                  "press C and exaggerate")
                        notice_until = now + 8
                        print(f"Warning: {notice}")
                    else:
                        notice, notice_until = "Calibrated to your face", now + 3
        elif feats:
            scores = model.scores(feats) if model else rule_scores(bs)
            if tracker.update(scores):
                print(f"mood -> {tracker.mood}")
                sender.send(tracker.mood)
        elif not banner:
            banner = "No face found - face the camera, add light"

        sender.keep_alive(tracker.mood)

        mode = "calibrated" if model else "general rules"
        draw_ui(cv2, frame, tracker, f"{mode}   cam {grabber.fps:.0f} fps",
                face_box, banner)
        cv2.imshow("Emotion tester", frame)

        key = cv2.waitKey(1) & 0xFF
        if key in (ord("q"), 27):
            break
        if key == ord("c") and calib is None:
            calib = {"step": 0, "start": time.monotonic(),
                     "samples": {e: [] for e in EMOTIONS}}
        if key == ord("r"):
            model = None
            CALIB_PATH.unlink(missing_ok=True)
            notice, notice_until = "Calibration cleared", time.monotonic() + 2
        if cv2.getWindowProperty("Emotion tester", cv2.WND_PROP_VISIBLE) < 1:
            break

    grabber.stop()
    cv2.destroyAllWindows()
    landmarker.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
