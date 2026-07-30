"""Python mirror of src/shmframe.h -- the writer side of the frame transport.

The C daemon creates and owns /dev/shm/guardian_frame; this module attaches to
it and publishes annotated frames into it. The binary layout below must match
the C structs byte for byte, so both sides carry the same offsets as explicit
constants and this module verifies the segment size on attach. The C header
holds equivalent _Static_asserts, which means a layout change breaks the build
on one side and raises on the other rather than silently corrupting frames.

Concurrency is a seqlock over a double buffer with exactly one writer (this
process): bump the slot's sequence to an odd value, fill the slot, bump it to
the next even value, then publish the slot index in the header. A reader that
sees an odd sequence, or a sequence that changed under it, retries.
"""

from __future__ import annotations

import mmap
import os
import struct

SHM_NAME = "guardian_frame"
SHM_PATH = "/dev/shm/" + SHM_NAME

MAGIC = 0x31415547  # "GUA1"
VERSION = 1

JPEG_MAX = 256 * 1024
DETJSON_MAX = 1024
NSLOTS = 2

# ---------------------------------------------------------------- layout ----
# struct guardian_shm (header, 64 bytes)
HDR_MAGIC = 0
HDR_VERSION = 4
HDR_ACTIVE = 8
HDR_TARGET_FPS = 12
HDR_TARGET_WIDTH = 16
HDR_CTRL_SEQ = 20
HDR_VISION_PID = 24
HDR_VISION_START = 32
HDR_FRAMES_TOTAL = 40
HDR_LAST_REAL_TS = 48
HDR_TARGET_NET = 56
HDR_SIZE = 64

# slot flags
FRAME_SYNTHETIC = 0x1  # placeholder frame, no real capture input

# struct guardian_slot (64-byte header, then det[], then jpeg[])
SLOT_SEQ = 0
SLOT_TS_WALL = 8
SLOT_TS_MONO = 16
SLOT_JPEG_LEN = 24
SLOT_PERSONS = 28
SLOT_FPS = 32
SLOT_WIDTH = 36
SLOT_HEIGHT = 40
SLOT_INFER_MS = 44
SLOT_DET_LEN = 48
SLOT_FLAGS = 52
# Vehicle count. Occupies the first word of the slot header's existing padding
# (_pad0[2] at 56..63 in shmframe.h), so the slot size and every other offset
# are unchanged -- a reader built before this field simply ignores those bytes.
SLOT_VEHICLES = 56
SLOT_DET = 64
SLOT_JPEG = SLOT_DET + DETJSON_MAX
SLOT_SIZE = SLOT_JPEG + JPEG_MAX

SHM_SIZE = HDR_SIZE + NSLOTS * SLOT_SIZE

_U32 = struct.Struct("<I")
_I32 = struct.Struct("<i")
_U64 = struct.Struct("<Q")
_F64 = struct.Struct("<d")
_F32 = struct.Struct("<f")


