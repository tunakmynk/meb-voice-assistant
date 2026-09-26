"""Hibrit arama (dense + sparse), grade/subject filtresiyle daraltılmış.

ADR-06: retrieval, cihazın aktif profiline göre Qdrant payload filtresiyle
daraltılır — 3. sınıf sorusuna 12. sınıf kitabından parça gelmez.

İki kol paralel çalışır ve Qdrant sunucu tarafında RRF ile birleştirilir:
  dense  → anlamsal yakınlık ("suyun hâl değiştirmesi" ↔ "buharlaşma")
  sparse → kelime örtüşmesi (özel isim, formül, terim birebir tutmalı)

Fastembed CPU'da ve senkron çalışır. Tek async süreçte (ADR-01) doğrudan
çağırmak event loop'u kilitler — bu sırada cihazın WebSocket'i de, panelin
SSE akışı da donar. Bu yüzden embed işi asyncio.to_thread ile ayrı bir
thread'e verilir.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass
from typing import TYPE_CHECKING

from qdrant_client import AsyncQdrantClient, models

from app.config import settings

if TYPE_CHECKING:
    from fastembed import SparseTextEmbedding, TextEmbedding

# ingest_qdrant.py ile aynı olmak zorunda.
DENSE = "dense"
SPARSE = "sparse"

# E5 ailesi normalde "query: " / "passage: " öneki bekler AMA bu korpus
# öneksiz embed edilmiş. Ölçüm: öneksiz cos=1.000000, "query: " ile 0.9655.
# Yani önek eklemek burada sessiz kalite kaybı olurdu. Boş bırakılması bilinçli.
QUERY_PREFIX = ""

# Cihaza ve panele lazım olan alanlar. contextualized_text kasıtlı olarak yok:
# LLM'e text gider (4.1), contextualized_text yalnızca embed girdisiydi ve
# her cevapta ~2 kat fazla veri taşımanın anlamı yok.
PAYLOAD_FIELDS = ["text", "breadcrumb", "grade", "subject", "unit", "source_file"]


@dataclass(slots=True)
class RetrievedChunk:
    chunk_id: str
    text: str
    score: float
    breadcrumb: str | None = None
    grade: str | None = None
    subject: str | None = None
    unit: str | None = None

    def as_source(self) -> dict:
        """answer_text olayındaki kaynak alanı (3.2)."""
        return {
            "grade": self.grade,
            "subject": self.subject,
            "unit": self.unit,
            "breadcrumb": self.breadcrumb,
        }

    def as_record(self) -> dict:
        """messages.retrieved_chunks sütunu (4.2)."""
        return {
            "chunk_id": self.chunk_id,
            "breadcrumb": self.breadcrumb,
            "grade": self.grade,
            "subject": self.subject,
            "score": round(self.score, 4),
        }


class Retriever:
    """Qdrant istemcisi + embed modellerini tutan tek nesne.

    Modeller ilk kullanımda değil, uygulama açılışında yüklenir (warmup):
    2 GB'lık modeli ilk sorunun üstüne yüklemek 4 saniye hedefini tek başına
    bitirir.
    """

    def __init__(self) -> None:
        self._client = AsyncQdrantClient(url=settings.qdrant_url, timeout=30)
        self._dense: TextEmbedding | None = None
        self._sparse: SparseTextEmbedding | None = None

    # --- model yükleme ---

    def _load_models(self) -> None:
        from fastembed import SparseTextEmbedding, TextEmbedding

        if self._dense is None:
            self._dense = TextEmbedding(
                model_name=settings.dense_model,
                cache_dir=settings.fastembed_cache_dir,
            )
        if self._sparse is None:
            self._sparse = SparseTextEmbedding(
                model_name=settings.sparse_model,
                cache_dir=settings.fastembed_cache_dir,
                # Korpusla aynı stemmer olmak zorunda; bkz. config.bm25_language.
                language=settings.bm25_language,
            )

    async def warmup(self) -> None:
        """Açılışta çağrılır: modelleri yükler ve bir kez ısıtır."""
        await asyncio.to_thread(self._load_models)
        await asyncio.to_thread(self._encode, "ısınma sorusu")

    # --- embed ---

    def _encode(self, query: str) -> tuple[list[float], models.SparseVector]:
        """Senkron ve CPU-bound. Her zaman to_thread içinden çağrılır."""
        self._load_models()
        assert self._dense is not None and self._sparse is not None

        dense_vec = next(iter(self._dense.embed([QUERY_PREFIX + query]))).tolist()
        # not: QUERY_PREFIX boş; korpusla birebir aynı girdi biçimi kullanılıyor.

        # BM25'te sorgu tarafı terim frekansı taşımaz (her terim 1 kez sayılır);
        # query_embed tam olarak bunu yapar. embed() kullanırsak sorgudaki
        # tekrarlar sonucu bozar.
        sp = next(iter(self._sparse.query_embed([query])))
        sparse_vec = models.SparseVector(
            indices=[int(i) for i in sp.indices],
            values=[float(v) for v in sp.values],
        )
        return dense_vec, sparse_vec

    # --- filtre ---

    @staticmethod
    def _build_filter(grade: str | None, subject: str | None) -> models.Filter | None:
        """Profil filtresi. İkisi de yoksa None döner — R-06'nın yedek yolu:
        filtresiz arama, doğruluk düşer ama sistem susmaz."""
        must: list[models.Condition] = []
        if grade:
            must.append(models.FieldCondition(
                key="grade", match=models.MatchValue(value=grade)))
        if subject:
            must.append(models.FieldCondition(
                key="subject", match=models.MatchValue(value=subject)))
        return models.Filter(must=must) if must else None

    # --- arama ---

    async def search(
        self,
        query: str,
        *,
        grade: str | None = None,
        subject: str | None = None,
        top_k: int | None = None,
    ) -> list[RetrievedChunk]:
        top_k = top_k or settings.top_k
        prefetch_limit = top_k * settings.prefetch_multiplier
        dense_vec, sparse_vec = await asyncio.to_thread(self._encode, query)
        flt = self._build_filter(grade, subject)

        response = await self._client.query_points(
            collection_name=settings.qdrant_collection,
            prefetch=[
                models.Prefetch(query=dense_vec, using=DENSE,
                                filter=flt, limit=prefetch_limit),
                models.Prefetch(query=sparse_vec, using=SPARSE,
                                filter=flt, limit=prefetch_limit),
            ],
            # RRF sıra tabanlıdır: kosinüs skoru (0-1) ile BM25 skorunu
            # (sınırsız) doğrudan toplamaya çalışmaz, ikisinin sıralamasını
            # birleştirir. Ölçekleri birbirinden bağımsız olduğu için doğru
            # seçim bu.
            query=models.FusionQuery(fusion=models.Fusion.RRF),
            limit=top_k,
            with_payload=PAYLOAD_FIELDS,
        )

        return [
            RetrievedChunk(
                chunk_id=str(p.id),
                text=(p.payload or {}).get("text", ""),
                score=p.score,
                breadcrumb=(p.payload or {}).get("breadcrumb"),
                grade=(p.payload or {}).get("grade"),
                subject=(p.payload or {}).get("subject"),
                unit=(p.payload or {}).get("unit"),
            )
            for p in response.points
        ]

    async def close(self) -> None:
        await self._client.close()


retriever = Retriever()
