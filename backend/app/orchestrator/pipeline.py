"""Turn orchestration: STT -> retrieval -> LLM -> TTS.

This is the only place that knows the shape of a turn. Per ADR-08 it talks to
providers exclusively through the adapter protocols, never through the OpenAI
SDK — which is also why it can be tested end to end with fakes and no API
credits.

Two things it owns beyond wiring:

1. **Latency accounting.** Each stage is timed separately, because "the turn
   took 6 seconds" is not actionable but "STT took 3.1s" is. The target from
   the design doc is <=4s to first audio.

2. **The KAYNAK_YOK decision.** Plan 0.5 established that retrieval always
   returns something and no score threshold can separate relevant from
   irrelevant context, so the model is the one that decides. Detecting its
   answer must be deterministic, not string-matching on prose.
"""

from __future__ import annotations

import time
from collections.abc import AsyncIterator
from dataclasses import dataclass, field

from app.adapters.llm import NO_SOURCE_MARKER
from app.orchestrator.query import build_retrieval_query
from app.rag.outline import book_outline
from app.orchestrator.sentences import stream_sentences
from app.rag.retriever import RetrievedChunk


@dataclass
class TurnResult:
    question: str
    answer: str
    chunks: list[RetrievedChunk]
    no_source: bool
    latencies_ms: dict[str, int] = field(default_factory=dict)


class _Stopwatch:
    """Records elapsed milliseconds per stage into a dict."""

    def __init__(self) -> None:
        self.marks: dict[str, int] = {}
        self._t0 = time.perf_counter()

    def mark(self, name: str) -> None:
        now = time.perf_counter()
        self.marks[name] = int((now - self._t0) * 1000)
        self._t0 = now


async def detect_no_source(
    stream: AsyncIterator[str],
) -> tuple[bool, AsyncIterator[str]]:
    """Decide whether the answer is a refusal, without waiting for a sentence.

    The naive reading of "check the first line" is that we must buffer until a
    newline arrives — which would delay the first spoken sentence and fight
    ADR-03. It doesn't, because we only need to buffer until the stream
    *diverges* from the marker.

    If the answer begins "Isı bir enerji...", the very first chunk already
    fails to prefix-match "KAYNAK_YOK", so we release immediately: the cost is
    one token, not one sentence. We only keep buffering in the case where the
    text still could be the marker — and if it turns out to be one, there is
    no answer to speak anyway.

    Returns (no_source, replayed_stream). The returned stream yields whatever
    was buffered first, so no token is lost.
    """
    buffer = ""

    async for delta in stream:
        buffer += delta
        stripped = buffer.lstrip()

        if not stripped:
            continue  # only whitespace so far, undecidable

        if stripped.startswith(NO_SOURCE_MARKER):
            return True, _replay(buffer, stream)

        if not NO_SOURCE_MARKER.startswith(stripped[: len(NO_SOURCE_MARKER)]):
            # Diverged — this cannot become the marker. Release now.
            return False, _replay(buffer, stream)

    # Stream ended while still a prefix of the marker (e.g. empty answer).
    return buffer.strip() == NO_SOURCE_MARKER, _replay(buffer, _empty())


async def _empty() -> AsyncIterator[str]:
    return
    yield  # pragma: no cover — makes this an async generator


async def _drop_marker_line(stream: AsyncIterator[str]) -> AsyncIterator[str]:
    """Strip the KAYNAK_YOK line, forwarding only the sentence after it.

    The marker is protocol, not speech. detect_no_source has already buffered
    it, so the first chunk here still contains it.
    """
    dropped = False
    async for delta in stream:
        if dropped:
            yield delta
            continue
        if "\n" in delta:
            _, rest = delta.split("\n", 1)
            dropped = True
            if rest.strip():
                yield rest
    # A marker with no following sentence yields nothing; the caller
    # substitutes a default message.


async def _replay(buffered: str, rest: AsyncIterator[str]) -> AsyncIterator[str]:
    if buffered:
        yield buffered
    async for delta in rest:
        yield delta


