"""Barge-in tests (ADR-04, plan 2.2).

The bug these lock down: asking a second question while the first answer was
still being spoken produced *both* answers at once, their audio interleaved on
the same socket.

Three separate defects had to line up for that, and each is covered here:

  1. `speech_start` did not cancel the running turn.
  2. `speech_end` overwrote `session.task`, so the old task kept running and
     kept writing to the socket.
  3. `cancel` called `Task.cancel()` without awaiting it — cancellation is only
     a request, and the task can still emit frames before it honours one.
"""

from __future__ import annotations

import asyncio

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from app.gateway import ws as ws_module


class SlowOrchestrator:
    """Emits a turn slowly enough that a second one can be started mid-flight.

    Records which turns actually ran to completion, so a test can tell
    "cancelled" apart from "finished quietly".
    """

    def __init__(self) -> None:
        self.started: list[str] = []
        self.completed: list[str] = []
        self._n = 0

    async def run_turn_events(self, pcm_audio, *, grade=None, subject=None, history=None):
        self._n += 1
        tag = f"tur{self._n}"
        self.started.append(tag)

        yield {"type": "transcript", "text": tag}
        yield {"type": "thinking"}
        for _ in range(20):
            await asyncio.sleep(0.05)
            yield {"type": "audio", "data": tag.encode()}
        yield {"type": "answer_text", "text": tag, "partial": False,
               "no_source": False, "source": None}
        self.completed.append(tag)
        yield {"type": "done", "latencies_ms": {}, "no_source": False}


@pytest.fixture
def client(monkeypatch):
    orch = SlowOrchestrator()
    monkeypatch.setattr(ws_module, "orchestrator", orch)

    app = FastAPI()
    app.include_router(ws_module.router)
    with TestClient(app) as c:
        yield c, orch


def _speak(sock, grade="8.Sınıf"):
    sock.send_json({"type": "speech_start", "grade": grade, "subject": "Fen Bilimleri"})
    sock.send_bytes(b"\x00\x01" * 100)
    sock.send_json({"type": "speech_end"})


def _drain_until(sock, kinds, limit=200):
    """Collect messages until one of `kinds` arrives (or `limit` messages)."""
    seen = []
    for _ in range(limit):
        msg = sock.receive()
        if "bytes" in msg and msg["bytes"] is not None:
            seen.append(("audio", msg["bytes"].decode()))
            continue
        import json
        event = json.loads(msg["text"])
        seen.append((event["type"], event.get("text")))
        if event["type"] in kinds:
            return seen
    return seen


def test_second_question_cancels_the_first(client):
    sock_client, orch = client
    with sock_client.websocket_connect("/ws") as sock:
        _speak(sock)
        _drain_until(sock, {"thinking"})

        # Interrupt while turn 1 is still streaming audio.
        _speak(sock)
        seen = _drain_until(sock, {"done"}, limit=300)

    assert orch.started == ["tur1", "tur2"]
    # The whole point: turn 1 must never reach the end.
    assert orch.completed == ["tur2"], orch.completed


def test_no_audio_from_the_old_turn_after_the_new_one_starts(client):
    """The mixing the user heard: frames from both turns on one socket."""
    sock_client, orch = client
    with sock_client.websocket_connect("/ws") as sock:
        _speak(sock)
        _drain_until(sock, {"thinking"})
        _speak(sock)
        seen = _drain_until(sock, {"done"}, limit=300)

    kinds = [k for k, _ in seen]
    assert "cancelled" in kinds, "device was not told to flush its buffer"

    after = seen[kinds.index("cancelled") + 1:]
    stale = [v for k, v in after if k == "audio" and v == "tur1"]
    assert stale == [], f"eski turdan {len(stale)} ses paketi sizdi"


