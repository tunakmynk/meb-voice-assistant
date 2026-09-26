"""Panel uçları (plan 4.1-4.4).

============================================================
Kimlik doğrulama (Açık Soru 7)
============================================================
Tek şifre, imzalı çerez. Kullanıcı hesabı yok çünkü kullanıcı YOK: paneli
açan tek kişi cihazın sahibi. Kullanıcı tablosu, parola sıfırlama ve rol
yönetimi eklemek, karşılığı olmayan bir karmaşıklık olurdu.

Çerezde şifre DEĞİL, şifreden türetilen bir HMAC damgası duruyor.
İstemci damgayı üretemez çünkü şifreyi bilmiyor. Şifreyi çerezde taşımak,
onu her istekte ağda dolaştırmak demekti.

ADR-10 gereği bağlantı düz HTTP (yerel ağ, demo). Bu yüzden panel gerçek
bir güvenlik sınırı DEĞİL, yanlışlıkla açılmayı önleyen bir kapı. Şifre de
demo günü paylaşılabilecek bir şey; kritik veri tutmuyor.

============================================================
Runtime ayarları (4.4)
============================================================
config.py'nin belgelediği gibi tek bir Settings örneği var ve modüller onu
ÇAĞRI ANINDA okuyor. Panelden yazılan değer bir sonraki turda geçerli;
yeniden başlatma gerekmiyor. Yalnızca ölçüm/kalite ayarları açık —
adresler ve anahtarlar panelden değiştirilemiyor, çünkü yanlış bir değer
sistemi demo ortasında ulaşılamaz hale getirirdi.
"""

from __future__ import annotations

import asyncio
import hashlib
import hmac
import json
import logging
from pathlib import Path

from fastapi import APIRouter, Cookie, Form, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse, StreamingResponse

from app.config import settings
from app.conversation.store import current_model_config, store
from app.panel import live

log = logging.getLogger(__name__)

router = APIRouter(prefix="/panel", tags=["panel"])

STATIC_DIR = Path(__file__).resolve().parents[1] / "static"
CEREZ = "meb_panel"

# Çerez damgası şifreden türetiliyor (HMAC-SHA256).
#
# Standart kütüphane yetiyor; demo öncesi yeni bir bağımlılık kurmanın
# karşılığı yok. Şifre çerezde TAŞINMIYOR, yalnızca damgası duruyor —
# istemci damgayı üretemez çünkü şifreyi bilmiyor.
#
# Şifre değişince eski damgalar kendiliğinden geçersiz oluyor; ayrı bir
# "oturumları kapat" adımı gerekmiyor.
def _damga() -> str:
    anahtar = (settings.panel_password or "meb").encode()
    return hmac.new(anahtar, b"panel-giris", hashlib.sha256).hexdigest()


def _girisli_mi(cerez: str | None) -> bool:
    if not cerez:
        return False
    # compare_digest: karşılaştırma süresi eşleşen karakter sayısına göre
    # değişmesin. Yerel ağda abartı ama maliyeti sıfır.
    return hmac.compare_digest(cerez, _damga())


def _zorunlu_giris(cerez: str | None) -> None:
    if not _girisli_mi(cerez):
        raise HTTPException(status_code=401, detail="giris gerekli")


# ---------------- Sayfa ve giriş ----------------

@router.get("")
async def panel_sayfasi(meb_panel: str | None = Cookie(default=None)):
    if not _girisli_mi(meb_panel):
        return FileResponse(STATIC_DIR / "panel_giris.html")
    return FileResponse(STATIC_DIR / "panel.html")


@router.post("/login")
async def giris(sifre: str = Form(...)):
    if sifre != settings.panel_password:
        # Sayfaya hata bilgisiyle dönülüyor; JSON döndürmek formu bozardı.
        return RedirectResponse("/panel?hata=1", status_code=303)
    yanit = RedirectResponse("/panel", status_code=303)
    yanit.set_cookie(CEREZ, _damga(), httponly=True, samesite="lax")
    return yanit


@router.post("/logout")
async def cikis():
    yanit = RedirectResponse("/panel", status_code=303)
    yanit.delete_cookie(CEREZ)
    return yanit


# ---------------- Canlı akış (4.2) ----------------

@router.get("/api/events")
async def canli_olaylar(request: Request, meb_panel: str | None = Cookie(default=None)):
    _zorunlu_giris(meb_panel)

    async def akis():
        kuyruk = live.abone_ol()
        try:
            while True:
                if await request.is_disconnected():
                    break
                try:
                    olay = await asyncio.wait_for(kuyruk.get(), timeout=15)
                except asyncio.TimeoutError:
                    # Yorum satırı: bazı vekiller sessiz bağlantıyı kapatıyor
                    # ve tarayıcı yeniden bağlanana kadar panel donuk kalıyor.
                    yield ": ping\n\n"
                    continue
                yield f"data: {json.dumps(olay, ensure_ascii=False)}\n\n"
        finally:
            live.abonelikten_cik(kuyruk)

    return StreamingResponse(akis(), media_type="text/event-stream", headers={
        "Cache-Control": "no-cache",
        "X-Accel-Buffering": "no",
    })


# ---------------- Sohbetler (4.3) ----------------

@router.get("/api/conversations")
async def sohbetler(limit: int = 20, meb_panel: str | None = Cookie(default=None)):
    _zorunlu_giris(meb_panel)
    return {"conversations": await store.recent_conversations(limit)}


@router.get("/api/conversations/{conversation_id}")
async def sohbet_detay(conversation_id: int, meb_panel: str | None = Cookie(default=None)):
    _zorunlu_giris(meb_panel)
    return {
        "id": conversation_id,
        "messages": await store.conversation_messages(conversation_id),
    }


# ---------------- Ayarlar (4.4) ----------------

# Panelden değiştirilebilecek alanlar ve sınırları. Beyaz liste bilinçli:
# serbest bırakmak, panelden qdrant_url'i bozup sistemi demo ortasında
# ulaşılamaz hale getirmeyi mümkün kılardı.
_AYARLAR = {
    "top_k": (int, 1, 10),
    "temperature": (float, 0.0, 1.5),
    "max_tokens": (int, 64, 1200),
}


@router.get("/api/settings")
async def ayarlar(meb_panel: str | None = Cookie(default=None)):
    _zorunlu_giris(meb_panel)
    return {
        "config": current_model_config(),
        "editable": list(_AYARLAR),
        "device_subscribers": live.abone_sayisi(),
    }


@router.post("/api/settings")
async def ayar_yaz(request: Request, meb_panel: str | None = Cookie(default=None)):
    _zorunlu_giris(meb_panel)
    gelen = await request.json()

    degisen = {}
    for ad, deger in gelen.items():
        if ad not in _AYARLAR:
            continue
        tur, alt, ust = _AYARLAR[ad]
        try:
            v = tur(deger)
        except (TypeError, ValueError):
            return JSONResponse({"error": f"{ad} sayi olmali"}, status_code=400)
        if not (alt <= v <= ust):
            return JSONResponse(
                {"error": f"{ad} {alt}-{ust} araliginda olmali"}, status_code=400)
        setattr(settings, ad, v)
        degisen[ad] = v

    if degisen:
        log.info("panel ayarlari degisti: %s", degisen)
    return {"config": current_model_config(), "changed": degisen}
