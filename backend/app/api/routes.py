"""REST endpoints shared by device and panel (ADR-05).

`GET /catalog` returns a **grade -> subject[] mapping**, not two flat lists.
Plan 0.4 measured why: subject names are not global. The 8th grade book is
"T.C. İnkılap Tarihi ve Atatürkçülük" while the 12th grade one is "İnkılap
Tarihi ve Atatürkçülük", and 7th grade has no Matematik at all. A flat list
would let a student pick a string that exists in the corpus but not for their
grade — and the filter would return zero results without raising anything.

The values are read from Qdrant rather than hardcoded, so the catalog cannot
drift from what is actually searchable. Nothing is normalised: the payload
string is the only correct value to send back in a query.
"""

from __future__ import annotations

import logging

from fastapi import APIRouter
from qdrant_client import models

from app.config import settings
from app.conversation.store import store
from app.rag.retriever import retriever

log = logging.getLogger(__name__)
router = APIRouter()

# The collection is static at runtime, so the catalog is built once.
_cache: dict[str, list[str]] | None = None


def _grade_sort_key(grade: str) -> int:
    try:
        return int(grade.split(".")[0])
    except (ValueError, IndexError):
        return 99


async def build_catalog() -> dict[str, list[str]]:
    client = retriever._client
    collection = settings.qdrant_collection

    grades = await client.facet(collection_name=collection, key="grade", limit=100)
    catalog: dict[str, list[str]] = {}

    for hit in grades.hits:
        grade = str(hit.value)
        subjects = await client.facet(
            collection_name=collection,
            key="subject",
            limit=100,
            facet_filter=models.Filter(must=[
                models.FieldCondition(key="grade", match=models.MatchValue(value=grade))
            ]),
        )
        catalog[grade] = sorted(str(s.value) for s in subjects.hits)

    return {g: catalog[g] for g in sorted(catalog, key=_grade_sort_key)}


@router.get("/catalog")
async def get_catalog() -> dict:
    global _cache
    if _cache is None:
        _cache = await build_catalog()
        log.info("Katalog kuruldu: %d sinif", len(_cache))
    return {"grades": _cache}


@router.get("/conversations")
async def list_conversations(limit: int = 20) -> dict:
    """Son sohbetler (ADR-07). Postgres yoksa boş liste — panel boş görünür
    ama hiçbir uç hata vermez."""
    return {"conversations": await store.recent_conversations(limit)}


@router.get("/conversations/{conversation_id}")
async def get_conversation(conversation_id: int) -> dict:
    """Tur detayı: soru, cevap, dayandığı kaynaklar ve ölçümler."""
    return {
        "id": conversation_id,
        "messages": await store.conversation_messages(conversation_id),
    }