class Orchestrator:
    """Runs one turn. Adapters are injected so tests can pass fakes."""

    def __init__(self, stt, retriever, llm, tts) -> None:
        self._stt = stt
        self._retriever = retriever
        self._llm = llm
        self._tts = tts

    async def _outline(self, grade: str | None,
                       subject: str | None) -> list[str]:
        """Kitabın bölüm başlıkları; alınamazsa boş liste.

        Sahte retriever'ların (testler) Qdrant istemcisi yok. Anahat bir
        iyileştirme olduğu için yokluğunda tur normal akışına devam ediyor
        — konum sorusu cevapsız kalır ama başka hiçbir şey bozulmaz.
        """
        client = getattr(self._retriever, "_client", None)
        if client is None:
            return []
        return await book_outline(client, grade, subject)

    async def run_turn(
        self,
        pcm_audio: bytes,
        *,
        grade: str | None = None,
        subject: str | None = None,
        history: list[dict[str, str]] | None = None,
    ) -> TurnResult:
        watch = _Stopwatch()

        question = await self._stt.transcribe(pcm_audio, subject=subject)
        watch.mark("stt")

        outline = await self._outline(grade, subject)
        search_text = await build_retrieval_query(question, history, self._llm, outline)
        chunks = await self._retriever.search(search_text, grade=grade, subject=subject)
        watch.mark("retrieval")

        stream = self._llm.stream_answer(question, chunks, grade=grade,
                                         history=history, outline=outline)
        no_source, stream = await detect_no_source(stream)
        watch.mark("llm_first_token")

        parts: list[str] = []
        async for delta in stream:
            parts.append(delta)
        watch.mark("llm_total")

        answer = "".join(parts)

        return TurnResult(
            question=question,
            answer=answer,
            chunks=chunks,
            no_source=no_source,
            latencies_ms=watch.marks,
        )

    async def run_turn_events(
        self,
        pcm_audio: bytes,
        *,
        grade: str | None = None,
        subject: str | None = None,
        history: list[dict[str, str]] | None = None,
    ) -> AsyncIterator[dict]:
        """Same turn, emitted as the events of the device contract (doc 3.2).

        The gateway stays dumb — it only forwards these onto the socket — so
        the turn's shape is testable without a WebSocket, and the panel can
        later consume the identical stream.
        """
        watch = _Stopwatch()

        # Ders bilgisi STT'ye ipucu olarak gidiyor: cihaz profili zaten
        # gönderiyor, kullanmamak boşa harcamak olurdu. Kısa terimlerin
        # ("ısı" -> "su"/"sıvı") yanlış duyulmasını azaltıyor.
        question = await self._stt.transcribe(pcm_audio, subject=subject)
        watch.mark("stt")
        yield {"type": "transcript", "text": question}
        yield {"type": "thinking"}

        # Follow-ups carry no content words of their own, so references are
        # resolved before searching. Without this the LLM gets history but
        # retrieval does not, and refuses on junk sources. Costs a small call,
        # and only when there is history to refer back to.
        outline = await self._outline(grade, subject)
        search_text = await build_retrieval_query(question, history, self._llm, outline)
        if search_text != question:
            yield {"type": "search_query", "text": search_text}
        chunks = await self._retriever.search(search_text, grade=grade, subject=subject)
        watch.mark("retrieval")

        stream = self._llm.stream_answer(question, chunks, grade=grade,
                                         history=history, outline=outline)
        no_source, stream = await detect_no_source(stream)
        watch.mark("llm_first_token")

        top = chunks[0] if chunks else None
        source = None if (no_source or top is None) else {
            "grade": top.grade,
            "subject": top.subject,
            "breadcrumb": top.breadcrumb,
        }

        # The marker is a protocol token, never something a student hears — so
        # it is stripped before the text reaches the splitter or TTS.
        if no_source:
            stream = _drop_marker_line(stream)

        # ADR-03: each finished sentence goes to TTS while the model is still
        # writing the rest. Plan 1.7 measured why — synthesising the whole
        # answer in one call cost 6.0 s before a single byte moved.
        spoken_parts: list[str] = []
        first_audio_marked = False

        async for sentence in stream_sentences(stream):
            spoken_parts.append(sentence)
            # Accumulated text, so a device that simply overwrites its label
            # shows the answer growing rather than only the latest sentence.
            yield {
                "type": "answer_text",
                "text": " ".join(spoken_parts),
                "partial": True,
                "no_source": no_source,
                "source": source,
            }

            # Forward each frame the moment it arrives — waiting for the
            # sentence's full clip costs ~1.2 s of silence (plan 2.1).
            async for frame in self._tts.stream(sentence):
                if not first_audio_marked:
                    watch.mark("tts_first_byte")
                    first_audio_marked = True
                yield {"type": "audio", "data": frame}

        answer = " ".join(spoken_parts)
        watch.mark("tts_total")

        yield {
            "type": "answer_text",
            "text": answer,
            "partial": False,
            "no_source": no_source,
            "source": source,
        }
        yield {
            "type": "done",
            "latencies_ms": watch.marks,
            "no_source": no_source,
            # Which chunks the answer rested on. The panel shows these and the
            # store records them (ADR-07); the text is left out because it is
            # already in Qdrant and would bloat every row.
            "retrieved": [
                {
                    "chunk_id": c.chunk_id,
                    "breadcrumb": c.breadcrumb,
                    "grade": c.grade,
                    "subject": c.subject,
                    "score": round(c.score, 4),
                }
                for c in chunks
            ],
        }
