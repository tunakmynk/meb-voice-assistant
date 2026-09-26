"""TTS adapter — text to audio.

The last link in ADR-03's streaming chain. The orchestrator splits the LLM
token stream at sentence boundaries and hands each sentence here separately,
so the student hears sentence one while the model is still writing the rest.

The contract is therefore not "speak the whole answer" but **speak one piece,
streaming**. Both parts matter, and both were measured (plan 2.1):

    tam dosya (mp3/pcm/wav)   ~1700 ms ilk bayt
    akisli mp3                 1282 ms
    akisli pcm                  683 ms   <- secildi

Waiting for a complete file wastes a second before any sound moves. PCM wins
over mp3 because there are no frames to buffer — samples leave as they are
generated — and it also removes the mp3 decoder from the device: raw samples
feed straight into I2S. The cost is roughly 3x the bytes, which is nothing on
a local network.
"""

from __future__ import annotations

from collections.abc import AsyncIterator
from typing import Protocol

from openai import AsyncOpenAI

from app.config import settings

# Bytes per streamed frame. Small enough that the first frame leaves quickly,
# large enough not to flood the socket with tiny writes.
CHUNK_BYTES = 8192


class TtsAdapter(Protocol):
    def stream(self, text: str) -> AsyncIterator[bytes]:
        """Yields audio bytes as they are produced, not after the file is done."""
        ...

    async def synthesize(self, text: str) -> bytes:
        """Whole-clip convenience for callers that cannot stream."""
        ...


class OpenAiTtsAdapter:
    """TtsAdapter implementation for OpenAI (ADR-08)."""

    def __init__(self) -> None:
        # Same lazy-construction reason as stt.py/llm.py: the constructor
        # validates the key, and importing this module must not require one.
        self._client: AsyncOpenAI | None = None

    def _get_client(self) -> AsyncOpenAI:
        if self._client is None:
            self._client = AsyncOpenAI(api_key=settings.openai_api_key)
        return self._client

    async def stream(self, text: str) -> AsyncIterator[bytes]:
        async with self._get_client().audio.speech.with_streaming_response.create(
            model=settings.tts_model,
            voice=settings.tts_voice,
            input=text,
            response_format=settings.tts_format,
        ) as response:
            async for chunk in response.iter_bytes(CHUNK_BYTES):
                if chunk:
                    yield chunk

    async def synthesize(self, text: str) -> bytes:
        return b"".join([chunk async for chunk in self.stream(text)])


tts_adapter = OpenAiTtsAdapter()
