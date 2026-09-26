"""Sentence splitting over a token stream (ADR-03, plan 2.1).

The measured baseline (plan 1.7) was 11.0 s to first audio, of which 6.0 s was
TTS synthesising the whole answer before emitting a byte. Splitting the LLM
stream at sentence boundaries lets the first sentence reach TTS while the
model is still writing the rest.

Turkish makes the naive `split(".")` wrong in ways that matter here, because
every false split costs a separate TTS call and every missed split delays
audio:

    "24 Temmuz 1923 tarihinde"   -> digits around the dot, not a boundary
    "8. Sınıf", "5. BÖLÜM"       -> ordinal numbers end with a dot
    "T.C. İnkılap Tarihi"        -> abbreviation, and it IS a real subject name
    "vb.", "örn."                -> common abbreviations

So the rule is: a boundary is sentence-ending punctuation, followed by
whitespace, that is not preceded by a known abbreviation or a bare number.
"""

from __future__ import annotations

import re
from collections.abc import AsyncIterator

# Abbreviations that end in a dot without ending a sentence. Lowercased at
# comparison time; "T.C." is here because 8th grade's subject name contains it.
_ABBREVIATIONS = {
    "vb", "vs", "örn", "bkz", "yy", "sy", "bkn", "dr", "prof", "doç",
    "t.c", "m.ö", "m.s", "a.g.e", "no",
}

_BOUNDARY = re.compile(r"([.!?…])(\s+|$)")

# Minimum characters before we are willing to cut. A two-word fragment costs a
# whole TTS round trip and sounds clipped; below this we wait for more text.
MIN_SENTENCE_CHARS = 12


def _is_real_boundary(text: str, dot_index: int) -> bool:
    """Is the punctuation at dot_index actually ending a sentence?"""
    if text[dot_index] in "!?…":
        return True

    before = text[:dot_index]
    if not before:
        return False

    # "1923." / "5." — a bare number before the dot is an ordinal or a date
    # fragment, not a sentence end.
    tail = re.search(r"(\S+)$", before)
    if tail is None:
        return False
    word = tail.group(1).lower()

    if word.rstrip(".").isdigit():
        return False

    # Strip the trailing dot(s) so "t.c" matches the abbreviation set.
    if word.rstrip(".") in _ABBREVIATIONS:
        return False

    # Single letter before a dot is almost always an initial ("A. Kemal").
    return len(word.rstrip(".")) != 1


def split_complete_sentences(buffer: str) -> tuple[list[str], str]:
    """Pull finished sentences out of a buffer.

    Returns (sentences, remainder). The remainder is text that might still
    grow into a sentence, so the caller keeps it for the next token.
    """
    sentences: list[str] = []
    start = 0

    for match in _BOUNDARY.finditer(buffer):
        end = match.end(1)
        if not _is_real_boundary(buffer, match.start(1)):
            continue
        candidate = buffer[start:end].strip()
        if len(candidate) < MIN_SENTENCE_CHARS:
            continue
        sentences.append(candidate)
        start = match.end()

    return sentences, buffer[start:]


async def stream_sentences(tokens: AsyncIterator[str]) -> AsyncIterator[str]:
    """Turn a token stream into a sentence stream, emitting as soon as each
    sentence completes rather than waiting for the answer to finish."""
    buffer = ""

    async for token in tokens:
        buffer += token
        sentences, buffer = split_complete_sentences(buffer)
        for sentence in sentences:
            yield sentence

    tail = buffer.strip()
    if tail:
        yield tail
