#!/usr/bin/env python3
"""Person detection for the Smart Guardian System (part 3-a).

This is the single component the project brief allows to be written in Python
("only image processing"). Everything it produces is handed to the C daemon
through shared memory; it makes no decisions of its own about email, MQTT,
storage or the API.

Pipeline
--------
    MJPEG over TCP from the capture host          (the board has no camera, so
        |                                          the brief's laptop-webcam
        v                                          fallback is used)
    JPEG frame boundary scanner
        |
        v
    OpenCV DNN, MobileNet-SSD (Caffe, VOC), class 15 = person
        |
        v
    overlay: boxes, count, student number, date, live clock, measured FPS
        |
        v
    JPEG re-encode -> /dev/shm/guardian_frame

Why MobileNet-SSD: on a Raspberry Pi 3B+ (4x Cortex-A53, no NPU) it is the
only readily available detector that clears one frame per second at a usable
accuracy. Experiment 3-3 characterises the resolution/FPS/accuracy trade-off.
"""

from __future__ import annotations

import argparse
import errno
import os
import queue
import signal
import socket
import sys
import threading
import time
from datetime import datetime

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shmframe  # noqa: E402

# MobileNet-SSD was trained on PASCAL VOC; index 15 is "person".
PERSON_CLASS = 15
CONF_THRESHOLD = 0.45

# The network's native input is 300x300, but MobileNet-SSD is fully
# convolutional so a smaller square can be fed at some cost in accuracy --
# which is exactly the trade-off experiment 3-3 measures. Measured on this
# board (Pi 3B+, OpenCV 4.6, 2 threads):
#
#       300x300 -> 1368 ms      224x224 ->  848 ms
#       256x256 ->  998 ms      192x192 ->  589 ms
#                               160x160 ->  489 ms
#
# The capture resolution barely matters by comparison; the network input is
# what dominates, so the thermal governor (part 4-4) throttles this too.
DEFAULT_NET_INPUT = 300

# Motion gating. A surveillance camera stares at an unchanging scene almost all
# the time, and running a 1.4-second inference on identical frames wastes the
# board's entire CPU budget and heats it for nothing. Frames are compared
# against the last analysed one on a tiny greyscale thumbnail; inference runs
# only when something actually changed, or when the heartbeat expires so a
# departure is still noticed promptly.
DEBUG_CONF = os.environ.get("GUARDIAN_DEBUG_CONF") == "1"

# The alley almost always has a parked car, so vehicles are counted alongside
# people. No extra inference: MobileNet-SSD is a VOC model and already emits
# these classes from the same forward pass -- they were simply being discarded.
#
# Bicycle (2) is deliberately excluded; it is the least reliable of the wheeled
# classes on night IR footage.
VEHICLE_CLASSES = {7: "car", 6: "bus", 14: "motorbike"}

# Vehicles need their own, much lower threshold than people. Measured on this
# camera with the same ROI crop:
#
#     alley, cars parked   0.27-0.37   (92 frames; only 2 ever reached 0.45)
#     yard, no vehicles    0.000       (15 frames, exactly zero every time)
#
# So the person threshold of 0.45 would find a car in ~2% of frames, while the
# noise floor for these classes is zero. 0.25 sits between the two with room on
# both sides. It is still a third of what people score, which is why the count
# needs the hysteresis below to stop it flickering.
VEHICLE_CONF_THRESHOLD = 0.25

# Reject a vehicle box that fills most of the region it was found in. Measured
# on this camera, the detector reliably emits two boxes for one parked car:
#
#     real car          63x73    =  3% of the ROI
#     spurious blob    385x379   = 99% of the ROI, conf 0.48-0.64
#
# The second is the network saying "this whole crop is somewhat car-like", not a
# second vehicle, and at 0.5+ confidence no threshold can separate it from the
# real one -- only its size can. 0.50 sits far from both measurements, so a
# genuinely close car still passes.
#
# Applied to vehicles only. The person path is what the brief grades and it is
# working; adding a size filter there could reject a legitimate close-up and
# there is no measured problem to justify the risk.
VEHICLE_MAX_AREA_FRAC = 0.50

# Contrast-equalise the network's input. MobileNet-SSD was trained on ordinary
# daylight photographs; this camera's night IR image is far flatter, and the
# model reads low-contrast shapes as the wrong class entirely -- the parked
# pickup came back as "chair 0.59" tightly cropped and "sofa 0.37" with context.
#
# Measured over 89 consecutive live frames at night:
#
#     plain   0.00 vehicles/frame   (nothing detected at all)
#     CLAHE   1.01 vehicles/frame
#
# and on the 12 recorded frames containing a person, it is neutral -- 12/12
# detected either way, mean 0.928 vs 0.929 -- so the graded person path is not
# traded away for it. CLAHE rather than a global histogram equalisation because
# the frame is unevenly lit: a streetlight at one end would otherwise crush the
# dark end where people actually appear.
#
# Applied to the 300x300 network input only. The published frame keeps its
# natural appearance, so the operator sees what the camera saw.
ENHANCE_CONTRAST = True
MOTION_THRESHOLD = 2.0      # mean absolute difference, 0..255
MOTION_HEARTBEAT = 3.0      # seconds; re-run inference even in a still scene

