"""Retrieval query construction tests.

The rewrite itself is the model's judgement, so these cover the wiring around
it: when it is called at all, and what happens when it fails.
"""

from __future__ import annotations

import asyncio

import pytest

from app.orchestrator.query import build_retrieval_query
from tests.fakes import FakeLlm

HISTORY = [
    {"role": "user", "content": "ısı ve sıcaklık arasındaki fark nedir"},
    {"role": "assistant", "content": "Isı bir enerji transferidir..."},
]


def _build(question, history, llm):
    return asyncio.run(build_retrieval_query(question, history, llm))


def test_first_question_skips_the_rewrite():
    """No history means nothing to refer back to — do not pay for a call."""
    llm = FakeLlm("cevap")
    assert _build("ısı nedir", [], llm) == "ısı nedir"
    assert _build("ısı nedir", None, llm) == "ısı nedir"
    assert llm.rewrite_calls == []


def test_followup_uses_the_rewritten_query():
    llm = FakeLlm("cevap", rewritten="ısı ve sıcaklık farkıyla ilgili örnek verir misin")
    result = _build("konuyla ilgili örnek verir misin", HISTORY, llm)

    assert result == "ısı ve sıcaklık farkıyla ilgili örnek verir misin"
    assert llm.rewrite_calls[0][0] == "konuyla ilgili örnek verir misin"


def test_rewrite_failure_falls_back_to_the_raw_question():
    """A demo must not die because one extra call failed."""

    class BrokenLlm(FakeLlm):
        async def rewrite_query(self, question, history):
            raise RuntimeError("upstream down")

    assert _build("bunu açıklar mısın", HISTORY, BrokenLlm("x")) == "bunu açıklar mısın"


def test_history_is_passed_through_to_the_rewriter():
    llm = FakeLlm("cevap")
    _build("bunu açıklar mısın", HISTORY, llm)
    assert llm.rewrite_calls[0][1] == HISTORY
