#!/usr/bin/env python3
"""Swagger/OpenAPI documentation layer for the Smart Guardian System (part 2-b).

The project brief permits a *thin* FastAPI layer purely as a documenter and API
gateway, on the explicit condition that all real logic -- reading temperature
and memory, counting people, executing commands -- lives in the C program.

This module honours that literally: it contains no business logic at all. Every
route forwards to the C daemon's loopback listener (127.0.0.1:8081) and returns
whatever comes back. The response models below exist only so Swagger UI renders
a useful schema; they never construct or alter a value.

Run by guardian-api.service on :8443 with the same self-signed certificate the
C server uses, so Swagger UI is reachable at

    https://<board>:8443/docs
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import Any, Dict, List, Optional

import httpx
import uvicorn
from fastapi import Body, Depends, FastAPI, HTTPException, Query, Request
from fastapi.responses import JSONResponse, StreamingResponse
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from pydantic import BaseModel, Field

# --------------------------------------------------------------------- config


def _strip_inline_comment(value: str) -> str:
    """Removes a trailing "  # comment" from a config value.

    The '#' must be preceded by whitespace, so a value that legitimately
    contains one is left alone. Mirrors strip_inline_comment() in src/config.c.
    """
    for i in range(1, len(value)):
        if value[i] == "#" and value[i - 1].isspace():
            return value[:i].rstrip()
    return value


def read_config(path: str) -> Dict[str, str]:
    cfg: Dict[str, str] = {}
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith(("#", ";")) or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                cfg[k.strip()] = _strip_inline_comment(v.strip())
    except OSError:
        pass
    return cfg


CFG = read_config(os.environ.get("GUARDIAN_CONF", "/etc/guardian/guardian.conf"))
STUDENT_ID = CFG.get("student_id", "402170516")
STUDENT_NAME = CFG.get("student_name", "Mahan Majlesi")
UPSTREAM = f"http://127.0.0.1:{CFG.get('loopback_port', '8081')}"

# ------------------------------------------------------------------- schemas
# Documentation only -- pydantic v1 is what Ubuntu 24.04 ships.


class Persons(BaseModel):
    persons: Optional[int] = Field(None, description="People visible right now")
    timestamp: str = Field(..., description="ISO-8601 UTC capture time")
    epoch: float = Field(..., description="Capture time as a Unix timestamp")
    student_id: str
    fps: Optional[float] = Field(None, description="Measured detector frame rate")
    frame_age_sec: Optional[float] = None
    resolution: Optional[str] = None
    inference_ms: Optional[float] = None
    guard_mode: Optional[bool] = None
    detections: Optional[List[Dict[str, Any]]] = Field(
        None, description="Bounding boxes for the current frame")
    available: bool

    class Config:
        schema_extra = {
            "example": {
                "persons": 2, "timestamp": "2026-07-27T04:31:12.482Z",
                "epoch": 1785119472.482, "student_id": "402170516",
                "fps": 2.85, "frame_age_sec": 0.31, "resolution": "640x480",
                "inference_ms": 288.0, "guard_mode": False,
                "detections": [{"x": 210, "y": 96, "w": 130, "h": 300,
                                "conf": 0.91}],
                "available": True,
            }
        }


class Telemetry(BaseModel):
    student_id: str
    timestamp: str
    epoch: float
    cpu_temp_c: float = Field(..., description="Read from /sys/class/thermal/thermal_zone0/temp")
    cpu_percent: float = Field(..., description="Derived from /proc/stat deltas")
    cpu_percent_per_core: List[float]
    cpu_freq_khz: int
    cpu_cores: int
    mem_total_kb: int = Field(..., description="Read from /proc/meminfo")
    mem_free_kb: int
    mem_available_kb: int
    mem_buffers_kb: int
    mem_cached_kb: int
    mem_used_percent: float
    swap_total_kb: int
    swap_free_kb: int
    load1: float
    load5: float
    load15: float
    uptime_sec: float
    daemon_rss_kb: int = Field(..., description="The C daemon's own resident set")
    thermal_level: int = Field(..., description="0 normal, 1-3 throttled")
    target_fps: int
    target_width: int
    target_net_input: int
    guard_mode: bool
    mqtt_connected: bool


class HistoryRecord(BaseModel):
    id: int
    timestamp: str
    epoch: float
    persons: int
    cpu_temp_c: float
    fps: float
    guard_mode: bool
    emailed: bool
    note: str


class History(BaseModel):
    student_id: str
    limit: int
    records: List[HistoryRecord]


class CommandRequest(BaseModel):
    cmd: str = Field(..., description="Command name; GET /api/v1/commands lists them")
    fps: Optional[int] = Field(None, ge=1, le=30, description="for cmd=set_fps")
    width: Optional[int] = Field(None, ge=160, le=1280,
                                 description="for cmd=set_resolution")
    net_input: Optional[int] = Field(None, ge=128, le=512,
                                     description="for cmd=set_net_input")
    enabled: Optional[bool] = Field(None, description="for the guard commands")

    class Config:
        schema_extra = {"example": {"cmd": "reboot"}}


class CommandResponse(BaseModel):
    cmd: str
    accepted: bool
    timestamp: str
    result: Dict[str, Any]


class GuardState(BaseModel):
    enabled: bool
    changed_at: str
    alarms_raised: int


class GuardRequest(BaseModel):
    enabled: bool

    class Config:
        schema_extra = {"example": {"enabled": True}}


# ----------------------------------------------------------------------- app

DESCRIPTION = f"""
REST interface of the **Smart Guardian System** &mdash; Embedded Systems final
project, {STUDENT_NAME} ({STUDENT_ID}).