RUNNING = True


def log(msg: str) -> None:
    print(f"<6>{datetime.now():%H:%M:%S} vision: {msg}", flush=True)


def warn(msg: str) -> None:
    print(f"<4>{datetime.now():%H:%M:%S} vision: {msg}", flush=True)


def _stop(signum, frame):  # noqa: ARG001
    global RUNNING
    RUNNING = False


def _strip_inline_comment(value: str) -> str:
    """Removes a trailing "  # comment" from a config value.

    The '#' must be preceded by whitespace, so a value that legitimately
    contains one is left alone. Mirrors strip_inline_comment() in src/config.c.
    """
    for i in range(1, len(value)):
        if value[i] == "#" and value[i - 1].isspace():
            return value[:i].rstrip()
    return value


def read_config(path: str) -> dict:
    cfg = {}
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith(("#", ";")) or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                cfg[k.strip()] = _strip_inline_comment(v.strip())
    except OSError as exc:
        warn(f"cannot read {path}: {exc}; using defaults")
    return cfg


# --------------------------------------------------------------- frame source


class MjpegSource:
    """Reads a concatenated-JPEG stream from a TCP client.

    ffmpeg on the capture host connects to us and pushes `-f mjpeg`, which is
    simply JPEG images back to back with no container. Frames are split on the
    SOI/EOI markers (FFD8 / FFD9).

    The listening socket is normally inherited from guardian-ingest.socket
    (systemd socket activation, fd 3) so the port is bound before this process
    even starts; --listen-port is the fallback for running by hand.
    """

    SOI = b"\xff\xd8"
    EOI = b"\xff\xd9"
    MAX_FRAME = 4 * 1024 * 1024

    def __init__(self, listen_fd: int | None, listen_port: int):
        if listen_fd is not None and self._fd_is_socket(listen_fd):
            self.srv = socket.socket(fileno=listen_fd)
            log(f"using the socket-activated listener on fd {listen_fd}")
        else:
            self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.srv.bind(("0.0.0.0", listen_port))
            self.srv.listen(1)
            log(f"listening on 0.0.0.0:{listen_port}")
        self.srv.settimeout(1.0)
        self.conn: socket.socket | None = None
        self.buf = bytearray()

    @staticmethod
    def _fd_is_socket(fd: int) -> bool:
        try:
            os.fstat(fd)
            socket.socket(fileno=os.dup(fd)).close()
            return True
        except (OSError, ValueError):
            return False

    def _accept(self) -> bool:
        try:
            self.conn, addr = self.srv.accept()
            self.conn.settimeout(5.0)
            self.buf.clear()
            log(f"capture host connected from {addr[0]}:{addr[1]}")
            return True
        except socket.timeout:
            return False
        except OSError as exc:
            warn(f"accept failed: {exc}")
            time.sleep(1.0)
            return False

    def _drop(self, why: str) -> None:
        if self.conn:
            warn(f"capture host disconnected ({why})")
            try:
                self.conn.close()
            except OSError:
                pass
        self.conn = None
        self.buf.clear()

    def read_frame(self) -> bytes | None:
        """Returns the next complete JPEG, or None if none is available yet."""
        if self.conn is None:
            if not self._accept():
                return None

        # Serve a frame already sitting in the buffer.
        frame = self._pop_frame()
        if frame is not None:
            return frame

        try:
            chunk = self.conn.recv(65536)
        except socket.timeout:
            return None
        except OSError as exc:
            self._drop(str(exc))
            return None

        if not chunk:
            self._drop("clean close")
            return None

        self.buf.extend(chunk)
        if len(self.buf) > self.MAX_FRAME:
            # Desynchronised: resynchronise on the next SOI rather than growing
            # without bound.
            idx = self.buf.rfind(self.SOI)
            warn("input buffer overflow, resynchronising")
            del self.buf[:idx if idx > 0 else len(self.buf)]
        return self._pop_frame()

    def _pop_frame(self) -> bytes | None:
        start = self.buf.find(self.SOI)
        if start < 0:
            return None
        end = self.buf.find(self.EOI, start + 2)
        if end < 0:
            return None
        end += 2
        frame = bytes(self.buf[start:end])
        del self.buf[:end]
        return frame

    def read_image(self):
        """Decoded BGR frame, or None. The interface both sources share.

        The main loop used to call read_frame() and decode the JPEG itself.
        Moving the decode in here lets an RTSP source -- which never has a JPEG
        in the first place -- present the same interface without inventing one
        just to have it thrown away again.
        """
        jpeg = self.read_frame()
        if jpeg is None:
            return None
        return cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)

    def close(self) -> None:
        self._drop("shutting down")
        try:
            self.srv.close()
        except OSError:
            pass


