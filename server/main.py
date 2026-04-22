# server/main.py
# FastAPI: MJPEG + H.264, переключение кодека через /agents/{id}/config,
# управление мышью через /ws/control/{agent|viewer}/{id}.
import asyncio
import time
import json
from typing import Dict, Optional, Set

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import StreamingResponse, FileResponse, PlainTextResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

app = FastAPI(title="Stream relay (MJPEG + H.264) + control")


# ============ MODEL ============
class AgentState:
    __slots__ = (
        "codec_target",
        "encoder_target",
        "bitrate_target",
        "fps_target",
        "mjpeg_q_target",
        "codec_current",
        "encoder_current",
        "bitrate_current",
        "fps_current",
        "mjpeg_latest",
        "mjpeg_count",
        "_mjpeg_event",
        "h264_keyframe_buffer",
        "h264_subscribers",
        "h264_count",
        "updated",
        "started",
    )

    def __init__(self):
        self.codec_target = "mjpeg"
        self.encoder_target = "cpu"
        self.bitrate_target = "4M"
        self.fps_target = 30
        self.mjpeg_q_target = 4

        self.codec_current: Optional[str] = None
        self.encoder_current: Optional[str] = None
        self.bitrate_current: Optional[str] = None
        self.fps_current: Optional[int] = None

        self.mjpeg_latest: Optional[bytes] = None
        self.mjpeg_count: int = 0
        self._mjpeg_event = asyncio.Event()

        self.h264_keyframe_buffer = bytearray()
        self.h264_subscribers: Set[asyncio.Queue] = set()
        self.h264_count: int = 0

        self.updated: float = 0.0
        self.started: float = time.time()

    def push_mjpeg(self, frame: bytes):
        self.mjpeg_latest = frame
        self.mjpeg_count += 1
        self.updated = time.time()
        ev = self._mjpeg_event
        self._mjpeg_event = asyncio.Event()
        ev.set()

    async def wait_mjpeg(self, timeout: float = 5.0) -> bool:
        try:
            await asyncio.wait_for(self._mjpeg_event.wait(), timeout=timeout)
            return True
        except asyncio.TimeoutError:
            return False

    def push_h264(self, chunk: bytes):
        idr_off = find_idr_offset(chunk)
        if idr_off >= 0:
            self.h264_keyframe_buffer = bytearray(chunk[idr_off:])
            self.h264_count += 1
        else:
            self.h264_keyframe_buffer.extend(chunk)
            if len(self.h264_keyframe_buffer) > 32 * 1024 * 1024:
                self.h264_keyframe_buffer = self.h264_keyframe_buffer[
                    -8 * 1024 * 1024 :
                ]
        self.updated = time.time()
        dead = []
        for q in self.h264_subscribers:
            try:
                q.put_nowait(chunk)
            except asyncio.QueueFull:
                dead.append(q)
        for q in dead:
            self.h264_subscribers.discard(q)


AGENTS: Dict[str, AgentState] = {}
LOCK = asyncio.Lock()


async def get_agent(aid: str) -> AgentState:
    async with LOCK:
        a = AGENTS.get(aid)
        if a is None:
            a = AgentState()
            AGENTS[aid] = a
        return a


# ============ H.264 helpers ============
def find_idr_offset(buf: bytes) -> int:
    """Смещение первого start code с последующим NALU типа 7 (SPS) или 5 (IDR)."""
    i = 0
    n = len(buf)
    while i < n - 3:
        if buf[i] == 0 and buf[i + 1] == 0:
            sc = 0
            if buf[i + 2] == 1:
                sc = 3
            elif i + 3 < n and buf[i + 2] == 0 and buf[i + 3] == 1:
                sc = 4
            if sc:
                if i + sc < n:
                    t = buf[i + sc] & 0x1F
                    if t == 7 or t == 5:
                        return i
                i += sc
                continue
        i += 1
    return -1


# ============ CONTROL HUB ============
class ControlHub:
    def __init__(self):
        self.agent_ws: Dict[str, WebSocket] = {}
        self.viewer_ws: Dict[str, Set[WebSocket]] = {}
        self.agent_hello: Dict[str, str] = {}


HUB = ControlHub()


