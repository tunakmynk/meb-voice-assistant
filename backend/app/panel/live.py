"""Panel için canlı olay yayını (plan 4.2).

Cihaz turu ilerlerken panelin de aynı anda görmesi gerekiyor: hangi soru
soruldu, hangi kaynaklara gidildi, ne kadar sürdü. Bunu veritabanını
yoklayarak yapmak iki sorun üretirdi — gecikme ve boşuna sorgu — ayrıca
tur BİTMEDEN hiçbir şey görünmezdi; oysa panelin asıl değeri turu
**akarken** göstermek.

**Neden kendi yayınımız, hazır bir kütüphane değil.** İhtiyaç dar:
tek süreç, birkaç tarayıcı sekmesi, kalıcılık yok. asyncio.Queue başına
bir abone yeterli.

**Yavaş abone turu yavaşlatmamalı.** Kuyruk dolarsa olay ATILIYOR, yayıncı
beklemiyor. Panel sekmesi arka planda kalıp olayları tüketmezse en kötü
ihtimalle birkaç güncelleme kaçırır; cihazdaki öğrencinin cevabı gecikmez.
Sıralama burada doğruluktan önemli değil, canlılık önemli.
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any

log = logging.getLogger(__name__)

# Abone başına kuyruk. Panel birkaç sekmede açık olabilir.
_aboneler: set[asyncio.Queue] = set()

# Kuyruk sınırı: dolduğunda en eski değil, YENİ olay atılıyor (aşağıya bakın).
_KUYRUK_SINIRI = 64

# Son bilinen durum. Panel sonradan açıldığında boş ekran görmesin diye
# yeni abonelere önce bu gönderiliyor.
_son_durum: dict[str, Any] = {"type": "status", "device": "disconnected"}


def yayinla(olay: dict[str, Any]) -> None:
    """Olayı tüm abonelere dağıtır. ASLA bloklamaz, ASLA hata fırlatmaz.

    Çağıran, tur akışının içinde; buradan çıkan bir istisna öğrencinin
    cevabını düşürürdü. Panel bir gözlemci, turun parçası değil.
    """
    if olay.get("type") == "status":
        global _son_durum
        _son_durum = olay

    for kuyruk in list(_aboneler):
        try:
            kuyruk.put_nowait(olay)
        except asyncio.QueueFull:
            # Yavaş abone: olayı düşür, turu bekletme.
            log.debug("panel abonesi yavas, olay atildi")
        except Exception:
            log.debug("panel yayini basarisiz", exc_info=True)


def abone_ol() -> asyncio.Queue:
    kuyruk: asyncio.Queue = asyncio.Queue(maxsize=_KUYRUK_SINIRI)
    # Yeni abone önce mevcut durumu görsün, sonraki olayı beklemesin.
    kuyruk.put_nowait(_son_durum)
    _aboneler.add(kuyruk)
    return kuyruk


def abonelikten_cik(kuyruk: asyncio.Queue) -> None:
    _aboneler.discard(kuyruk)


def abone_sayisi() -> int:
    return len(_aboneler)
