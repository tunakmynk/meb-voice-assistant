"""Device WebSocket gateway — the event contract from design doc 3.2.

Device -> backend:  speech_start {grade, subject} · binary PCM · speech_end · cancel
Backend -> device:  transcript · thinking · answer_text · binary audio · done · error

This module is deliberately thin: it owns the socket and the buffer, and
forwards whatever the orchestrator emits. Turn logic lives in the pipeline so
it stays testable without a socket.

ADR-04 (barge-in): each turn runs as a cancellable asyncio task, so a `cancel`
event arriving mid-answer stops the LLM and TTS streams instead of waiting for
the turn to finish.
"""

from __future__ import annotations

import asyncio
import logging

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from app.adapters.llm import llm_adapter
from app.adapters.stt import stt_adapter
from app.adapters.tts import tts_adapter
from app.conversation.store import current_model_config, store
from app.orchestrator.pipeline import Orchestrator
from app.panel import live
from app.rag.retriever import retriever

log = logging.getLogger(__name__)
router = APIRouter()

orchestrator = Orchestrator(stt_adapter, retriever, llm_adapter, tts_adapter)


class _Session:
    """Per-connection state: the audio buffer and the active turn."""

    def __init__(self) -> None:
        self.chunks: list[bytes] = []
        self.grade: str | None = None
        self.subject: str | None = None
        self.task: asyncio.Task | None = None
        # ADR-07: bir WebSocket oturumu = bir sohbet. İlk turda açılır.
        self.conversation_id: int | None = None
        self.opened = False
        # Short-term context, doc 4.2: last few turns travel with the request.
        self.history: list[dict[str, str]] = []

    def reset_audio(self) -> None:
        self.chunks = []

    @property
    def audio(self) -> bytes:
        return b"".join(self.chunks)

    def remember(self, question: str, answer: str) -> None:
        self.history.extend([
            {"role": "user", "content": question},
            {"role": "assistant", "content": answer},
        ])
        # Keep the last 3 turns (6 messages); older context costs tokens
        # without helping a single-question-per-turn device.
        self.history = self.history[-6:]


async def _persist(session: _Session, question: str, answer: str,
                   summary: dict | None) -> None:
    """Write the finished turn (ADR-07). Never raises — see store docstring."""
    if summary is None:
        return
    if not session.opened:
        session.opened = True
        session.conversation_id = await store.start_conversation(
            device_id=None, grade=session.grade, subject=session.subject,
        )
    await store.record_turn(
        session.conversation_id,
        question=question,
        answer=answer,
        chunks=summary.get("retrieved", []),
        latencies=summary.get("latencies_ms", {}),
        model_config=current_model_config(),
        no_source=summary.get("no_source", False),
    )


async def _cancel_active(session: _Session, ws: WebSocket) -> None:
    """Stop the running turn and wait until it has actually stopped.

    Awaiting matters. `Task.cancel()` only *requests* cancellation; the task
    keeps running until its next await point, and until then it can still push
    audio frames onto the socket. Starting the next turn without waiting is
    exactly how two answers end up interleaved on one connection.

    The device is told so it can drop whatever it has already buffered — bytes
    sent before the cancel are still in flight and would otherwise be played.
    """
    task = session.task
    session.task = None
    if task is None or task.done():
        return

    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass

    try:
        await ws.send_json({"type": "cancelled"})
    except Exception:
        pass  # socket already gone; nothing to tell


# Yarıda kesilen cevap geçmişe böyle giriyor. İşaret şart: işaretsiz
# saklanırsa model cevabı tamamladığını sanır ve öğrenci "devam et" dediğinde
# baştan anlatmak yerine yeni bir konuya geçer.
_KESILDI = "{answer} [öğrenci sözümü burada kesti]"


