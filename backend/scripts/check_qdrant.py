"""Zemin kontrolü: koleksiyon gerçekten sorgulanabilir durumda mı?

Plan Aşama 0.2-0.4 (docs/uygulama-plani.md). Üç soruyu kod okuyarak değil,
Qdrant'a sorarak cevaplar:

  1. Koleksiyon var mı, kaç nokta taşıyor, vektör yapılandırması beklediğimiz
     gibi mi (dense 1024 + sparse IDF)?
  2. grade / subject üzerinde payload index'i açık mı (Açık Soru 1, R-06)?
  3. grade ve subject alanlarında GERÇEKTE hangi değerler var (Açık Soru 2)?

3. madde en kritiği: cihaz ekranındaki sınıf/ders seçimi ile payload değeri
birebir eşleşmezse filtre sessizce sıfır sonuç döndürür — hata da vermez.

Kullanım (backend/ dizininden):
    .venv\\Scripts\\python.exe scripts\\check_qdrant.py
"""

from __future__ import annotations

import sys
from pathlib import Path

from qdrant_client import QdrantClient

BACKEND_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BACKEND_DIR))

from app.config import settings  # noqa: E402


def main() -> None:
    client = QdrantClient(url=settings.qdrant_url, timeout=60)

    if not client.collection_exists(settings.qdrant_collection):
        sys.exit(
            f"HATA: '{settings.qdrant_collection}' koleksiyonu yok.\n"
            f"Once yukle:  .venv\\Scripts\\python.exe scripts\\ingest_qdrant.py --recreate"
        )

    info = client.get_collection(settings.qdrant_collection)
    print(f"Koleksiyon : {settings.qdrant_collection}")
    print(f"Nokta      : {info.points_count}")

    # --- vektör yapılandırması ---
    vectors = info.config.params.vectors or {}
    sparse = info.config.params.sparse_vectors or {}
    for name, params in vectors.items():
        print(f"dense[{name}]  : size={params.size} distance={params.distance}")
    for name, params in sparse.items():
        modifier = getattr(params, "modifier", None)
        print(f"sparse[{name}] : modifier={modifier}")

    # --- payload index (Açık Soru 1) ---
    # payload_schema yalnızca INDEXLI alanları listeler; index'siz bir alan
    # burada hiç görünmez. Bu yüzden "var mı yok mu" sorusunun tek doğru yeri.
    schema = info.payload_schema or {}
    print("\n--- payload index ---")
    for field in ("grade", "subject"):
        if field in schema:
            print(f"  [x] {field}: {schema[field].data_type} "
                  f"({schema[field].points} nokta)")
        else:
            print(f"  [ ] {field}: INDEX YOK -> filtre tum payload'lari tarar (R-06)")

    # --- gerçek değerler (Açık Soru 2) ---
    # facet, alanın benzersiz değerlerini sayarak döndürür. Aynı işi scroll ile
    # tüm noktaları çekerek de yapabilirdik ama bu, ~100 bin noktayı ağdan
    # geçirmek demek olurdu; facet sayımı sunucuda yapar.
    for field in ("grade", "subject"):
        print(f"\n--- {field} degerleri ---")
        try:
            result = client.facet(
                collection_name=settings.qdrant_collection,
                key=field,
                limit=50,
            )
            for hit in result.hits:
                print(f"  {hit.value!r:<40} {hit.count}")
        except Exception as exc:  # facet Qdrant 1.12+ ister
            print(f"  facet calismadi ({exc}); ornek noktalardan okunuyor:")
            points, _ = client.scroll(
                collection_name=settings.qdrant_collection,
                limit=200,
                with_payload=[field],
                with_vectors=False,
            )
            values = {(p.payload or {}).get(field) for p in points}
            for value in sorted(v for v in values if v is not None):
                print(f"  {value!r}")


if __name__ == "__main__":
    main()