def test_explicit_cancel_stops_the_turn(client):
    sock_client, orch = client
    with sock_client.websocket_connect("/ws") as sock:
        _speak(sock)
        _drain_until(sock, {"thinking"})

        sock.send_json({"type": "cancel"})
        seen = _drain_until(sock, {"cancelled"}, limit=300)

    assert orch.completed == []
    assert any(k == "cancelled" for k, _ in seen)


def test_cancel_with_nothing_running_is_harmless(client):
    sock_client, orch = client
    with sock_client.websocket_connect("/ws") as sock:
        sock.send_json({"type": "cancel"})
        _speak(sock)
        _drain_until(sock, {"done"}, limit=300)

    assert orch.completed == ["tur1"]


class PartialThenSlow:
    """Cevap metnini ERKEN, sesi yavaş üretir — gerçek akışın sırası bu.

    SlowOrchestrator answer_text'i sesin sonunda üretiyor; o sırayla araya
    girildiğinde ortada hiç cevap metni olmuyor ve testin ölçmek istediği
    durum hiç oluşmuyor. Gerçek pipeline'da cevap metni TTS çalarken geliyor,
    yarıda kesilen turun hatırlanacak bir içeriği bu yüzden oluyor.
    """

    def __init__(self) -> None:
        self.histories: list[list[dict]] = []
        self._n = 0

    async def run_turn_events(self, pcm_audio, *, grade=None, subject=None,
                              history=None):
        self._n += 1
        self.histories.append(list(history or []))
        tag = f"tur{self._n}"

        yield {"type": "transcript", "text": f"{tag} sorusu"}
        yield {"type": "thinking"}
        yield {"type": "answer_text", "text": f"{tag} cevabinin ilk yarisi",
               "partial": True, "no_source": False, "source": None}
        for _ in range(20):
            await asyncio.sleep(0.05)
            yield {"type": "audio", "data": tag.encode()}
        yield {"type": "answer_text", "text": f"{tag} tam cevap",
               "partial": False, "no_source": False, "source": None}
        yield {"type": "done", "latencies_ms": {}, "no_source": False}


@pytest.fixture
def partial_client(monkeypatch):
    orch = PartialThenSlow()
    monkeypatch.setattr(ws_module, "orchestrator", orch)

    app = FastAPI()
    app.include_router(ws_module.router)
    with TestClient(app) as c:
        yield c, orch


def test_interrupted_answer_stays_in_history(partial_client):
    """Yarıda kesilen tur, sonraki turun geçmişinde görünmeli.

    Öğrenci cevabın bir kısmını DUYUYOR ve sonraki sorusu neredeyse her zaman
    ona atıfta bulunuyor ("bununla ilgili"). Kesilen tur atılırsa yeniden
    yazma adımının çözecek bir şeyi kalmıyor, arama içerik kelimesi olmayan
    bir soruyla yapılıyor ve alakasız kaynak dönüyor — yanlış breadcrumb'lı
    yanlış cevap, cevapsızlıktan kötü.
    """
    sock_client, orch = partial_client
    with sock_client.websocket_connect("/ws") as sock:
        _speak(sock)
        # answer_text'e kadar: kesildiğinde hatırlanacak metin ancak bu
        # olaydan sonra var oluyor. "thinking"e kadar beklemek yetmez,
        # çünkü o sırada cevap henüz boş.
        _drain_until(sock, {"answer_text"})

        _speak(sock)                            # araya gir
        _drain_until(sock, {"transcript"}, limit=300)   # 2. tur başladı

    assert len(orch.histories) == 2
    gecmis = orch.histories[1]
    assert [m["role"] for m in gecmis] == ["user", "assistant"]
    assert gecmis[0]["content"] == "tur1 sorusu"
    # Duyulan kısım korunmalı...
    assert "tur1 cevabinin ilk yarisi" in gecmis[1]["content"]
    # ...ve yarıda kaldığı belli olmalı, yoksa model cevabı tamamladığını
    # sanıp "devam et" denince yeni bir konuya geçer.
    assert "kesti" in gecmis[1]["content"]