async def _run_turn(ws: WebSocket, session: _Session) -> None:
    question = answer = ""
    summary: dict | None = None
    try:
        async for event in orchestrator.run_turn_events(
            session.audio,
            grade=session.grade,
            subject=session.subject,
            history=session.history,
        ):
            if event["type"] == "audio":
                await ws.send_bytes(event["data"])
                continue
            if event["type"] == "transcript":
                question = event["text"]
                live.yayinla({"type": "turn", "phase": "question",
                              "text": question,
                              "grade": session.grade, "subject": session.subject})
            elif event["type"] == "answer_text":
                answer = event["text"]
            elif event["type"] == "done":
                # Record BEFORE announcing completion.
                #
                # Writing after `done` looked natural and lost every single
                # turn: the client sends its next question the moment `done`
                # arrives, `speech_start` cancels this task, and it dies partway
                # through the insert. Closing the socket did the same. Doing the
                # work while the turn is still in progress removes the race
                # instead of racing it — by the time `done` goes out, the row is
                # already committed. All the audio has been sent by now, so the
                # few milliseconds cost the student nothing.
                summary = event
                live.yayinla({
                    "type": "turn", "phase": "done",
                    "text": answer,
                    "no_source": event.get("no_source", False),
                    "latencies_ms": event.get("latencies_ms", {}),
                    "retrieved": event.get("retrieved", []),
                })
                if question and answer:
                    session.remember(question, answer)
                    # Guarded on its own: the built-in store swallows its own
                    # failures, but this boundary must hold for ANY store — a
                    # persistence problem must never reach the student as a
                    # failed answer.
                    try:
                        await _persist(session, question, answer, summary)
                    except Exception:
                        log.warning("tur kaydedilemedi", exc_info=True)

            await ws.send_json(event)

    except asyncio.CancelledError:
        # Expected on barge-in — the device already stopped playback.
        #
        # But the turn still HAPPENED for the student: they heard part of the
        # answer and their next question almost always refers to it ("bununla
        # ilgili", "peki bu neden böyle"). Dropping it left the rewrite step
        # with nothing to resolve, so retrieval searched a question with no
        # content words and returned unrelated sources — a wrong answer with a
        # wrong breadcrumb, which is worse than no answer.
        #
        # Only short-term memory is updated. _persist needs the `done`
        # summary (retrieved chunks, latencies, no_source) and a cancelled
        # turn has none of it; writing a row with those fields empty would
        # make the panel's numbers lie.
        if question and answer:
            session.remember(question, _KESILDI.format(answer=answer))
        raise
    except Exception as exc:
        log.exception("turn failed")
        await ws.send_json({
            "type": "error",
            "code": type(exc).__name__,
            "message": str(exc)[:200],
        })


@router.websocket("/ws")
async def device_socket(ws: WebSocket) -> None:
    await ws.accept()
    session = _Session()
    # Panel cihazin bagli olup olmadigini gormeli; demo gunu "cihaz mi
    # dustu, backend mi" sorusunun en hizli cevabi bu.
    live.yayinla({"type": "status", "device": "connected"})

    try:
        while True:
            message = await ws.receive()

            if message["type"] == "websocket.disconnect":
                break

            # Binary frames are raw PCM; they carry no envelope by design so
            # the device does not pay JSON encoding cost per audio chunk.
            if (data := message.get("bytes")) is not None:
                session.chunks.append(data)
                continue

            event = _parse(message.get("text"))
            if event is None:
                continue

            kind = event.get("type")

            if kind == "speech_start":
                # Pressing the button IS the interruption (ADR-04). Waiting for
                # an explicit `cancel` would leave the old turn streaming while
                # the new one starts, and both answers would play at once.
                await _cancel_active(session, ws)
                session.reset_audio()
                session.grade = event.get("grade")
                session.subject = event.get("subject")

            elif kind == "speech_end":
                if not session.audio:
                    await ws.send_json({
                        "type": "error", "code": "empty_audio",
                        "message": "Ses alınamadı.",
                    })
                    continue
                # Belt and braces: a device that never sends speech_start must
                # still not be able to run two turns down one socket.
                await _cancel_active(session, ws)
                session.task = asyncio.create_task(_run_turn(ws, session))

            elif kind == "cancel":
                await _cancel_active(session, ws)
                session.reset_audio()

    except WebSocketDisconnect:
        pass
    finally:
        if session.task and not session.task.done():
            session.task.cancel()
        live.yayinla({"type": "status", "device": "disconnected"})


def _parse(text: str | None) -> dict | None:
    import json

    if not text:
        return None
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        log.warning("bad json frame: %r", text[:80])
        return None
    return parsed if isinstance(parsed, dict) else None