class RtspSource:
    """Pulls H.264 from an RTSP camera or DVR through OpenCV's FFMPEG backend.

    Used when the board has a real camera on the network instead of a laptop
    pushing its webcam at us. The stream is decoded here and handed on as BGR,
    so nothing downstream changes.

    WHY THERE IS A THREAD IN HERE
    -----------------------------
    This is the part that is easy to get wrong and hard to notice. The DVR
    pushes frames at its own rate (12 fps on the sub-stream); the detector
    consumes at target_fps or slower, and slower still whenever inference is
    running. FFmpeg buffers the difference internally, so a plain cap.read()
    on the main loop hands back the OLDEST queued frame, not the newest -- and
    the backlog grows for as long as the process runs. The picture stays
    perfectly smooth while drifting further and further into the past, which is
    exactly the kind of fault nobody spots until a latency measurement is taken
    weeks later.

    CAP_PROP_BUFFERSIZE=1 is the documented fix and the FFMPEG backend widely
    ignores it, so it cannot be relied on. Instead a daemon thread reads flat
    out -- consuming frames as fast as they arrive, which is what keeps the
    queue empty -- and overwrites a single slot. read_image() returns whatever
    is in the slot right now. Frames are dropped at the source, deliberately,
    which is the same rule the MJPEG path already follows.
    """

    def __init__(self, host: str, port: int, channel: str, user: str,
                 password: str, transport: str = "tcp"):
        # Must be set before the first VideoCapture: OpenCV reads it when it
        # constructs the backend, not per-call. TCP because UDP silently loses
        # slices on a busy network and the artefacts look like camera faults.
        os.environ.setdefault("OPENCV_FFMPEG_CAPTURE_OPTIONS",
                              f"rtsp_transport;{transport}")

        self._url = (f"rtsp://{user}:{password}@{host}:{port}"
                     f"/Streaming/Channels/{channel}")
        # Kept for logging. The password never appears in a log line, a
        # journal entry or an exception message.
        self.safe_url = (f"rtsp://{user}:***@{host}:{port}"
                         f"/Streaming/Channels/{channel}")

        # A Condition rather than a bare Lock, because read_image() has to
        # BLOCK until a frame is actually available. The MJPEG source paces the
        # main loop for free -- its socket read blocks -- and if this one
        # returned None the instant no new frame had arrived, the loop would
        # spin flat out on an empty slot and burn a core for nothing.
        self._cond = threading.Condition()
        self._latest = None          # newest decoded frame
        self._stamp = 0.0            # when it was decoded
        self._seq = 0                # bumped per frame; how a waiter spots a new one
        self._running = True
        self._connected = False
        self._thread = threading.Thread(target=self._pump, name="rtsp",
                                        daemon=True)
        self._thread.start()
        log(f"rtsp source: {self.safe_url} ({transport})")

    def _open(self):
        cap = cv2.VideoCapture(self._url, cv2.CAP_FFMPEG)
        if not cap.isOpened():
            cap.release()
            return None
        # Harmless if ignored, helpful on backends that honour it.
        try:
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        except Exception:                                     # noqa: BLE001
            pass
        return cap

    def _pump(self) -> None:
        backoff = 2.0
        cap = None
        while self._running:
            if cap is None:
                cap = self._open()
                if cap is None:
                    warn(f"rtsp: cannot open {self.safe_url}, "
                         f"retrying in {backoff:.0f}s")
                    self._connected = False
                    time.sleep(backoff)
                    backoff = min(backoff * 2, 30.0)
                    continue
                backoff = 2.0
                log(f"rtsp: connected to {self.safe_url}")
                self._connected = True

            ok, frame = cap.read()
            if not ok or frame is None:
                # A DVR drops connections on its own schedule; treat any read
                # failure as a disconnect and rebuild rather than spinning on a
                # dead handle.
                warn("rtsp: stream ended, reconnecting")
                self._connected = False
                cap.release()
                cap = None
                with self._cond:
                    self._cond.notify_all()      # unblock a waiting reader
                continue

            with self._cond:
                self._latest = frame
                self._stamp = time.monotonic()
                self._seq += 1
                self._cond.notify_all()

        if cap is not None:
            cap.release()

    def read_image(self, timeout: float = 1.0):
        """Block for up to `timeout` for the newest frame; None if none came.

        Two properties matter here:

        * It waits, so the caller is paced by the camera exactly as the MJPEG
          source is paced by its socket. No busy loop.
        * It returns the CURRENT slot, never a queue. If the detector was busy
          for three frame times, the two intermediate frames are gone -- which
          is the point. Serving them would be serving the past.

        None while disconnected, rather than the last good frame: the software
        watchdog decides the camera is gone from the age of the last real
        frame, and quietly re-serving a stale one forever would hide exactly
        the fault it exists to catch.
        """
        with self._cond:
            # The slot is emptied on every read, so "is it empty" is the whole
            # question -- no sequence comparison needed. If a frame is already
            # waiting we take it without sleeping; if not, we block until the
            # pump notifies or the timeout expires.
            if self._latest is None:
                self._cond.wait(timeout)
            if self._latest is None:
                return None
            frame, stamp = self._latest, self._stamp
            self._latest = None          # consume: never serve the same twice
        if time.monotonic() - stamp > 5.0:
            return None                  # decoded, but too old to be truthful
        return frame

    @property
    def connected(self) -> bool:
        return self._connected

    def close(self) -> None:
        self._running = False
        self._thread.join(timeout=3.0)


# ------------------------------------------------------------------- overlay


