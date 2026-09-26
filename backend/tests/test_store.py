"""Persistence wiring tests (ADR-07, plan 2.4/2.5).

These do not touch Postgres. What matters here is *what the gateway decides to
record and when* — the SQL itself is exercised by running the app.

The rules being locked down:
  - a conversation is opened once per socket, not once per turn
  - settings are captured per turn, so a later config change cannot rewrite
    history
  - a turn cut short by barge-in is never recorded
  - a broken database never breaks the turn
"""

from __future__ import annotations

import asyncio
import json

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from app.conversation.store import InMemoryConversationStore
from app.gateway import ws as ws_module


class QuickOrchestrator:
    def __init__(self) -> None:
        self._n = 0

    async def run_turn_events(self, pcm_audio, *, grade=None, subject=None, history=None):
        self._n += 1
        yield {"type": "transcript", "text": f"soru{self._n}"}
        yield {"type": "thinking"}
        yield {"type": "audio", "data": b"pcm"}
        yield {"type": "answer_text", "text": f"cevap{self._n}", "partial": False,
               "no_source": False, "source": None}
        yield {
            "type": "done",
            "latencies_ms": {"stt": 100, "retrieval": 50},
            "no_source": False,
            "retrieved": [{"chunk_id": "c1", "breadcrumb": "5. ÜNİTE > ISI",
                           "grade": "8.Sınıf", "subject": "Fen Bilimleri",
                           "score": 0.83}],
        }


@pytest.fixture
def client(monkeypatch):
    fake_store = InMemoryConversationStore()
    monkeypatch.setattr(ws_module, "orchestrator", QuickOrchestrator())
    monkeypatch.setattr(ws_module, "store", fake_store)

    app = FastAPI()
    app.include_router(ws_module.router)
    with TestClient(app) as c:
        yield c, fake_store


def _speak(sock):
    sock.send_json({"type": "speech_start", "grade": "8.Sınıf", "subject": "Fen Bilimleri"})
    sock.send_bytes(b"\x00\x01" * 50)
    sock.send_json({"type": "speech_end"})
    for _ in range(30):
        msg = sock.receive()
        if msg.get("text") and json.loads(msg["text"])["type"] == "done":
            return


def test_turn_is_recorded_with_sources_and_timings(client):
    http, fake_store = client
    with http.websocket_connect("/ws") as sock:
        _speak(sock)

    assert len(fake_store.turns) == 1
    turn = fake_store.turns[0]
    assert turn["question"] == "soru1"
    assert turn["answer"] == "cevap1"
    assert turn["latencies"] == {"stt": 100, "retrieval": 50}
    assert turn["chunks"][0]["breadcrumb"] == "5. ÜNİTE > ISI"
    # Raw audio must never be written (design doc 4.2).
    assert "audio" not in turn and "pcm" not in json.dumps(turn)


def test_settings_are_captured_per_turn(client):
    """The panel can change top_k at runtime, so the value must be frozen with
    the answer rather than read back from config later."""
    http, fake_store = client
    with http.websocket_connect("/ws") as sock:
        _speak(sock)

    cfg = fake_store.turns[0]["model_config"]
    assert "llm_model" in cfg and "top_k" in cfg and "temperature" in cfg


def test_slow_write_is_not_cancelled_by_the_next_question(client):
    """Regression: four real turns ran and not one row was written.

    The write happens after `done` is sent, so a follow-up question arrives at
    `speech_start` while it is still in flight and barge-in cancels the task
    mid-write. Barge-in must interrupt speech, not bookkeeping.

    The in-memory store is instant, which is why the earlier tests missed
    this — the delay here is what makes the race reproducible.
    """
    http, fake_store = client

    class SlowStore(InMemoryConversationStore):
        async def record_turn(self, *a, **kw):
            await asyncio.sleep(0.3)
            await super().record_turn(*a, **kw)

    slow = SlowStore()
    ws_module.store = slow
    try:
        with http.websocket_connect("/ws") as sock:
            _speak(sock)
            _speak(sock)   # ilk turun yazmasi hala surerken
    finally:
        ws_module.store = fake_store

    assert len(slow.turns) == 2, f"kaydedilen tur: {len(slow.turns)}"


def test_one_conversation_per_socket_not_per_turn(client):
    http, fake_store = client
    with http.websocket_connect("/ws") as sock:
        _speak(sock)
        _speak(sock)

    assert len(fake_store.conversations) == 1, fake_store.conversations
    assert len(fake_store.turns) == 2
    assert {t["conversation_id"] for t in fake_store.turns} == {1}


def test_separate_sockets_get_separate_conversations(client):
    http, fake_store = client
    with http.websocket_connect("/ws") as sock:
        _speak(sock)
    with http.websocket_connect("/ws") as sock:
        _speak(sock)

    assert len(fake_store.conversations) == 2


def test_interrupted_turn_is_not_recorded(client):
    """Barge-in cancels before `done`, so a half-spoken answer stays out."""
    http, fake_store = client
    with http.websocket_connect("/ws") as sock:
        sock.send_json({"type": "speech_start", "grade": "8.Sınıf", "subject": "Fen"})
        sock.send_bytes(b"\x00" * 50)
        sock.send_json({"type": "cancel"})
        _speak(sock)

    assert len(fake_store.turns) == 1
    assert fake_store.turns[0]["question"] == "soru1"


def test_a_broken_store_does_not_break_the_turn(client):
    """Persistence is best-effort: the student still gets their answer."""
    http, fake_store = client

    class BrokenStore(InMemoryConversationStore):
        async def start_conversation(self, **kw):
            raise RuntimeError("db down")

    ws_module.store = BrokenStore()
    kinds: list[str] = []

    def drain_until_done() -> None:
        """Record EVERY message, not just up to the first `done`.

        The write happens after `done` is sent, so a stray error lands in the
        stream *behind* it. A helper that stops at `done` would never see it —
        and this test would pass with the guard removed, proving nothing.
        """
        for _ in range(40):
            msg = sock.receive()
            if msg.get("bytes") is not None:
                kinds.append("audio")
                continue
            kinds.append(json.loads(msg["text"])["type"])
            if kinds[-1] == "done":
                return

    try:
        with http.websocket_connect("/ws") as sock:
            for _ in range(2):
                sock.send_json({"type": "speech_start", "grade": "8.Sınıf",
                                "subject": "Fen Bilimleri"})
                sock.send_bytes(b"\x00" * 50)
                sock.send_json({"type": "speech_end"})
                drain_until_done()
    finally:
        ws_module.store = fake_store

    assert "answer_text" in kinds
    assert "audio" in kinds
    assert kinds.count("done") == 2, kinds
    assert "error" not in kinds, kinds
