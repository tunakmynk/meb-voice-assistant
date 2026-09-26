"""Conversation persistence (ADR-07, plan 2.4).

Behind an interface for the same reason the providers are (ADR-08): the
gateway records turns without knowing whether they land in Postgres, and the
tests use an in-memory double instead of needing a database.

Two rules from the design doc shape this:

- **Raw audio is never stored.** Only the transcript, the answer, the sources
  it rested on, and the timings.
- **The settings are recorded per turn, not read from config later.** The
  panel can change top_k / temperature at runtime (4.4), so "what was this
  answer produced with" is only answerable if it is written down at the time.

Persistence must never take the voice loop down with it. Every write is
best-effort: a database that is down degrades the panel, not the student's
answer.
"""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any, Protocol

import asyncpg

from app.config import settings

log = logging.getLogger(__name__)

SCHEMA_PATH = Path(__file__).resolve().parents[2] / "db" / "init" / "001_schema.sql"


class ConversationStore(Protocol):
    async def start_conversation(
        self, *, device_id: str | None, grade: str | None, subject: str | None
    ) -> int | None:
        """Opens a conversation and returns its id (None if unavailable)."""
        ...

    async def record_turn(
        self,
        conversation_id: int | None,
        *,
        question: str,
        answer: str,
        chunks: list[dict[str, Any]],
        latencies: dict[str, int],
        model_config: dict[str, Any],
        no_source: bool,
    ) -> None:
        """Writes one question/answer pair with its sources and timings."""
        ...


class PostgresConversationStore:
    """asyncpg implementation. Connections come from a pool held open for the
    process lifetime, so a turn never pays connection setup."""

    def __init__(self) -> None:
        self._pool: asyncpg.Pool | None = None

    async def connect(self) -> None:
        """Opens the pool and applies the schema.

        The schema is applied here as well as in docker-entrypoint-initdb.d
        because that directory only runs when the volume is empty — an
        existing installation would otherwise never get the tables.
        """
        self._pool = await asyncpg.create_pool(settings.postgres_dsn, min_size=1, max_size=5)
        async with self._pool.acquire() as conn:
            await conn.execute(SCHEMA_PATH.read_text(encoding="utf-8"))
        log.info("Postgres baglandi, sema hazir.")

    async def close(self) -> None:
        if self._pool is not None:
            await self._pool.close()
            self._pool = None

    async def start_conversation(
        self, *, device_id: str | None, grade: str | None, subject: str | None
    ) -> int | None:
        if self._pool is None:
            return None
        try:
            async with self._pool.acquire() as conn:
                return await conn.fetchval(
                    "INSERT INTO conversations (device_id, grade, subject) "
                    "VALUES ($1, $2, $3) RETURNING id",
                    device_id, grade, subject,
                )
        except Exception:
            log.warning("sohbet acilamadi", exc_info=True)
            return None

    async def record_turn(
        self,
        conversation_id: int | None,
        *,
        question: str,
        answer: str,
        chunks: list[dict[str, Any]],
        latencies: dict[str, int],
        model_config: dict[str, Any],
        no_source: bool,
    ) -> None:
        if self._pool is None or conversation_id is None:
            return
        try:
            async with self._pool.acquire() as conn, conn.transaction():
                await conn.execute(
                    "INSERT INTO messages (conversation_id, role, content) "
                    "VALUES ($1, 'user', $2)",
                    conversation_id, question,
                )
                await conn.execute(
                    "INSERT INTO messages (conversation_id, role, content, "
                    "retrieved_chunks, model_config, latencies, no_source) "
                    "VALUES ($1, 'assistant', $2, $3, $4, $5, $6)",
                    conversation_id, answer,
                    json.dumps(chunks, ensure_ascii=False),
                    json.dumps(model_config, ensure_ascii=False),
                    json.dumps(latencies),
                    no_source,
                )
                await conn.execute(
                    "UPDATE conversations SET last_message_at = now() WHERE id = $1",
                    conversation_id,
                )
        except Exception:
            # Deliberately swallowed: a failed write must not break the turn
            # the student is listening to.
            log.warning("tur kaydedilemedi", exc_info=True)


    async def recent_conversations(self, limit: int = 20) -> list[dict[str, Any]]:
        """Panel'in ana sorgusu: son sohbetler, tur sayısıyla birlikte."""
        if self._pool is None:
            return []
        rows = await self._pool.fetch(
            """
            SELECT c.id, c.grade, c.subject, c.started_at, c.last_message_at,
                   count(*) FILTER (WHERE m.role = 'assistant') AS turns
            FROM conversations c
            LEFT JOIN messages m ON m.conversation_id = c.id
            GROUP BY c.id
            ORDER BY c.last_message_at DESC
            LIMIT $1
            """,
            limit,
        )
        return [dict(r) for r in rows]

    async def conversation_messages(self, conversation_id: int) -> list[dict[str, Any]]:
        if self._pool is None:
            return []
        rows = await self._pool.fetch(
            "SELECT role, content, created_at, retrieved_chunks, latencies, no_source "
            "FROM messages WHERE conversation_id = $1 ORDER BY created_at, id",
            conversation_id,
        )
        out = []
        for r in rows:
            d = dict(r)
            for key in ("retrieved_chunks", "latencies"):
                if d.get(key):
                    d[key] = json.loads(d[key])
            out.append(d)
        return out


def current_model_config() -> dict[str, Any]:
    return {
        "stt_model": settings.stt_model,
        "llm_model": settings.llm_model,
        "tts_model": settings.tts_model,
        "tts_voice": settings.tts_voice,
        "top_k": settings.top_k,
        "temperature": settings.temperature,
        "max_tokens": settings.max_tokens,
    }


store = PostgresConversationStore()


class InMemoryConversationStore:
    """Test double. Same shape, no database.

    Kept beside the real one so a drift in the interface breaks the tests
    rather than only showing up at runtime.
    """

    def __init__(self) -> None:
        self.conversations: list[dict] = []
        self.turns: list[dict] = []

    async def start_conversation(self, *, device_id=None, grade=None, subject=None) -> int:
        self.conversations.append(
            {"device_id": device_id, "grade": grade, "subject": subject}
        )
        return len(self.conversations)

    async def record_turn(self, conversation_id, *, question, answer, chunks,
                          latencies, model_config, no_source) -> None:
        self.turns.append({
            "conversation_id": conversation_id,
            "question": question,
            "answer": answer,
            "chunks": chunks,
            "latencies": latencies,
            "model_config": model_config,
            "no_source": no_source,
        })
