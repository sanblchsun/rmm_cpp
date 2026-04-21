# FastAPI сервер: принимает MJPEG-поток от C++ агентов (POST /ingest/{id})
# и раздаёт любому числу браузеров как multipart/x-mixed-replace (GET /stream/{id}).

import asyncio
import time
from typing import Dict, Optional

from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import StreamingResponse, FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

app = FastAPI(title="MJPEG relay")

# ---------- модель потока ----------
class AgentStream:
    __slots__ = ("latest", "count", "updated", "started", "_event")

    def __init__(self):
        self.latest: Optional[bytes] = None
        self.count: int = 0
        self.updated: float = 0.0
        self.started: float = time.time()
        self._event: asyncio.Event = asyncio.Event()

    def push(self, frame: bytes) -> None:
        self.latest = frame
        self.count += 1
        self.updated = time.time()
        ev = self._event
        self._event = asyncio.Event()
        ev.set()

    async def wait(self, timeout: float = 5.0) -> bool:
        try:
            await asyncio.wait_for(self._event.wait(), timeout=timeout)
            return True
        except asyncio.TimeoutError:
            return False


AGENTS: Dict[str, AgentStream] = {}
LOCK = asyncio.Lock()


async def get_agent(agent_id: str) -> AgentStream:
    async with LOCK:
        a = AGENTS.get(agent_id)
        if a is None:
            a = AgentStream()
            AGENTS[agent_id] = a
        return a


# ---------- приём от агента ----------
SOI = b"\xff\xd8"  # Start Of Image
EOI = b"\xff\xd9"  # End Of Image
MAX_BUFFER = 16 * 1024 * 1024  # 16 MB страховка


@app.post("/ingest/{agent_id}")
async def ingest(agent_id: str, request: Request):
    agent = await get_agent(agent_id)
    buf = bytearray()
    print(f"[ingest] agent '{agent_id}' connected from {request.client.host}")
    try:
        async for chunk in request.stream():
            if not chunk:
                continue
            buf.extend(chunk)
            while True:
                soi = buf.find(SOI)
                if soi < 0:
                    buf.clear()
                    break
                if soi > 0:
                    del buf[:soi]
                eoi = buf.find(EOI, 2)
                if eoi < 0:
                    if len(buf) > MAX_BUFFER:
                        buf.clear()
                    break
                frame = bytes(buf[: eoi + 2])
                del buf[: eoi + 2]
                agent.push(frame)
    except Exception as e:
        print(f"[ingest] '{agent_id}' disconnected: {e}")
    else:
        print(f"[ingest] '{agent_id}' stream ended")
    return {"status": "ok", "agent_id": agent_id, "frames": agent.count}


# ---------- отдача браузеру ----------
BOUNDARY = "frame"


@app.get("/stream/{agent_id}")
async def stream(agent_id: str):
    agent = await get_agent(agent_id)

    async def gen():
        last = -1
        # ждём первый кадр до 10 секунд, чтобы не отдавать пустой ответ
        if agent.latest is None:
            await agent.wait(timeout=10.0)
        while True:
            if agent.count != last and agent.latest is not None:
                last = agent.count
                f = agent.latest
                yield (
                    b"--" + BOUNDARY.encode() + b"\r\n"
                    b"Content-Type: image/jpeg\r\n"
                    b"Content-Length: " + str(len(f)).encode() + b"\r\n\r\n"
                    + f + b"\r\n"
                )
            else:
                if not await agent.wait(timeout=5.0):
                    # keep-alive «пустая» итерация; просто продолжаем
                    continue

    headers = {
        "Cache-Control": "no-cache, no-store, private",
        "Pragma": "no-cache",
        "Connection": "close",
    }
    return StreamingResponse(
        gen(),
        media_type=f"multipart/x-mixed-replace; boundary={BOUNDARY}",
        headers=headers,
    )


# ---------- метаданные ----------
@app.get("/agents")
async def list_agents():
    now = time.time()
    data = []
    for aid, a in AGENTS.items():
        alive = a.updated > 0 and (now - a.updated) < 5.0
        data.append({
            "id": aid,
            "frames": a.count,
            "last_update": a.updated,
            "uptime_s": round(now - a.started, 1),
            "alive": alive,
        })
    return {"agents": data}


@app.get("/healthz")
async def healthz():
    return {"ok": True, "agents": len(AGENTS)}


# ---------- статика ----------
app.mount("/static", StaticFiles(directory="static"), name="static")


@app.get("/")
async def index():
    return FileResponse("static/index.html")