class ShmWriter:
    """Attaches to the segment created by the C daemon and publishes frames."""

    def __init__(self, path: str = SHM_PATH):
        self.path = path
        self.fd = os.open(path, os.O_RDWR)
        size = os.fstat(self.fd).st_size
        if size < SHM_SIZE:
            os.close(self.fd)
            raise RuntimeError(
                f"{path} is {size} bytes, expected at least {SHM_SIZE}; "
                "the C daemon and vision layouts disagree"
            )
        self.mm = mmap.mmap(self.fd, SHM_SIZE, mmap.MAP_SHARED,
                            mmap.PROT_READ | mmap.PROT_WRITE)

        magic = _U32.unpack_from(self.mm, HDR_MAGIC)[0]
        if magic != MAGIC:
            self.close()
            raise RuntimeError(
                f"bad magic 0x{magic:08x} in {path}; start guardian.service first"
            )

        self._slot = 0
        self._seq = [0, 0]
        _I32.pack_into(self.mm, HDR_VISION_PID, os.getpid())

    # -------------------------------------------------------------- control
    def targets(self) -> tuple[int, int, int]:
        """(target_fps, target_width, target_net_input) from the C daemon."""
        fps = _I32.unpack_from(self.mm, HDR_TARGET_FPS)[0]
        width = _I32.unpack_from(self.mm, HDR_TARGET_WIDTH)[0]
        net = _I32.unpack_from(self.mm, HDR_TARGET_NET)[0]
        return fps, width, net

    def ctrl_seq(self) -> int:
        return _I32.unpack_from(self.mm, HDR_CTRL_SEQ)[0]

    def set_start_ts(self, ts: float) -> None:
        _F64.pack_into(self.mm, HDR_VISION_START, ts)

    # ---------------------------------------------------------------- write
    def publish(self, jpeg: bytes, persons: int, ts_wall: float, ts_mono: float,
                fps: float, width: int, height: int, infer_ms: float,
                det_json: bytes = b"[]", synthetic: bool = False,
                vehicles: int = 0) -> bool:
        """Publish one annotated frame. Returns False if the JPEG is too big.

        `synthetic=True` marks a placeholder produced while no capture host is
        connected: readers still get an image for the dashboard, but the C
        watchdog and the detection consumer both ignore it.
        """
        if len(jpeg) > JPEG_MAX:
            return False
        if len(det_json) >= DETJSON_MAX:
            det_json = b"[]"

        idx = 1 - self._slot          # write into the slot nobody is reading
        base = HDR_SIZE + idx * SLOT_SIZE

        # --- seqlock: mark the slot as being written (odd sequence) ---
        self._seq[idx] += 1
        _U64.pack_into(self.mm, base + SLOT_SEQ, self._seq[idx])

        _F64.pack_into(self.mm, base + SLOT_TS_WALL, ts_wall)
        _F64.pack_into(self.mm, base + SLOT_TS_MONO, ts_mono)
        _U32.pack_into(self.mm, base + SLOT_JPEG_LEN, len(jpeg))
        _I32.pack_into(self.mm, base + SLOT_PERSONS, persons)
        _F32.pack_into(self.mm, base + SLOT_FPS, fps)
        _I32.pack_into(self.mm, base + SLOT_WIDTH, width)
        _I32.pack_into(self.mm, base + SLOT_HEIGHT, height)
        _F32.pack_into(self.mm, base + SLOT_INFER_MS, infer_ms)
        _U32.pack_into(self.mm, base + SLOT_DET_LEN, len(det_json))
        _U32.pack_into(self.mm, base + SLOT_FLAGS,
                       FRAME_SYNTHETIC if synthetic else 0)
        _I32.pack_into(self.mm, base + SLOT_VEHICLES, vehicles)

        self.mm[base + SLOT_DET:base + SLOT_DET + len(det_json)] = det_json
        self.mm[base + SLOT_JPEG:base + SLOT_JPEG + len(jpeg)] = jpeg

        # --- seqlock: slot is complete (even sequence) ---
        self._seq[idx] += 1
        _U64.pack_into(self.mm, base + SLOT_SEQ, self._seq[idx])

        # Only now does the slot become the one readers should look at.
        _U32.pack_into(self.mm, HDR_ACTIVE, idx)
        self._slot = idx

        total = _U64.unpack_from(self.mm, HDR_FRAMES_TOTAL)[0] + 1
        _U64.pack_into(self.mm, HDR_FRAMES_TOTAL, total)

        # The watchdog's clock: only real capture input advances it.
        if not synthetic:
            _F64.pack_into(self.mm, HDR_LAST_REAL_TS, ts_mono)
        return True

    # ---------------------------------------------------------------- close
    def close(self) -> None:
        try:
            if getattr(self, "mm", None):
                self.mm.close()
        finally:
            if getattr(self, "fd", None) is not None:
                os.close(self.fd)
                self.fd = None
