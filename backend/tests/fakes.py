"""Fake adapters for tests.

None of these inherit from the Protocols — a matching method signature is all
Python's structural typing needs, and a type checker verifies it statically.

Their real value showed up immediately: the OpenAI account ran out of credits,
and because the orchestrator only ever talks to the protocols (ADR-08), the
whole turn is still testable without a single paid API call.
"""

from __future__ import annotations

from collections.abc import AsyncIterator

from app.rag.retriever import RetrievedChunk


class FakeStt:
    def __init__(self, transcript: str = "ısı ve sıcaklık arasındaki fark nedir") -> None:
        self.transcript = transcript
        self.calls: list[bytes] = []
        self.subjects: list[str | None] = []

    async def transcribe(self, pcm_audio: bytes, *, subject: str | None = None) -> str:
        self.calls.append(pcm_audio)
        self.subjects.append(subject)
        return self.transcript


class FakeRetriever:
    def __init__(self, chunks: list[RetrievedChunk] | None = None) -> None:
        self.chunks = chunks if chunks is not None else [
            RetrievedChunk(
                chunk_id="1", text="Isı bir enerji türüdür.", score=0.83,
                breadcrumb="5. ÜNİTE > ISI VE MADDE", grade="8.Sınıf",
                subject="Fen Bilimleri",
            )
        ]
        self.calls: list[tuple[str, str | None, str | None]] = []

    async def search(self, query: str, *, grade=None, subject=None, **_):
        self.calls.append((query, grade, subject))
        return self.chunks


class FakeLlm:
    """Emits a fixed answer as token-sized pieces, like a real stream would."""

    def __init__(self, answer: str, chunk_size: int = 3,
                 rewritten: str | None = None) -> None:
        self.answer = answer
        self.chunk_size = chunk_size
        self.rewritten = rewritten
        self.rewrite_calls: list[tuple[str, list]] = []
        self.outlines: list[list[str] | None] = []

    async def rewrite_query(self, question: str, history: list,
                            outline: list[str] | None = None) -> str:
        self.rewrite_calls.append((question, history))
        self.outlines.append(outline)
        return self.rewritten if self.rewritten is not None else question

    def stream_answer(self, question, chunks, *, grade=None, history=None,
                      outline=None) -> AsyncIterator[str]:
        async def gen() -> AsyncIterator[str]:
            for i in range(0, len(self.answer), self.chunk_size):
                yield self.answer[i : i + self.chunk_size]

        return gen()


class FakeTts:
    """Records what was spoken and emits it as two frames.

    Two rather than one so tests notice if the pipeline mistakenly assumes a
    sentence arrives as a single audio message.
    """

    def __init__(self) -> None:
        self.spoken: list[str] = []

    async def stream(self, text: str) -> AsyncIterator[bytes]:
        self.spoken.append(text)
        data = text.encode()
        yield data[: len(data) // 2] or b"\x00"
        yield data[len(data) // 2 :]

    async def synthesize(self, text: str) -> bytes:
        return b"".join([c async for c in self.stream(text)])