@app.websocket("/ws/control/agent/{aid}")
async def ws_control_agent(ws: WebSocket, aid: str):
    await ws.accept()
    old = HUB.agent_ws.get(aid)
    if old is not None:
        try:
            await old.close()
        except Exception:
            pass
    HUB.agent_ws[aid] = ws
    print(f"[ctrl] agent connected: {aid}")
    try:
        while True:
            msg = await ws.receive_text()
            try:
                obj = json.loads(msg)
            except Exception:
                continue
            if obj.get("type") == "hello":
                HUB.agent_hello[aid] = msg
                for vws in list(HUB.viewer_ws.get(aid, ())):
                    try:
                        await vws.send_text(msg)
                    except Exception:
                        pass
    except WebSocketDisconnect:
        pass
    except Exception as e:
        print(f"[ctrl] agent {aid}: {e}")
    finally:
        if HUB.agent_ws.get(aid) is ws:
            HUB.agent_ws.pop(aid, None)
            HUB.agent_hello.pop(aid, None)
        print(f"[ctrl] agent disconnected: {aid}")


@app.websocket("/ws/control/viewer/{aid}")
async def ws_control_viewer(ws: WebSocket, aid: str):
    await ws.accept()
    HUB.viewer_ws.setdefault(aid, set()).add(ws)
    hello = HUB.agent_hello.get(aid)
    if hello:
        try:
            await ws.send_text(hello)
        except Exception:
            pass
    try:
        while True:
            msg = await ws.receive_text()
            agent = HUB.agent_ws.get(aid)
            if agent is not None:
                try:
                    await agent.send_text(msg)
                except Exception:
                    pass
    except WebSocketDisconnect:
        pass
    except Exception as e:
        print(f"[ctrl] viewer {aid}: {e}")
    finally:
        s = HUB.viewer_ws.get(aid)
        if s is not None:
            s.discard(ws)


# ============ CONFIG (admin) ============
class ConfigBody(BaseModel):
    codec: Optional[str] = Field(None, pattern="^(mjpeg|h264)$")
    encoder: Optional[str] = Field(None, pattern="^(cpu|amf|qsv|nvenc)$")
    bitrate: Optional[str] = Field(None, pattern=r"^\d+[KMkm]?$")
    fps: Optional[int] = Field(None, ge=1, le=120)
    mjpeg_q: Optional[int] = Field(None, ge=2, le=31)


@app.get("/agents/{aid}/config", response_class=PlainTextResponse)
async def get_config(aid: str):
    a = await get_agent(aid)
    return (
        f"codec={a.codec_target}\n"
        f"encoder={a.encoder_target}\n"
        f"bitrate={a.bitrate_target}\n"
        f"fps={a.fps_target}\n"
        f"mjpeg_q={a.mjpeg_q_target}\n"
    )


@app.post("/agents/{aid}/config")
async def set_config(aid: str, body: ConfigBody):
    a = await get_agent(aid)
    if body.codec is not None:
        a.codec_target = body.codec
    if body.encoder is not None:
        a.encoder_target = body.encoder
    if body.bitrate is not None:
        a.bitrate_target = body.bitrate
    if body.fps is not None:
        a.fps_target = body.fps
    if body.mjpeg_q is not None:
        a.mjpeg_q_target = body.mjpeg_q
    return {
        "status": "ok",
        "target": {
            "codec": a.codec_target,
            "encoder": a.encoder_target,
            "bitrate": a.bitrate_target,
            "fps": a.fps_target,
            "mjpeg_q": a.mjpeg_q_target,
        },
    }


# ============ INGEST ============
MJPEG_SOI = b"\xff\xd8"
MJPEG_EOI = b"\xff\xd9"
MAX_MJPEG = 16 * 1024 * 1024


@app.post("/ingest/{aid}")
async def ingest(aid: str, request: Request):
    a = await get_agent(aid)
    ctype = request.headers.get("content-type", "").lower()
    a.encoder_current = request.headers.get("x-agent-encoder")
    a.bitrate_current = request.headers.get("x-agent-bitrate")
    try:
        a.fps_current = int(request.headers.get("x-agent-fps", "0")) or None
    except:
        a.fps_current = None

    if "h264" in ctype:
        a.codec_current = "h264"
        print(f"[ingest] h264/{a.encoder_current} <- {request.client.host} id={aid}")
        try:
            async for chunk in request.stream():
                if chunk:
                    a.push_h264(chunk)
        except Exception as e:
            print(f"[ingest] h264 {aid} err: {e}")
        return {"status": "ok", "mode": "h264"}
    else:
        a.codec_current = "mjpeg"
        print(f"[ingest] mjpeg <- {request.client.host} id={aid}")
        buf = bytearray()
        try:
            async for chunk in request.stream():
                if not chunk:
                    continue
                buf.extend(chunk)
                while True:
                    soi = buf.find(MJPEG_SOI)
                    if soi < 0:
                        buf.clear()
                        break
                    if soi > 0:
                        del buf[:soi]
                    eoi = buf.find(MJPEG_EOI, 2)
                    if eoi < 0:
                        if len(buf) > MAX_MJPEG:
                            buf.clear()
                        break
                    frame = bytes(buf[: eoi + 2])
                    del buf[: eoi + 2]
                    a.push_mjpeg(frame)
        except Exception as e:
            print(f"[ingest] mjpeg {aid} err: {e}")
        return {"status": "ok", "mode": "mjpeg"}


