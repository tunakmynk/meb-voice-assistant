"""End-to-end turn over the real WebSocket (plan 1.7).

There is no microphone here, so the question audio is synthesised with TTS and
fed back in as if the device had recorded it. That makes the loop honest: the
bytes travel the same path a real utterance would (PCM -> WAV -> STT), and STT
has to actually recognise Turkish speech rather than being handed a string.

It also gives the latency baseline the design doc asks for: per-stage timings,
against the <=4 s to first audio target.

Usage (server must be running):
    .venv\\Scripts\\python.exe scripts\\e2e_turn.py
    .venv\\Scripts\\python.exe scripts\\e2e_turn.py "soru" 8.Sınıf "Fen Bilimleri"
"""

from __future__ import annotations

import array
import asyncio
import json
import sys
import time
from pathlib import Path

import websockets

BACKEND_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BACKEND_DIR))

from app.config import settings  # noqa: E402

WS_URL = "ws://127.0.0.1:8000/ws"
TTS_RATE = 24000  # OpenAI pcm output rate


async def synth_question(text: str) -> bytes:
    """Speak the question, returning 16 kHz mono 16-bit PCM."""
    from openai import AsyncOpenAI

    client = AsyncOpenAI(api_key=settings.openai_api_key)
    resp = await client.audio.speech.create(
        model=settings.tts_model, voice=settings.tts_voice,
        input=text, response_format="pcm",
    )
    return _resample(resp.content, TTS_RATE, settings.audio_sample_rate)


def _resample(pcm: bytes, src_rate: int, dst_rate: int) -> bytes:
    """Averaging downsample. Picking every Nth sample would alias badly and
    hand STT a harsher signal than a real microphone would."""
    if src_rate == dst_rate:
        return pcm
    samples = array.array("h")
    samples.frombytes(pcm)
    ratio = src_rate / dst_rate
    out = array.array("h")
    for i in range(int(len(samples) / ratio)):
        start, end = int(i * ratio), min(int((i + 1) * ratio), len(samples))
        window = samples[start:end] or samples[start:start + 1]
        out.append(int(sum(window) / len(window)))
    return out.tobytes()


async def main() -> None:
    question = sys.argv[1] if len(sys.argv) > 1 else "ısı ve sıcaklık arasındaki fark nedir"
    grade = sys.argv[2] if len(sys.argv) > 2 else "8.Sınıf"
    subject = sys.argv[3] if len(sys.argv) > 3 else "Fen Bilimleri"

    print(f"Soru sese cevriliyor: {question!r}")
    pcm = await synth_question(question)
    secs = len(pcm) / (settings.audio_sample_rate * settings.audio_sample_width)
    print(f"  {len(pcm)} bayt, ~{secs:.1f} sn ses\n")

    async with websockets.connect(WS_URL, max_size=None) as ws:
        await ws.send(json.dumps({"type": "speech_start", "grade": grade, "subject": subject}))

        # Send in 32 KB frames, as a device streaming while recording would.
        for i in range(0, len(pcm), 32768):
            await ws.send(pcm[i:i + 32768])

        t0 = time.perf_counter()
        await ws.send(json.dumps({"type": "speech_end"}))
        print(f"speech_end gonderildi | profil: {grade} / {subject}\n")

        audio_bytes = first_audio = 0
        while True:
            msg = await asyncio.wait_for(ws.recv(), timeout=120)
            elapsed = (time.perf_counter() - t0) * 1000

            if isinstance(msg, bytes):
                if not audio_bytes:
                    first_audio = elapsed
                audio_bytes += len(msg)
                print(f"  [{elapsed:7.0f} ms] audio  {len(msg)} bayt")
                continue

            event = json.loads(msg)
            kind = event["type"]

            if kind == "transcript":
                print(f"  [{elapsed:7.0f} ms] transcript: {event['text']!r}")
            elif kind == "thinking":
                print(f"  [{elapsed:7.0f} ms] thinking")
            elif kind == "answer_text":
                flag = " (KAYNAK_YOK)" if event.get("no_source") else ""
                print(f"  [{elapsed:7.0f} ms] answer_text{flag}:")
                print(f"      {event['text']}")
                if event.get("source"):
                    s = event["source"]
                    print(f"      kaynak: {s['grade']} / {s['subject']} / {s['breadcrumb']}")
            elif kind == "error":
                print(f"  [{elapsed:7.0f} ms] HATA {event['code']}: {event['message']}")
                return
            elif kind == "done":
                print(f"  [{elapsed:7.0f} ms] done")
                print("\n" + "=" * 62)
                for stage, ms in event["latencies_ms"].items():
                    print(f"  {stage:<16} {ms:>6} ms")
                print(f"  {'-'*30}")
                print(f"  {'ILK SESE KADAR':<16} {first_audio:>6.0f} ms  "
                      f"(hedef <=4000)  {'GECTI' if first_audio <= 4000 else 'ASILDI'}")
                print(f"  {'ses toplam':<16} {audio_bytes:>6} bayt")
                print("=" * 62)
                return


if __name__ == "__main__":
    asyncio.run(main())