def draw_overlay(img, boxes, persons, fps, student_id, infer_ms, stream_up,
                 vboxes=()):
    """Boxes, counter, student number, date, live clock and measured FPS.

    The brief requires the student number, the date and the live system time to
    be burned into every output frame, and the real (measured) frame rate to be
    displayed -- so `fps` here is derived from actual frame intervals, never
    from the configured target.

    `vboxes` are vehicles. They default to empty so any caller that predates
    them keeps working unchanged.
    """
    h, w = img.shape[:2]

    def box(x1, y1, x2, y2, text, colour):
        cv2.rectangle(img, (x1, y1), (x2, y2), colour, 2)
        (tw, th), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 0.45, 1)
        cv2.rectangle(img, (x1, max(0, y1 - th - 6)), (x1 + tw + 6, y1),
                      colour, -1)
        cv2.putText(img, text, (x1 + 3, max(10, y1 - 4)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 0, 0), 1, cv2.LINE_AA)

    # Vehicles first, so a person standing against a car is drawn on top --
    # people are what the system is actually for.
    for (x1, y1, x2, y2, conf, kind) in vboxes:
        box(x1, y1, x2, y2, f"{kind} {conf:.2f}", (60, 180, 255))   # amber, BGR

    for (x1, y1, x2, y2, conf) in boxes:
        box(x1, y1, x2, y2, f"person {conf:.2f}", (0, 220, 60))     # green

    # Translucent banner so the text stays legible over any scene.
    bar_h = 26
    strip = img[0:bar_h, 0:w]
    cv2.addWeighted(strip, 0.35, np.zeros_like(strip), 0.65, 0, strip)
    cv2.rectangle(img, (0, h - bar_h), (w, h), (0, 0, 0), -1)

    now = datetime.now()
    colour = (60, 220, 60) if persons == 0 else (60, 200, 255)
    cv2.putText(img, f"PERSONS: {persons}", (8, 18),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, colour, 2, cv2.LINE_AA)
    if vboxes:
        # Same amber as the vehicle boxes, so the number and the rectangles are
        # obviously the same thing.
        cv2.putText(img, f"VEH: {len(vboxes)}", (150, 18),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, (60, 180, 255), 2,
                    cv2.LINE_AA)
    cv2.putText(img, f"FPS {fps:5.2f}", (w - 210, 18),
                cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1, cv2.LINE_AA)
    cv2.putText(img, f"{infer_ms:4.0f} ms", (w - 92, 18),
                cv2.FONT_HERSHEY_SIMPLEX, 0.55, (200, 200, 200), 1, cv2.LINE_AA)

    cv2.putText(img, student_id, (8, h - 8),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 200, 255), 2, cv2.LINE_AA)
    cv2.putText(img, now.strftime("%Y-%m-%d %H:%M:%S"), (w - 250, h - 8),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 1, cv2.LINE_AA)
    if not stream_up:
        cv2.putText(img, "NO SIGNAL", (w // 2 - 90, h // 2),
                    cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 0, 255), 2, cv2.LINE_AA)
    return img