> **This layer contains no logic.** It is a documentation and gateway shim, as
> the project brief permits. Every endpoint below forwards to the C daemon on
> `127.0.0.1:{CFG.get('loopback_port', '8081')}`, which is where temperature and
> memory are read from `/proc` and `/sys`, people are counted, history is
> stored and commands are executed.

**Authentication** &mdash; the privileged commands (`reboot`, `set_fps`,
`set_resolution`, `restart_vision`, `snapshot`, the guard toggles) require a
bearer token. Click **Authorize** and paste the value of `GUARDIAN_API_TOKEN`
from the board's `/etc/guardian/secrets.env`.

**MQTT topics** &mdash; `telemetry/{STUDENT_ID}/home`, `persons/{STUDENT_ID}/home`,
`alarm/{STUDENT_ID}/home`, `status/{STUDENT_ID}/home` (Last Will).
"""

app = FastAPI(
    title=f"Smart Guardian System &mdash; {STUDENT_ID}",
    description=DESCRIPTION,
    version="1.0.0",
    contact={"name": STUDENT_NAME},
    openapi_tags=[
        {"name": "monitoring", "description": "Live state of the board and the scene"},
        {"name": "stream", "description": "MJPEG video"},
        {"name": "history", "description": "Black box (part 4-2)"},
        {"name": "control", "description": "Commands and anti-theft mode"},
        {"name": "meta", "description": "Health and service metadata"},
    ],
)

bearer = HTTPBearer(auto_error=False)


def auth_header(cred: Optional[HTTPAuthorizationCredentials] = Depends(bearer)
                ) -> Dict[str, str]:
    """Passes the caller's bearer token through to the C daemon.

    The token is never validated here -- the C daemon is the only authority,
    using a constant-time comparison. This shim just forwards the header.
    """
    if cred is None:
        return {}
    return {"Authorization": f"{cred.scheme} {cred.credentials}"}


async def upstream_get(path: str, params: Optional[dict] = None,
                       headers: Optional[dict] = None) -> JSONResponse:
    try:
        async with httpx.AsyncClient(timeout=20.0) as client:
            r = await client.get(UPSTREAM + path, params=params, headers=headers)
    except httpx.HTTPError as exc:
        raise HTTPException(
            status_code=502,
            detail=f"the C daemon is not answering on {UPSTREAM}: {exc}",
        ) from exc
    return JSONResponse(status_code=r.status_code, content=r.json())


async def upstream_post(path: str, payload: dict,
                        headers: Optional[dict] = None) -> JSONResponse:
    try:
        async with httpx.AsyncClient(timeout=20.0) as client:
            r = await client.post(UPSTREAM + path, json=payload, headers=headers)
    except httpx.HTTPError as exc:
        raise HTTPException(
            status_code=502,
            detail=f"the C daemon is not answering on {UPSTREAM}: {exc}",
        ) from exc
    return JSONResponse(status_code=r.status_code, content=r.json())


# ------------------------------------------------------------------- routes


@app.get("/api/v1/persons", response_model=Persons, tags=["monitoring"],
         summary="Current person count",
         description="Number of people in the newest processed frame, with the "
                     "capture timestamp. 503 until the detector publishes its "
                     "first frame.")
async def persons():
    return await upstream_get("/api/v1/persons")


@app.get("/api/v1/telemetry", response_model=Telemetry, tags=["monitoring"],
         summary="CPU temperature, memory and load",
         description="Every field is parsed in C directly from "
                     "`/sys/class/thermal/thermal_zone0/temp`, `/proc/meminfo`, "
                     "`/proc/stat`, `/proc/loadavg` and `/proc/uptime` -- no "
                     "shell utilities are involved.")
async def telemetry():
    return await upstream_get("/api/v1/telemetry")


@app.get("/api/v1/history", response_model=History, tags=["history"],
         summary="Recent detections",
         description="Newest first, from the SQLite ring buffer on the board.")
async def history(limit: int = Query(5, ge=1, le=200,
                                     description="How many records to return")):
    return await upstream_get("/api/v1/history", params={"limit": limit})


@app.get("/api/v1/stats", tags=["history"],
         summary="Lifetime counters",
         description="Totals that survive ring-buffer pruning, plus MQTT, mail, "
                     "watchdog and thermal counters.")
async def stats():
    return await upstream_get("/api/v1/stats")


@app.get("/api/v1/guard", response_model=GuardState, tags=["control"],
         summary="Anti-theft mode state")
async def guard_get():
    return await upstream_get("/api/v1/guard")


@app.post("/api/v1/guard", response_model=GuardState, tags=["control"],
          summary="Arm or disarm anti-theft mode",
          description="When armed, every detection sends an immediate email "
                      f"and publishes to `alarm/{STUDENT_ID}/home`.")
async def guard_set(body: GuardRequest,
                    headers: dict = Depends(auth_header)):
    return await upstream_post("/api/v1/guard", body.dict(), headers)


@app.get("/api/v1/commands", tags=["control"],
         summary="Command catalogue",
         description="The dispatch table the C daemon exposes. Adding a command "
                     "on the board makes it appear here with no change to this "
                     "layer.")
async def commands():
    return await upstream_get("/api/v1/commands")


@app.post("/api/v1/command", response_model=CommandResponse, tags=["control"],
          summary="Execute a command",
          description="Send `{\"cmd\":\"reboot\"}` to restart the board. Other "
                      "commands: `ping`, `guard_on`, `guard_off`, `set_fps`, "
                      "`set_resolution`, `set_net_input`, `snapshot`, "
                      "`publish_telemetry`, "
                      "`restart_vision`. All but `ping` need the bearer token.")
async def command(body: CommandRequest = Body(...),
                  headers: dict = Depends(auth_header)):
    payload = {k: v for k, v in body.dict().items() if v is not None}
    return await upstream_post("/api/v1/command", payload, headers)


@app.get("/api/v1/health", tags=["meta"], summary="Component health")
async def health():
    return await upstream_get("/api/v1/health")


@app.get("/api/v1/stream", tags=["stream"],
         summary="Live MJPEG stream",
         description="`multipart/x-mixed-replace` of annotated frames. Swagger "
                     "UI cannot render a never-ending multipart response, so "
                     "open this URL directly in a browser or embed it in an "
                     "`<img>` tag; the dashboard on port 443 does exactly that.",
         response_description="multipart/x-mixed-replace; boundary=guardianframe")
async def stream():
    client = httpx.AsyncClient(timeout=None)
    try:
        req = client.build_request("GET", UPSTREAM + "/api/v1/stream")
        resp = await client.send(req, stream=True)
    except httpx.HTTPError as exc:
        await client.aclose()
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    async def relay():
        try:
            async for chunk in resp.aiter_raw():
                yield chunk
        finally:
            await resp.aclose()
            await client.aclose()

    return StreamingResponse(
        relay(),
        status_code=resp.status_code,
        media_type=resp.headers.get("content-type",
                                    "multipart/x-mixed-replace; boundary=guardianframe"),
    )


@app.get("/", include_in_schema=False)
async def root():
    return JSONResponse({
        "service": "Smart Guardian System -- API documentation layer",
        "student": f"{STUDENT_NAME} ({STUDENT_ID})",
        "swagger_ui": "/docs",
        "redoc": "/redoc",
        "openapi": "/openapi.json",
        "dashboard": f"https://{CFG.get('public_host', '192.168.100.26')}/",
        "upstream": UPSTREAM,
        "note": "all logic lives in the C daemon; this layer only documents "
                "and forwards",
    })


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="/etc/guardian/guardian.conf")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--ssl-certfile", default=None)
    ap.add_argument("--ssl-keyfile", default=None)
    args = ap.parse_args()

    os.environ.setdefault("GUARDIAN_CONF", args.config)

    print(f"<6>guardian-api: proxying to {UPSTREAM}, Swagger UI on "
          f"https://{args.host}:{args.port}/docs", flush=True)

    uvicorn.run(
        app,
        host=args.host,
        port=args.port,
        ssl_certfile=args.ssl_certfile,
        ssl_keyfile=args.ssl_keyfile,
        log_level="info",
        access_log=False,   # journald already records what matters
        workers=1,          # 1 GB of RAM; one worker is the right answer here
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
