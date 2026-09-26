"""Multi-turn conversation over one WebSocket session.

Diagnoses whether short-term context (doc 4.2) actually works: the second
question deliberately refers back with "bununla", so it is meaningless on its
own. If the assistant answers it sensibly, context is carried; if it refuses
or answers something unrelated, it is not.

Usage:
    .venv\\Scripts\\python.exe scripts\\e2e_multiturn.py
"""

from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

import websockets

BACKEND_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BACKEND_DIR))

from scripts.e2e_turn import synth_question  # noqa: E402

WS_URL = "ws://127.0.0.1:8000/ws"

# 2. tur: manuel testte kalan soru — hicbir kelime listesi yakalamazdi.
# 3. tur: gercek konu degisimi — eski konuyu miras ALMAMALI.
TURNS = [
    "ısı ve sıcaklık arasındaki fark nedir",
    "konuyla ilgili birkaç örnek verebilir misin",
    "bununla ilgili daha farklı örnekler verebilir misin",
    "basınç birimi nedir",
]


async def one_turn(ws, pcm: bytes, grade: str, subject: str) -> None:
    await ws.send(json.dumps({"type": "speech_start", "grade": grade, "subject": subject}))
    for i in range(0, len(pcm), 32768):
        await ws.send(pcm[i:i + 32768])
    await ws.send(json.dumps({"type": "speech_end"}))

    while True:
        msg = await asyncio.wait_for(ws.recv(), timeout=120)
        if isinstance(msg, bytes):
            continue
        event = json.loads(msg)
        if event["type"] == "transcript":
            print(f"  SORU     : {event['text']}")
        elif event["type"] == "answer_text" and not event.get("partial", False):
            flag = "  [KAYNAK_YOK]" if event.get("no_source") else ""
            print(f"  CEVAP{flag}: {event['text']}")
            if event.get("source"):
                print(f"  KAYNAK   : {event['source']['breadcrumb']}")
        elif event["type"] == "search_query":
            print(f"  ARAMA    : {event['text']}")
        elif event["type"] == "error":
            print(f"  HATA: {event['code']} — {event['message']}")
            return
        elif event["type"] == "done":
            lat = event.get("latencies_ms", {})
            total = sum(lat.values())
            print("  SURE     : " + ", ".join(f"{k} {v}" for k, v in lat.items())
                  + f"  | toplam {total} ms")
            return


async def main() -> None:
    grade, subject = "8.Sınıf", "Fen Bilimleri"
    print("Sorular sese cevriliyor...")
    audios = [await synth_question(q) for q in TURNS]

    async with websockets.connect(WS_URL, max_size=None) as ws:
        for i, pcm in enumerate(audios, 1):
            print(f"\n--- TUR {i} ---")
            await one_turn(ws, pcm, grade, subject)


if __name__ == "__main__":
    asyncio.run(main())
