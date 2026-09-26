"""vectors/v2/**/*.parquet  ->  Qdrant koleksiyonu (meb-books-v2)

Doküman "vektör verisi Qdrant'a yüklenmiş durumdadır" diyor; gerçekte elimizde
parquet dosyaları var, snapshot yok. Bu script o boşluğu kapatır.

Önemli: dense ve sparse vektörler ZATEN hesaplanmış durumda
(dense = intfloat/multilingual-e5-large, 1024 boyut; sparse = Qdrant/bm25).
Burada hiçbir model çalıştırmıyoruz — parquet'ten okuyup Qdrant'a taşıyoruz.
Bu yüzden yükleme dakikalar sürer, saatler değil.

Kullanım (backend/ dizininden):
    .venv\\Scripts\\python.exe scripts\\ingest_qdrant.py --recreate
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import pyarrow.parquet as pq
from qdrant_client import QdrantClient, models

BACKEND_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = BACKEND_DIR.parent
sys.path.insert(0, str(BACKEND_DIR))

from app.config import settings  # noqa: E402

VECTORS_DIR = REPO_ROOT / "vectors" / "v2"

# Vektör adları. Koleksiyonda ne yazarsak sorgu tarafında da aynısını
# kullanmak zorundayız (app/rag/retriever.py). Tek yerde sabitliyoruz.
DENSE = "dense"
SPARSE = "sparse"

# grade/subject üzerinde filtre yapacağız (ADR-06). Index olmazsa Qdrant
# her sorguda tüm payload'ları tarar -> R-06'daki "filtre yavaş çalışır".
INDEXED_FIELDS = {
    "grade": models.PayloadSchemaType.KEYWORD,
    "subject": models.PayloadSchemaType.KEYWORD,
}

BATCH = 256


def create_collection(client: QdrantClient, recreate: bool) -> None:
    exists = client.collection_exists(settings.qdrant_collection)
    if exists and not recreate:
        info = client.get_collection(settings.qdrant_collection)
        print(f"Koleksiyon zaten var ({info.points_count} nokta). "
              f"Yeniden kurmak icin --recreate ver.")
        sys.exit(0)
    if exists:
        print(f"Koleksiyon siliniyor: {settings.qdrant_collection}")
        client.delete_collection(settings.qdrant_collection)

    client.create_collection(
        collection_name=settings.qdrant_collection,
        vectors_config={
            DENSE: models.VectorParams(
                size=settings.dense_vector_size,
                distance=models.Distance.COSINE,
            )
        },
        sparse_vectors_config={
            # IDF modifier: BM25'in "nadir kelime daha degerli" agirligini
            # Qdrant sunucu tarafinda hesaplar. Parquet'teki sparse_values
            # ham terim frekansi; IDF'i koleksiyon istatistigi belirler.
            SPARSE: models.SparseVectorParams(modifier=models.Modifier.IDF)
        },
    )
    for field, schema in INDEXED_FIELDS.items():
        client.create_payload_index(
            collection_name=settings.qdrant_collection,
            field_name=field,
            field_schema=schema,
        )
    print(f"Koleksiyon olusturuldu: {settings.qdrant_collection} "
          f"(dense={settings.dense_vector_size}, sparse=IDF, "
          f"index={list(INDEXED_FIELDS)})")


def points_from_parquet(path: Path):
    """Bir parquet dosyasini Qdrant PointStruct'lara cevirir."""
    table = pq.read_table(path)
    cols = table.to_pydict()
    for i in range(table.num_rows):
        meta = json.loads(cols["metadata"][i])
        yield models.PointStruct(
            # chunk_id zaten UUID; Qdrant nokta kimligi olarak dogrudan gecerli.
            # Ayni veriyi ikinci kez yuklersek yeni kayit degil, ustune yazma olur.
            id=cols["chunk_id"][i],
            vector={
                DENSE: cols["dense_vector"][i],
                SPARSE: models.SparseVector(
                    indices=cols["sparse_indices"][i],
                    values=cols["sparse_values"][i],
                ),
            },
            payload={
                **meta,
                # text: LLM'e verilecek bagalam. contextualized_text: embed girdisi,
                # panelde/hata ayiklamada neyin embed edildigini gormek icin duruyor.
                "text": cols["text"][i],
                "contextualized_text": cols["contextualized_text"][i],
            },
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--recreate", action="store_true",
                        help="Koleksiyon varsa sil ve sifirdan kur")
    args = parser.parse_args()

    files = sorted(VECTORS_DIR.rglob("*.parquet"))
    if not files:
        sys.exit(f"HATA: {VECTORS_DIR} altinda parquet bulunamadi.")

    client = QdrantClient(url=settings.qdrant_url, timeout=120)
    create_collection(client, args.recreate)

    started = time.perf_counter()
    total = 0
    # chunk_id, metnin deterministik UUID5'i. Ayni metin -> ayni id. Sadece
    # basliktan ibaret ("## GERCEK YASAM PROBLEMLERI") chunk'lar farkli
    # unitelerde birebir tekrar ettigi icin ayni id'yi paylasiyor ve Qdrant'ta
    # ustune yaziliyor. Bu istenen davranis: icerigi olmayan tekrar saklanmaz.
    seen_ids: set[str] = set()
    for n, path in enumerate(files, 1):
        batch: list[models.PointStruct] = []
        count = 0
        for point in points_from_parquet(path):
            seen_ids.add(str(point.id))
            batch.append(point)
            if len(batch) >= BATCH:
                client.upsert(settings.qdrant_collection, points=batch, wait=True)
                count += len(batch)
                batch = []
        if batch:
            client.upsert(settings.qdrant_collection, points=batch, wait=True)
            count += len(batch)
        total += count
        print(f"[{n:>2}/{len(files)}] {path.name}: {count} chunk")

    elapsed = time.perf_counter() - started
    expected = len(seen_ids)
    reported = client.get_collection(settings.qdrant_collection).points_count
    print(f"\nBitti: {total} satir okundu, {expected} benzersiz chunk_id "
          f"({total - expected} tekrar birlestirildi), "
          f"Qdrant {reported} nokta bildiriyor — {elapsed:.1f} sn")
    if reported != expected:
        sys.exit("HATA: benzersiz id sayisi ile Qdrant'taki nokta sayisi tutmuyor.")


if __name__ == "__main__":
    main()
