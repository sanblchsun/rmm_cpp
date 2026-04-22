# FastAPI: MJPEG + H.264, переключение кодека через /agents/{id}/config.
import asyncio, time, re
from typing import Dict, Optional, Set, List
from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.responses import StreamingResponse, FileResponse, PlainTextResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

app = FastAPI(title="Stream relay (MJPEG + H.264)")


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
        # mjpeg:
        "mjpeg_latest",
        "mjpeg_count",
        "_mjpeg_event",
        # h264:
        "h264_keyframe_buffer",  # последние байты начиная с последнего keyframe
        "h264_subscribers",  # set of asyncio.Queue[bytes]
        "h264_count",
        # общее:
        "updated",
        "started",
    )

    def __init__(self):
        # target = что админ хочет; current = что реально шлёт агент
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
        self.mjpeg_count = 0
        self._mjpeg_event = asyncio.Event()

        self.h264_keyframe_buffer = bytearray()
        self.h264_subscribers: Set[asyncio.Queue] = set()
        self.h264_count = 0

        self.updated = 0.0
        self.started = time.time()

    # --- MJPEG ---
    def push_mjpeg(self, frame: bytes):
        self.mjpeg_latest = frame
        self.mjpeg_count += 1
        self.updated = time.time()
        ev = self._mjpeg_event
        self._mjpeg_event = asyncio.Event()
        ev.set()

    async def wait_mjpeg(self, timeout=5.0) -> bool:
        try:
            await asyncio.wait_for(self._mjpeg_event.wait(), timeout=timeout)
            return True
        except asyncio.TimeoutError:
            return False

    # --- H.264 ---
    def push_h264(self, chunk: bytes):
        """
        Накопительно кладём байты в keyframe_buffer. Если в чанке встречен IDR NALU,
        сбрасываем буфер так, чтобы он начинался именно с этого IDR (ровно с его стартового кода).
        Также рассылаем чанк всем активным подписчикам.
        """
        idr_off = find_idr_offset(chunk)
        if idr_off >= 0:
            # буфер начинается с IDR-фрейма
            self.h264_keyframe_buffer = bytearray(chunk[idr_off:])
            self.h264_count += 1
        else:
            self.h264_keyframe_buffer.extend(chunk)
            if len(self.h264_keyframe_buffer) > 32 * 1024 * 1024:
                # страховка: держим не больше 32 MB между keyframe
                self.h264_keyframe_buffer = self.h264_keyframe_buffer[
                    -8 * 1024 * 1024 :
                ]

        self.updated = time.time()
        # рассылка
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
# Находим в Annex-B смещение первого NALU с типом 5 (IDR). Start code: 000001 или 00000001.
_IDR_RE3 = re.compile(
    rb"\x00\x00\x01[\x65\xE5\x45\xC5]"
)  # nal_ref_idc|type=5, любой nal_ref_idc


# В WebCodecs достаточно, чтобы blob начинался со SPS/PPS/IDR — но SPS/PPS обычно идут вместе с IDR
# у libx264/hw-энкодеров (repeat-headers). Если нет — агент настроен на repeat_sps/pps.
# Упрощённо ищем NALU-заголовок с типом 5:
def find_idr_offset(buf: bytes) -> int:
    """Возвращает индекс первого start code, после которого идёт IDR NALU (type=5). Или -1."""
    i = 0
    n = len(buf)
    while i < n - 3:
        if buf[i] == 0 and buf[i + 1] == 0:
            sc_len = 0
            if buf[i + 2] == 1:
                sc_len = 3
            elif i + 3 < n and buf[i + 2] == 0 and buf[i + 3] == 1:
                sc_len = 4
            if sc_len:
                if i + sc_len < n:
                    nalu_type = buf[i + sc_len] & 0x1F
                    if nalu_type == 7:  # SPS — ещё лучше, чем IDR: у декодера будет всё
                        return i
                    if nalu_type == 5:
                        return i
                i += sc_len
                continue
        i += 1
    return -1


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
    lines = [
        f"codec={a.codec_target}",
        f"encoder={a.encoder_target}",
        f"bitrate={a.bitrate_target}",
        f"fps={a.fps_target}",
        f"mjpeg_q={a.mjpeg_q_target}",
    ]
    return "\n".join(lines) + "\n"


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
    q: asyncio.Queue = asyncio.Queue(maxsize=200)
    # отдаём стартовый буфер (начинается с SPS/IDR, если он есть) — чтобы декодер сразу завёлся
    if a.h264_keyframe_buffer:
        await ws.send_bytes(bytes(a.h264_keyframe_buffer))
    a.h264_subscribers.add(q)
    try:
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