# ============ STREAM OUT ============
BOUNDARY = "frame"


@app.get("/stream/mjpeg/{aid}")
async def stream_mjpeg(aid: str):
    a = await get_agent(aid)

    async def gen():
        last = -1
        if a.mjpeg_latest is None:
            await a.wait_mjpeg(timeout=10.0)
        while True:
            if a.mjpeg_count != last and a.mjpeg_latest is not None:
                last = a.mjpeg_count
                f = a.mjpeg_latest
                yield (
                    b"--" + BOUNDARY.encode() + b"\r\nContent-Type: image/jpeg\r\n"
                    b"Content-Length: "
                    + str(len(f)).encode()
                    + b"\r\n\r\n"
                    + f
                    + b"\r\n"
                )
            else:
                await a.wait_mjpeg(timeout=5.0)

    return StreamingResponse(
        gen(),
        media_type=f"multipart/x-mixed-replace; boundary={BOUNDARY}",
        headers={"Cache-Control": "no-store", "Connection": "close"},
    )


@app.websocket("/ws/stream/h264/{aid}")
async def ws_h264(ws: WebSocket, aid: str):
    await ws.accept()
    a = await get_agent(aid)
    q: asyncio.Queue = asyncio.Queue(maxsize=400)

    # КРИТИЧНО: snapshot буфера и подписка на очередь должны выполняться
    # атомарно относительно event loop — между ними не должно быть ни одного
    # await. Иначе ingest-корутина может обработать новые chunk'и: они
    # окажутся в буфере (уже снят snapshot? значит не попадут в него) и не
    # попадут в нашу очередь (мы ещё не в subscribers). На клиенте получится
    # дыра между snapshot и live-частью -> decoder: Decoding error на первом
    # же P-кадре после дыры.
    #
    # Порядок: сперва подписываемся, потом снимаем snapshot. Тогда каждый
    # новый chunk гарантированно попадает либо в snapshot (если push_h264
    # выполнился до нашей строки со snapshot), либо в очередь (если после
    # подписки). Перекрытия нет, дыр нет.
    a.h264_subscribers.add(q)
    snapshot = bytes(a.h264_keyframe_buffer) if a.h264_keyframe_buffer else b""

    try:
        if snapshot:
            await ws.send_bytes(snapshot)
        while True:
            chunk = await q.get()
            await ws.send_bytes(chunk)
    except WebSocketDisconnect:
        pass
    except Exception as e:
        print(f"[ws h264] {aid} err: {e}")
    finally:
        a.h264_subscribers.discard(q)


# ============ META ============
@app.get("/agents")
async def list_agents():
    now = time.time()
    out = []
    for aid, a in AGENTS.items():
        alive = a.updated > 0 and (now - a.updated) < 5.0
        out.append(
            {
                "id": aid,
                "alive": alive,
                "uptime_s": round(now - a.started, 1),
                "mjpeg_frames": a.mjpeg_count,
                "h264_keyframes": a.h264_count,
                "h264_viewers": len(a.h264_subscribers),
                "ctrl_connected": aid in HUB.agent_ws,
                "target": {
                    "codec": a.codec_target,
                    "encoder": a.encoder_target,
                    "bitrate": a.bitrate_target,
                    "fps": a.fps_target,
                    "mjpeg_q": a.mjpeg_q_target,
                },
                "current": {
                    "codec": a.codec_current,
                    "encoder": a.encoder_current,
                    "bitrate": a.bitrate_current,
                    "fps": a.fps_current,
                },
            }
        )
    return {"agents": out}


@app.get("/healthz")
async def healthz():
    return {"ok": True, "agents": len(AGENTS)}


app.mount("/static", StaticFiles(directory="static"), name="static")


@app.get("/")
async def index():
    return FileResponse("static/index.html")
