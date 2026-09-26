"""Kitap anahatı — sıralı bölüm başlıkları.

**Çözdüğü sorun.** "Bu kitabın son kısmındaki konuyu anlat" sorusuna sistem
"bu konuyu kitabında bulamadım" diyordu. Sebep prompt değil, retrieval:
anlamsal arama KONUM sorusuna cevap veremez. "Son kısım" ifadesinin, kitabın
son bölümünün içeriğiyle hiçbir anlamsal benzerliği yok — embed uzayında
"son" kelimesi nanoteknolojiye yaklaştırmıyor. Skor eşiği de kurtarmıyor,
çünkü arama her koşulda en yakın K parçayı döndürüyor (plan 0.5).

Ama sıra bilgisi VERİDE zaten var: her parçanın `chunk_index` alanı kitap
içindeki yerini tutuyor. Anahat bundan üretiliyor ve sorunun yeniden
yazılması sırasında modele veriliyor; model "son kısım"ı gerçek bölüm adına
çevirince arama sıradan bir konu araması hâline geliyor.

**Neden anahtar kelime listesi değil.** "son", "ilk", "sonundaki" gibi bir
liste denenebilirdi ama bu projede aynı yaklaşım bir kez ölçülüp elendi
(query.py'deki gerekçe): Türkçe konuma da geçmişe de sınırsız biçimde atıf
yapıyor ve liste hiçbir zaman tamamlanmıyor. Karar zaten sohbeti okuyan
modele bırakılıyor.

**Maliyet.** Kitap başına bir kez Qdrant taraması (188 parça ölçüldü) ve
sonuç önbelleğe alınıyor. Anahat 46 bölüm / ~1300 karakter — yeniden yazma
çağrısına rahatça sığıyor.
"""

from __future__ import annotations

import logging

from qdrant_client import models

from app.config import settings

log = logging.getLogger(__name__)

# (grade, subject) -> sıralı bölüm başlıkları. Koleksiyon çalışma anında
# değişmiyor, bu yüzden süresiz önbellek yeterli (routes.py'deki katalog
# önbelleğiyle aynı gerekçe).
_cache: dict[tuple[str, str], list[str]] = {}


async def book_outline(client, grade: str | None, subject: str | None) -> list[str]:
    """Kitabın bölüm başlıkları, kitaptaki SIRAYLA.

    grade ve subject verilmemişse boş liste döner: hangi kitap olduğu belli
    değilken anahat üretmek anlamsız ve yanlış kitabın başlıklarını modele
    vermek, olmayan bir konuyu varmış gibi göstermek olurdu.
    """
    if not grade or not subject:
        return []

    anahtar = (grade, subject)
    if anahtar in _cache:
        return _cache[anahtar]

    flt = models.Filter(must=[
        models.FieldCondition(key="grade", match=models.MatchValue(value=grade)),
        models.FieldCondition(key="subject", match=models.MatchValue(value=subject)),
    ])

    try:
        # chunk_index'te payload indeksi yok, bu yüzden Qdrant tarafında
        # sıralamak yerine tarayıp burada sıralıyoruz. Bir kitap birkaç yüz
        # parça ve sonuç önbelleğe giriyor; indeks eklemenin karşılığı yok.
        noktalar, _ = await client.scroll(
            collection_name=settings.qdrant_collection,
            scroll_filter=flt,
            limit=10000,
            with_payload=["section", "chunk_index"],
            with_vectors=False,
        )
    except Exception:
        # Anahat bir iyileştirme; alınamazsa tur normal akışına devam etmeli.
        log.warning("anahat alinamadi: %s / %s", grade, subject, exc_info=True)
        return []

    sirali = sorted(
        ((n.payload or {}).get("chunk_index") or 0, (n.payload or {}).get("section") or "")
        for n in noktalar
    )

    bolumler: list[str] = []
    for _, bolum in sirali:
        # Ardışık tekrarlar atlanıyor: aynı bölüm birden çok parçaya bölünmüş
        # oluyor ve her parçayı listelemek anahatı okunamaz hâle getirirdi.
        if bolum and (not bolumler or bolumler[-1] != bolum):
            bolumler.append(bolum)

    _cache[anahtar] = bolumler
    log.info("anahat: %s / %s -> %d bolum", grade, subject, len(bolumler))
    return bolumler
