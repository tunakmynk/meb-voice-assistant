"""FastAPI application — single async process (ADR-01).

Startup does two things that matter for the demo:

1. **Warms up the retriever.** fastembed loads ~2 GB of ONNX weights on first
   use. Paying that on the first student question would blow the 4 s budget in
   the most visible possible way; paying it at boot costs nobody anything.
2. **Checks the API key early.** A missing key otherwise surfaces as a failed
   turn halfway through a demo, which looks like a broken product rather than
   a missing config line.
"""

from __future__ import annotations

import logging
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from app.config import settings
from app.conversation.store import store
from app.api.routes import router as api_router
from app.gateway.ws import router as ws_router
from app.panel.routes import router as panel_router
from app.rag.retriever import retriever

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
log = logging.getLogger(__name__)

STATIC_DIR = Path(__file__).parent / "static"


@asynccontextmanager
async def lifespan(app: FastAPI):
    if not settings.openai_api_key:
        log.warning(
            "OPENAI_API_KEY bos — STT/LLM/TTS cagrilari basarisiz olacak. "
            "backend/.env dosyasini doldurun."
        )
    log.info("Retriever warmup basliyor (ilk yuklemede ~2 GB model)...")
    await retriever.warmup()
    log.info("Warmup tamam, servis hazir.")

    # Kalıcılık isteğe bağlı: Postgres yoksa ses döngüsü yine de çalışmalı,
    # yalnızca panel boş kalır (ADR-07).
    try:
        await store.connect()
    except Exception:
        log.warning("Postgres'e baglanilamadi — sohbetler kaydedilmeyecek.", exc_info=True)

    yield
    await store.close()
    await retriever.close()


app = FastAPI(title="MEB Sesli AI Asistan", lifespan=lifespan)
app.include_router(api_router)
app.include_router(ws_router)
app.include_router(panel_router)

if STATIC_DIR.is_dir():
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.get("/health")
async def health() -> dict:
    return {
        "status": "ok",
        "collection": settings.qdrant_collection,
        "llm_model": settings.llm_model,
        "api_key_set": bool(settings.openai_api_key),
    }


@app.get("/")
async def index() -> FileResponse:
    """Serve the browser test page — R-03's fallback path."""
    return FileResponse(STATIC_DIR / "test.html")