def placeholder(width, height, msg):
    img = np.full((height, width, 3), 24, dtype=np.uint8)
    cv2.putText(img, msg, (20, height // 2),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (90, 90, 200), 2, cv2.LINE_AA)
    return img


# ---------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(description="Guardian person detector")
    ap.add_argument("--config", default="/etc/guardian/guardian.conf")
    ap.add_argument("--model-dir", default="/opt/guardian/models")
    ap.add_argument("--listen-fd", type=int, default=None,
                    help="inherited listening socket (systemd socket activation)")
    ap.add_argument("--listen-port", type=int, default=9000)
    ap.add_argument("--jpeg-quality", type=int, default=75)
    ap.add_argument("--no-detect", action="store_true",
                    help="pass frames through without inference (experiment 2-1b)")
    args = ap.parse_args()

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    cfg = read_config(args.config)
    student_id = cfg.get("student_id", "402170516")
    target_fps = int(cfg.get("target_fps", 10))
    target_width = int(cfg.get("target_width", 640))
    net_input = int(cfg.get("target_net_input", DEFAULT_NET_INPUT))

    # Thread count for the DNN. Two used to be the optimum and no longer is.
    #
    # The original measurement was taken when inference ran INLINE in the frame
    # loop, so every extra DNN thread competed with decoding, overlaying and
    # publishing on the same four cores and the gains cancelled out:
    #   1t 2573 ms | 2t 1418 ms | 3t 1521 ms | 4t 1492 ms
    #
    # Once inference moved to its own worker the picture inverted, because the
    # frame path now spends most of its time waiting on I/O rather than
    # competing. Re-measured on this board, same model, same image:
    #   300 px:  1t 2491 | 2t 1551 | 3t 1212 | 4t 1088 ms
    #   224 px:  1t 1411 | 2t  809 | 3t  693 | 4t  560 ms
    #   160 px:  1t  752 | 2t  414 | 3t  346 | 4t  317 ms
    #
    # And yet the default here is still 2, because the microbenchmark above is
    # measured in isolation and the board is not. Deploying 4 threads made the
    # system WORSE: inference finishing sooner means more inferences per second,
    # which raised sustained CPU enough to cross the thermal governor's setpoint.
    # The governor then cut the frame rate to 2 fps and the capture width to
    # 320 px, so the "30% faster" change produced a slower, uglier stream.
    #
    # The lesson is that on a thermally-limited board the useful quantity is
    # work per degree, not milliseconds per inference. Left configurable: with
    # decoding moved off the board (camera_source=socket + mac/stream_dvr.sh)
    # there is now real headroom, so raising this is worth re-measuring -- but
    # it must be judged on sustained temperature, not a benchmark loop.
    cv2.setNumThreads(int(cfg.get("cv_threads", os.environ.get("GUARDIAN_CV_THREADS", "2"))))

    # --- model ---
    proto = os.path.join(args.model_dir, "MobileNetSSD_deploy.prototxt")
    weights = os.path.join(args.model_dir, "MobileNetSSD_deploy.caffemodel")
    net = None
    if not args.no_detect:
        if not (os.path.exists(proto) and os.path.exists(weights)):
            warn(f"model missing in {args.model_dir}; run scripts/fetch_model.sh")
            return 1
        t0 = time.monotonic()
        net = cv2.dnn.readNetFromCaffe(proto, weights)
        net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
        net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)
        log(f"MobileNet-SSD loaded in {time.monotonic() - t0:.1f}s")

    # --- shared memory (created by the C daemon) ---
    for attempt in range(30):
        try:
            shm = shmframe.ShmWriter()
            break
        except (OSError, RuntimeError) as exc:
            if attempt == 29:
                warn(f"cannot attach to shared memory: {exc}")
                return 1
            time.sleep(1.0)
    shm.set_start_ts(time.time())
    log("attached to the shared frame segment")

    # --- inference worker ------------------------------------------------
    # One frame in flight at a time, and always the newest one: a deeper queue
    # would only build latency, since a frame that has been waiting is already
    # worthless next to the one that just arrived. infer_busy is what makes the
    # main loop skip submission while a pass is running, so frames are dropped
    # at the gate rather than piling up behind it.
    infer_q = queue.Queue(maxsize=1)
    infer_lock = threading.Lock()
    infer_busy = threading.Event()
    infer_state = {"boxes": [], "vboxes": [], "ms": 0.0, "count": 0}

    detect_vehicles = str(cfg.get("detect_vehicles", "true")).strip().lower() \
        not in ("0", "false", "no", "off")
    try:
        vehicle_threshold = float(cfg.get("vehicle_conf_threshold",
                                          VEHICLE_CONF_THRESHOLD))
    except ValueError:
        vehicle_threshold = VEHICLE_CONF_THRESHOLD
    try:
        vehicle_hold = max(0, int(cfg.get("vehicle_hold_frames", 8)))
    except ValueError:
        vehicle_hold = 8

    enhance = str(cfg.get("enhance_contrast", str(ENHANCE_CONTRAST))).strip().lower() \
        not in ("0", "false", "no", "off")
    clahe = cv2.createCLAHE(clipLimit=3.0, tileGridSize=(8, 8)) if enhance else None

    def infer_worker():
        while RUNNING:
            try:
                item = infer_q.get(timeout=0.5)
            except queue.Empty:
                continue
            if item is None:
                break
            small, net_in, fw, fh, roi = item
            rx, ry, rw, rh = roi
            try:
                t0 = time.monotonic()
                if clahe is not None:
                    # On the already-resized 300x300 input, so it costs a couple
                    # of milliseconds against a ~1300 ms forward pass, and it
                    # runs here rather than on the capture loop.
                    lab = cv2.cvtColor(small, cv2.COLOR_BGR2LAB)
                    l, a, bch = cv2.split(lab)
                    small = cv2.cvtColor(cv2.merge((clahe.apply(l), a, bch)),
                                         cv2.COLOR_LAB2BGR)
                blob = cv2.dnn.blobFromImage(
                    small,
                    scalefactor=0.007843,       # 2/255, the model's scaling
                    size=(net_in, net_in),
                    mean=127.5,
                )
                net.setInput(blob)
                det = net.forward()
                ms = (time.monotonic() - t0) * 1000.0

                # Unthresholded best person score, for diagnosing "it did not
                # see me" reports. Without this the only observable is the
                # thresholded count, which cannot distinguish "scored 0.44 and
                # was rejected" from "scored nothing at all" -- two problems
                # with completely different fixes.
                best_p = 0.0
                if DEBUG_CONF:
                    best_p = max([float(det[0, 0, i, 2])
                                  for i in range(det.shape[2])
                                  if int(det[0, 0, i, 1]) == PERSON_CLASS] + [0.0])

                # People and vehicles are collected into separate lists rather
                # than one tagged list. The person count is what the brief
                # grades, so its path stays byte-for-byte what it was; vehicles
                # ride alongside and cannot perturb it.
                found = []
                vfound = []
                for i in range(det.shape[2]):
                    conf = float(det[0, 0, i, 2])
                    cls = int(det[0, 0, i, 1])
                    if cls == PERSON_CLASS:
                        if conf < CONF_THRESHOLD:
                            continue
                        label = None
                    elif detect_vehicles and cls in VEHICLE_CLASSES:
                        if conf < vehicle_threshold:
                            continue
                        label = VEHICLE_CLASSES[cls]
                    else:
                        continue
                    # The network's outputs are fractions of the ROI it was
                    # given, so they scale by the ROI's size and shift by its
                    # origin. Getting this wrong draws boxes in the right shape
                    # at the wrong place, which looks like a detection bug
                    # rather than a coordinate bug.
                    x1 = max(0, int(det[0, 0, i, 3] * rw) + rx)
                    y1 = max(0, int(det[0, 0, i, 4] * rh) + ry)
                    x2 = min(fw - 1, int(det[0, 0, i, 5] * rw) + rx)
                    y2 = min(fh - 1, int(det[0, 0, i, 6] * rh) + ry)
                    if x2 <= x1 or y2 <= y1:
                        continue
                    if label is None:
                        found.append((x1, y1, x2, y2, conf))
                    elif (x2 - x1) * (y2 - y1) <= VEHICLE_MAX_AREA_FRAC * rw * rh:
                        vfound.append((x1, y1, x2, y2, conf, label))
                    elif DEBUG_CONF:
                        log(f"debug: dropped oversized {label} "
                            f"{x2 - x1}x{y2 - y1} "
                            f"({100.0 * (x2 - x1) * (y2 - y1) / (rw * rh):.0f}% "
                            f"of ROI) conf={conf:.2f}")

                geom_ok = (fw, fh) == (last_frame_wh[0], last_frame_wh[1])
                if DEBUG_CONF:
                    best_v = max([float(det[0, 0, i, 2]) for i in range(det.shape[2])
                                  if int(det[0, 0, i, 1]) in VEHICLE_CLASSES] + [0.0])
                    log(f"debug: roi={roi} best_person={best_p:.3f} "
                        f"best_vehicle={best_v:.3f} kept={len(found)}p/{len(vfound)}v "
                        f"geom_ok={geom_ok} "
                        f"worker_wh=({fw},{fh}) main_wh={tuple(last_frame_wh)} "
                        f"boxes={found[:2]}")

                with infer_lock:
                    # Boxes are absolute pixels against the frame the worker
                    # saw. If the capture width changed underneath us they no
                    # longer apply anywhere, so drop them rather than draw a
                    # box in the wrong place.
                    infer_state["boxes"] = found if geom_ok else []
                    infer_state["vboxes"] = vfound if geom_ok else []
                    infer_state["ms"] = ms
                    infer_state["count"] += 1
            except Exception as exc:                       # noqa: BLE001
                warn(f"inference failed: {exc}")
            finally:
                infer_busy.clear()

    last_frame_wh = [0, 0]
    infer_thread = threading.Thread(target=infer_worker, name="infer", daemon=True)
    infer_thread.start()

    # --- detection region of interest ------------------------------------
    #
    # MobileNet-SSD takes a SQUARE input, so handing it a 16:9 frame stretches
    # every person horizontally by 1.78x. Measured on 12 recorded frames of a
    # person standing 42% of frame height away from this camera:
    #
    #   whole frame stretched to 300px   mean conf 0.000    0/12 detected
    #   whole frame letterboxed to 300   mean conf 0.000    0/12 detected
    #   square crop of the alley -> 300  mean conf 0.806   12/12 detected
    #   whole frame stretched to 512px   mean conf 0.056    0/12 detected
    #
    # So this is not a resolution problem -- 512px barely moved it, and the
    # subject was already 125px tall in the network. Two things had to be true
    # together: correct proportions AND enough pixels. Letterboxing fixes the
    # aspect but shrinks the person to ~70px; the square crop fixes the aspect
    # AND spends all 300px on the half of the frame people actually walk in
    # (the left 44% of this view is our own wall, rooftop and plants).
    #
    # Cost is unchanged: 300x300 either way. Detection runs on the crop while
    # the stream still shows the whole scene, with boxes mapped back.
    #
    # roi = "auto" takes the square nearest the configured edge; an explicit
    # "x,y,w,h" in capture-frame pixels overrides it for a differently framed
    # camera.
    roi_cfg = str(cfg.get("detect_roi", "auto")).strip().lower()
    roi_align = str(cfg.get("detect_roi_align", "right")).strip().lower()

    def roi_for(fw, fh):
        if roi_cfg not in ("auto", ""):
            try:
                x, y, rw, rh = (int(v) for v in roi_cfg.split(","))
                x = max(0, min(x, fw - 1)); y = max(0, min(y, fh - 1))
                return x, y, max(1, min(rw, fw - x)), max(1, min(rh, fh - y))
            except ValueError:
                pass                      # fall through to auto on a bad value
        side = min(fw, fh)                # the largest undistorted square
        if roi_align == "left":
            x = 0
        elif roi_align == "center":
            x = (fw - side) // 2
        else:
            x = fw - side                 # right: the alley side of this view
        return x, (fh - side) // 2, side, side

    # --- frame source ------------------------------------------------------
    # Two ways in, chosen by configuration so switching back is a one-line edit
    # and the already-measured webcam experiments stay reproducible:
    #
    #   socket -- a capture host pushes MJPEG at guardian-ingest.socket. The
    #             brief's fallback for a board with no camera of its own.
    #   rtsp   -- the board pulls H.264 from a camera or DVR on the LAN. No
    #             laptop in the loop at all.
    cam_source = cfg.get("camera_source", "socket").strip().lower()

    if cam_source == "rtsp":
        cam_pass = os.environ.get("GUARDIAN_CAM_PASS", "")
        if not cam_pass:
            # Refuse rather than fall back silently: dropping to the socket
            # path here would leave a "working" system quietly watching the
            # wrong camera, which is worse than not starting.
            warn("camera_source=rtsp but GUARDIAN_CAM_PASS is not set "
                 "(EnvironmentFile=/etc/guardian/secrets.env missing from the unit?)")
            return 1
        src = RtspSource(
            host=cfg.get("camera_host", "192.168.100.64"),
            port=int(cfg.get("camera_rtsp_port", 554)),
            channel=str(cfg.get("camera_channel", "202")),
            user=cfg.get("camera_user", "admin"),
            password=cam_pass,
            transport=cfg.get("camera_transport", "tcp"),
        )
    else:
        src = MjpegSource(args.listen_fd, args.listen_port)

    # --- loop state ---
    frame_times: list[float] = []
    fps_measured = 0.0
    last_emit = 0.0
    last_ctrl = -1
    idle_notified = False
    frames_seen = 0
    encode_params = [int(cv2.IMWRITE_JPEG_QUALITY), args.jpeg_quality]

    # Motion gating state: the thumbnail of the last analysed frame, the boxes
    # it produced, and when that analysis happened.
    prev_thumb = None
    last_boxes: list = []
    last_vboxes: list = []
    vboxes: list = []
    vehicle_miss = 0
    last_infer_ms = 0.0
    last_infer_at = 0.0
    inferences = 0
    gated_frames = 0

    log(f"detector running: capture {target_width}px @ {target_fps} fps, "
        f"network input {net_input}px, {cv2.getNumThreads()} threads")
    if detect_vehicles:
        log(f"vehicles: {'/'.join(sorted(set(VEHICLE_CLASSES.values())))} "
            f"at conf >= {vehicle_threshold}, held {vehicle_hold} frames "
            f"(counted and drawn only -- never mailed or alarmed)")
    else:
        log("vehicles: disabled by config")
    log(f"contrast equalisation on the network input: "
        f"{'CLAHE' if clahe is not None else 'off'}")

    while RUNNING:
        # Pick up throttling decisions from the C thermal governor.
        seq = shm.ctrl_seq()
        if seq != last_ctrl:
            fps_c, width_c, net_c = shm.targets()
            if fps_c > 0:
                target_fps = fps_c
            if width_c > 0:
                target_width = width_c
            # net_c < 0 is SHMFRAME_NET_OFF: relay and overlay, but run no
            # inference. 0 still means "unchanged", matching the C setter.
            if net_c != 0 and net_c != net_input:
                net_input = net_c
                prev_thumb = None      # thumbnails are not comparable across
                                       # a resolution change
                # Any in-flight result belongs to the old geometry.
                with infer_lock:
                    infer_state["boxes"] = []
            if last_ctrl >= 0:
                net_desc = f"{net_input}px" if net_input > 0 else "detection off"
                log(f"control update: {target_fps} fps, capture {target_width}px, "
                    f"network input {net_desc}")
            last_ctrl = seq

        img_in = src.read_image()

        if img_in is None:
            # No signal. Keep publishing a placeholder at 1 Hz so the dashboard
            # and the MJPEG stream stay alive instead of freezing on the last
            # good image. These frames are flagged synthetic, so the C
            # watchdog still sees the pipeline as stale and acts on it
            # (part 4-3) and the detection consumer ignores their zero count.
            now = time.monotonic()
            if now - last_emit > 1.0:
                if not idle_notified:
                    warn("no frames from the capture host")
                    idle_notified = True
                img = placeholder(target_width, int(target_width * 3 / 4),
                                  "waiting for the capture host")
                img = draw_overlay(img, [], 0, 0.0, student_id, 0.0, False)
                ok, enc = cv2.imencode(".jpg", img, encode_params)
                if ok:
                    shm.publish(enc.tobytes(), 0, time.time(), time.monotonic(),
                                0.0, img.shape[1], img.shape[0], 0.0,
                                synthetic=True)
                last_emit = now
            time.sleep(0.05)
            continue

        idle_notified = False

        # Rate limiting: drop frames rather than queue them, so the pipeline
        # always works on the freshest image and never builds latency.
        now = time.monotonic()
        min_interval = 1.0 / max(1, target_fps)
        if now - last_emit < min_interval:
            continue

        # `ts_wall` is the capture instant. Experiment 3-5 subtracts it from
        # the laptop's MQTT receive time, so it is stamped here -- as early as
        # possible -- and carried through unchanged.
        ts_wall = time.time()
        ts_mono = now

        # Already decoded by the source: MjpegSource does the imdecode, the
        # RTSP source never has a JPEG to begin with.
        img = img_in

        if img.shape[1] != target_width:
            scale = target_width / img.shape[1]
            img = cv2.resize(img, (target_width, int(img.shape[0] * scale)),
                             interpolation=cv2.INTER_AREA)

        h, w = img.shape[:2]
        # Published for the worker so it can tell whether the boxes it just
        # produced still describe the frame geometry being drawn.
        last_frame_wh[0], last_frame_wh[1] = w, h
        boxes = last_boxes
        infer_ms = last_infer_ms

        # net_input <= 0 is the "relay only" mode (SHMFRAME_NET_OFF): frames are
        # still decoded, overlaid and published, but nothing is inferred. It
        # isolates the cost of moving pixels from the cost of the network for
        # experiment 2-1, and is the thermal governor's last resort.
        detect_on = net is not None and net_input > 0
        if not detect_on:
            boxes = []
            vboxes = []
            last_vboxes = []
            infer_ms = 0.0

        if detect_on:
            # --- motion gate ---
            thumb = cv2.resize(cv2.cvtColor(img, cv2.COLOR_BGR2GRAY), (64, 48),
                               interpolation=cv2.INTER_AREA)
            if prev_thumb is None:
                moved, motion = True, 255.0
            else:
                motion = float(cv2.absdiff(thumb, prev_thumb).mean())
                moved = motion >= MOTION_THRESHOLD
            stale = (now - last_infer_at) >= MOTION_HEARTBEAT

            # Hand the frame to the inference thread instead of running the
            # network here. Doing it inline stalled this loop for the whole
            # ~1.25 s of a forward pass, and nothing was published meanwhile, so
            # the stream ran at 5 fps with an empty room and collapsed to 1.5 fps
            # the moment somebody walked in -- precisely when the motion gate
            # stopped skipping. Detection is not made any faster by this; the
            # frame path simply stops waiting for it.
            if (moved or stale) and not infer_busy.is_set():
                prev_thumb = thumb
                last_infer_at = now
                infer_busy.set()
                rx, ry, rw, rh = roi_for(w, h)
                try:
                    infer_q.put_nowait((
                        cv2.resize(img[ry:ry + rh, rx:rx + rw],
                                   (net_input, net_input)),
                        net_input, w, h, (rx, ry, rw, rh)))
                except queue.Full:
                    infer_busy.clear()
            else:
                # Either the scene is unchanged or the worker is still busy;
                # this frame costs only the clock and FPS redraw.
                gated_frames += 1

            # Newest completed result. The boxes belong to a frame captured up
            # to one inference ago, so on a moving subject the box trails the
            # body slightly. That lag was already present before -- results were
            # reused between inferences either way -- and a trailing box on
            # smooth video beats a correctly-placed box on a 1.5 fps slideshow.
            with infer_lock:
                boxes = infer_state["boxes"]
                vboxes_raw = infer_state["vboxes"]
                infer_ms = infer_state["ms"]
                inferences = infer_state["count"]
                last_boxes = boxes
                last_infer_ms = infer_ms

            # Hysteresis, vehicles only. They score 0.27-0.33 against a 0.25
            # threshold, so a stationary car crosses the line back and forth and
            # the raw count blinks between 0 and 1. Hold the last non-empty
            # result for a few inferences instead of dropping it immediately.
            #
            # People deliberately do NOT get this. It would delay the count the
            # brief grades and that experiment 3-5 times, and people score high
            # enough (0.66-0.88) not to need it.
            if vboxes_raw:
                last_vboxes = vboxes_raw
                vehicle_miss = 0
            elif last_vboxes:
                vehicle_miss += 1
                if vehicle_miss > vehicle_hold:
                    last_vboxes = []
            vboxes = last_vboxes

        persons = len(boxes)
        vehicles = len(vboxes)

        # Measured FPS: a rolling window over real frame intervals.
        frame_times.append(now)
        cutoff = now - 5.0
        while frame_times and frame_times[0] < cutoff:
            frame_times.pop(0)
        if len(frame_times) >= 2:
            span = frame_times[-1] - frame_times[0]
            fps_measured = (len(frame_times) - 1) / span if span > 0 else 0.0

        img = draw_overlay(img, boxes, persons, fps_measured, student_id,
                           infer_ms, True, vboxes=vboxes)

        ok, enc = cv2.imencode(".jpg", img, encode_params)
        if not ok:
            continue

        # People first and capped separately, so a frame full of parked cars can
        # never crowd a person out of the detection list.
        det_items = [
            f'{{"x":{b[0]},"y":{b[1]},"w":{b[2] - b[0]},"h":{b[3] - b[1]},'
            f'"conf":{b[4]:.2f},"cls":"person"}}' for b in boxes[:12]
        ] + [
            f'{{"x":{b[0]},"y":{b[1]},"w":{b[2] - b[0]},"h":{b[3] - b[1]},'
            f'"conf":{b[4]:.2f},"cls":"{b[5]}"}}' for b in vboxes[:8]
        ]
        det_json = "[" + ",".join(det_items) + "]"

        shm.publish(enc.tobytes(), persons, ts_wall, ts_mono, fps_measured,
                    w, h, infer_ms, det_json.encode(), vehicles=vehicles)

        last_emit = now
        frames_seen += 1
        if frames_seen == 1:
            log(f"first frame published ({w}x{h}, {len(enc)} bytes)")
        elif frames_seen % 300 == 0:
            saved = 100.0 * gated_frames / max(1, frames_seen)
            log(f"{frames_seen} frames, {fps_measured:.2f} fps, "
                f"{inferences} inferences ({saved:.0f}% gated out), "
                f"{last_infer_ms:.0f} ms each, {persons} person(s)")

    log("stopping")
    src.close()
    shm.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
