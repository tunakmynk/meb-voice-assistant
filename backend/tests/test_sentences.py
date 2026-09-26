"""Sentence splitter tests.

The cases are drawn from text the system actually produces: the eval answers
and the corpus breadcrumbs. A false split costs an extra TTS call and sounds
clipped; a missed split delays audio.
"""

from __future__ import annotations

import asyncio

from app.orchestrator.sentences import split_complete_sentences, stream_sentences


def _stream(text: str, chunk: int = 3) -> list[str]:
    async def gen():
        for i in range(0, len(text), chunk):
            yield text[i : i + chunk]

    async def run():
        return [s async for s in stream_sentences(gen())]

    return asyncio.run(run())


def test_splits_plain_sentences():
    out = _stream("Isı bir enerjidir. Sıcaklık ise ölçüdür. Bu fark önemlidir.")
    assert out == [
        "Isı bir enerjidir.",
        "Sıcaklık ise ölçüdür.",
        "Bu fark önemlidir.",
    ]


def test_dates_do_not_split():
    """Straight from a real answer: the 1923 dot must not end a sentence."""
    text = "Lozan Barış Antlaşması, 24 Temmuz 1923 tarihinde imzalandı. Savaş bitti."
    assert _stream(text) == [
        "Lozan Barış Antlaşması, 24 Temmuz 1923 tarihinde imzalandı.",
        "Savaş bitti.",
    ]


def test_ordinals_do_not_split():
    text = "Bu konu 8. sınıf kitabının 5. ünitesinde anlatılır. Devamı var."
    assert _stream(text) == [
        "Bu konu 8. sınıf kitabının 5. ünitesinde anlatılır.",
        "Devamı var.",
    ]


def test_abbreviation_does_not_split():
    """T.C. matters — it is literally part of a subject name in the corpus."""
    text = "T.C. İnkılap Tarihi dersinde işlenir. Ayrıca vb. konular da vardır."
    assert _stream(text) == [
        "T.C. İnkılap Tarihi dersinde işlenir.",
        "Ayrıca vb. konular da vardır.",
    ]


def test_question_and_exclamation_split():
    assert _stream("Bunu biliyor musun? Evet, çok kolay! Devam edelim.") == [
        "Bunu biliyor musun?",
        "Evet, çok kolay!",
        "Devam edelim.",
    ]


def test_trailing_text_without_punctuation_is_emitted():
    """The model can stop mid-sentence at max_tokens; nothing may be dropped."""
    assert _stream("Isı bir enerjidir. Sıcaklık ise") == [
        "Isı bir enerjidir.",
        "Sıcaklık ise",
    ]


def test_no_text_is_lost_at_any_chunk_size():
    text = "Isı enerjidir. 1923 yılında, 8. sınıfta T.C. tarihi okunur! Bitti."
    for size in (1, 2, 7, 500):
        joined = " ".join(_stream(text, chunk=size))
        for word in ("Isı", "1923", "8.", "T.C.", "Bitti."):
            assert word in joined, f"chunk_size={size} lost {word!r}"


def test_very_short_fragment_waits_for_more():
    """'Evet.' alone would cost a whole TTS round trip for one word."""
    sentences, remainder = split_complete_sentences("Evet. Bu doğru bir cevaptır.")
    assert sentences == ["Evet. Bu doğru bir cevaptır."]
    assert remainder == ""
