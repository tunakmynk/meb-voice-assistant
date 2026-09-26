"""Orchestrator tests — no network, no API credits.

The focus is detect_no_source, because that is where a subtle bug would be
invisible in production: a missed marker means the student hears a confident
fabricated answer.
"""

from __future__ import annotations

import asyncio

from app.adapters.llm import NO_SOURCE_MARKER
from app.orchestrator.pipeline import Orchestrator, detect_no_source
from tests.fakes import FakeLlm, FakeRetriever, FakeStt, FakeTts


async def _collect(stream) -> str:
    return "".join([d async for d in stream])


def _detect(answer: str, chunk_size: int = 3) -> tuple[bool, str]:
    """Run detect_no_source over a faked stream, return (flag, replayed text)."""

    async def run():
        llm = FakeLlm(answer, chunk_size=chunk_size)
        stream = llm.stream_answer("q", [])
        flag, rest = await detect_no_source(stream)
        return flag, await _collect(rest)

    return asyncio.run(run())


def test_normal_answer_is_not_flagged():
    flag, text = _detect("Isı bir enerji türüdür, sıcaklık ise ölçüsüdür.")
    assert flag is False
    assert text == "Isı bir enerji türüdür, sıcaklık ise ölçüsüdür."


def test_marker_is_detected():
    answer = f"{NO_SOURCE_MARKER}\nBunu kitabında bulamadım."
    flag, text = _detect(answer)
    assert flag is True
    assert text == answer  # nothing is swallowed


def test_no_token_is_lost_at_any_chunk_size():
    """Buffering must replay exactly, whatever the token boundaries are.

    chunk_size=1 is the worst case: the marker arrives one letter at a time,
    so the prefix check runs on every single character.
    """
    answer = "Isı bir enerji türüdür."
    for size in (1, 2, 5, 100):
        flag, text = _detect(answer, chunk_size=size)
        assert flag is False
        assert text == answer, f"chunk_size={size} lost tokens"


def test_word_starting_like_the_marker_is_not_a_false_positive():
    """'KAYNAK' alone must not trip the check — only the full marker does."""
    flag, text = _detect("KAYNAK olarak kitabındaki 5. ünite kullanıldı.", chunk_size=1)
    assert flag is False
    assert text.startswith("KAYNAK olarak")


def test_leading_whitespace_does_not_hide_the_marker():
    flag, _ = _detect(f"\n  {NO_SOURCE_MARKER}\nBulamadım.", chunk_size=1)
    assert flag is True


def test_empty_answer_is_not_flagged():
    flag, text = _detect("")
    assert flag is False
    assert text == ""


def test_run_turn_wires_stages_and_times_them():
    stt = FakeStt("yoğunluk nasıl hesaplanır")
    retriever = FakeRetriever()
    orch = Orchestrator(stt, retriever, FakeLlm("Yoğunluk kütle bölü hacimdir."), FakeTts())

    result = asyncio.run(orch.run_turn(b"pcm", grade="6.Sınıf", subject="Fen Bilimleri"))

    assert result.question == "yoğunluk nasıl hesaplanır"
    assert result.answer == "Yoğunluk kütle bölü hacimdir."
    assert result.no_source is False
    # The transcript, not the raw audio, is what reaches retrieval — and the
    # profile must be passed through untouched (plan 0.4: exact strings only).
    assert retriever.calls == [("yoğunluk nasıl hesaplanır", "6.Sınıf", "Fen Bilimleri")]
    assert set(result.latencies_ms) == {"stt", "retrieval", "llm_first_token", "llm_total"}


def test_run_turn_flags_no_source():
    orch = Orchestrator(
        FakeStt(), FakeRetriever(),
        FakeLlm(f"{NO_SOURCE_MARKER}\nBunu kitabında bulamadım."), FakeTts(),
    )
    result = asyncio.run(orch.run_turn(b"pcm", grade="6.Sınıf"))
    assert result.no_source is True


def _events(answer: str, chunks=None) -> list[dict]:
    tts = FakeTts()
    orch = Orchestrator(FakeStt(), FakeRetriever(chunks), FakeLlm(answer), tts)

    async def run():
        return [e async for e in orch.run_turn_events(b"pcm", grade="8.Sınıf",
                                                      subject="Fen Bilimleri")]

    out = asyncio.run(run())
    out[0]["_spoken"] = tts.spoken  # smuggle for assertions
    return out


def test_events_follow_the_device_contract():
    events = _events("Isı enerjidir. Sıcaklık ölçüdür.")
    kinds = [e["type"] for e in events]

    assert kinds[0] == "transcript"
    assert kinds[1] == "thinking"
    assert kinds[-1] == "done"
    assert "audio" in kinds
    # Audio must start before the turn ends, not all at the finish.
    assert kinds.index("audio") < len(kinds) - 2


def test_audio_is_streamed_per_sentence():
    """Each sentence goes to TTS separately — that is the whole point of 2.1."""
    events = _events("Isı enerjidir. Sıcaklık ölçüdür.")
    assert events[0]["_spoken"] == ["Isı enerjidir.", "Sıcaklık ölçüdür."]


def test_answer_text_accumulates_then_finalises():
    events = _events("Isı enerjidir. Sıcaklık ölçüdür.")
    texts = [e for e in events if e["type"] == "answer_text"]

    assert texts[0]["partial"] is True
    assert texts[-1]["partial"] is False
    assert texts[-1]["text"] == "Isı enerjidir. Sıcaklık ölçüdür."
    # Partials grow, so a device that overwrites its label shows the answer
    # building up rather than only the latest sentence.
    assert texts[0]["text"] in texts[-1]["text"]


def test_marker_line_is_never_spoken():
    """KAYNAK_YOK is protocol, not speech — the student must not hear it."""
    events = _events(f"{NO_SOURCE_MARKER}\nBunu kitabında bulamadım.")
    spoken = events[0]["_spoken"]

    assert all(NO_SOURCE_MARKER not in s for s in spoken), spoken
    assert any("bulamadım" in s.lower() for s in spoken), spoken

    final = [e for e in events if e["type"] == "answer_text"][-1]
    assert final["no_source"] is True
    assert final["source"] is None
