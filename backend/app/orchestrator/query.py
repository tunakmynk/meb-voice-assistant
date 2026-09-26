"""Retrieval query construction for follow-up questions.

A measured bug: short-term context reached the LLM but never reached
retrieval. Asked "peki bununla ilgili bana bir örnek verir misin" after a
question about ısı/sıcaklık, the search saw only the follow-up — which has no
content words at all — and returned Solunum and Fiziksel Değişimler. The model
then refused, correctly, because the sources it was handed were irrelevant.

**Why this needs the LLM rather than a word list.** Two things were tried and
measured first:

1. *Always* prepending the previous question. This destroys real topic
   changes. With ısı/sıcaklık in history, searching "basınç birimi nedir"
   returned three heat chunks and **zero** pressure chunks — a silent wrong
   answer, the worst failure mode we have.

2. A keyword heuristic (`bu`/`şu`/`o` family, `peki`, `ayrıca`). It missed
   "Konuyla ilgili birkaç örnek verebilir misin?" in real testing, and would
   equally miss "az önce anlattığın", "aynı konuda", "söylediğin şey".
   Turkish refers back in unbounded ways; the list can never be finished.

So the judgement is left to the model, which already reads the conversation.
The cost is one small call (~700 ms, measured) and it is paid **only when
there is history** — the first question of a session is unaffected.
"""

from __future__ import annotations

import logging

log = logging.getLogger(__name__)


async def build_retrieval_query(
    question: str,
    history: list[dict[str, str]] | None,
    llm,
    outline: list[str] | None = None,
) -> str:
    """The text actually sent to the retriever.

    A failed rewrite falls back to the raw question rather than dropping the
    turn — a slightly worse search beats an error mid-demo.

    **Why the outline also triggers a rewrite.** The early return used to be
    "no history, nothing to resolve". That was wrong for a second kind of
    reference: position in the book. "Bu kitabın son kısmındaki konuyu anlat"
    is usually the *first* question of a session, so the rewrite never ran and
    retrieval searched for the literal words — which match nothing, because
    "son kısım" has no semantic similarity to the last section's content. With
    the outline in hand the model resolves it to a real section name.

    The cost is one small call on the first question of a session; every later
    question already paid it. If that latency ever matters more than answering
    positional questions, dropping `or outline` here restores the old
    behaviour.
    """
    if not history and not outline:
        return question

    try:
        return await llm.rewrite_query(question, history or [], outline)
    except Exception:
        log.warning("query rewrite failed, using raw question", exc_info=True)
        return question
