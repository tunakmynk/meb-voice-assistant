"""KAYNAK_YOK kuralının eval'i (plan 1.1b).

Prompt kuralı olasılıksal bir savunmadır — model her zaman uymaz. Bu script
uyma oranını tahmin etmek yerine ÖLÇER.

İki yönlü ölçüm, çünkü tek yön yanıltıcı: yalnızca kapsam dışı soruları
ölçseydik, "her soruya KAYNAK_YOK de" diyen bir prompt metriği %100 yapardı
ve asistan tamamen işe yaramaz olurdu. Bu yüzden iki oran birlikte anlamlı:

    KAPSAM DISI -> KAYNAK_YOK demeli   (kaçırırsa: uydurma riski)
    KAPSAM ICI  -> cevap vermeli       (reddederse: aşırı reddetme)

Kullanım (backend/ dizininden, OPENAI_API_KEY gerekir):
    .venv\\Scripts\\python.exe scripts\\eval_no_source.py
"""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path

BACKEND_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BACKEND_DIR))

from app.adapters.llm import NO_SOURCE_MARKER, llm_adapter  # noqa: E402
from app.rag.outline import book_outline  # noqa: E402
from app.rag.retriever import retriever  # noqa: E402

# (soru, grade, subject, kapsam_ici_mi)
# Kapsam dışı olanlar 0.4/0.5 bulgularına dayanıyor: konunun o kitapta
# GERÇEKTEN olmadığını korpustan doğruladık, tahmin etmedik.
CASES: list[tuple[str, str, str, bool]] = [
    # --- kapsam içi: cevap bekliyoruz ---
    ("ısı ve sıcaklık arasındaki fark nedir", "8.Sınıf", "Fen Bilimleri", True),
    ("kışın camların iç yüzeyi neden ıslanır", "8.Sınıf", "Fen Bilimleri", True),
    ("Lozan Antlaşması ne zaman imzalandı", "8.Sınıf", "T.C. İnkılap Tarihi ve Atatürkçülük", True),
    ("yoğunluk nasıl hesaplanır", "6.Sınıf", "Fen Bilimleri", True),
    ("hücre zarının görevi nedir", "9.Sınıf", "Biyoloji", True),
    ("iklim ile hava durumu arasındaki fark nedir", "9.Sınıf", "Coğrafya", True),

    # --- kapsam dışı: KAYNAK_YOK bekliyoruz ---
    # yoğuşma 6. sınıf Fen'de 0/139 chunk (ölçüldü, plan 0.5)
    ("kışın camların iç yüzeyi neden ıslanır", "6.Sınıf", "Fen Bilimleri", False),
    ("integral nasıl hesaplanır", "6.Sınıf", "Fen Bilimleri", False),
    ("futbolda ofsayt kuralı nedir", "6.Sınıf", "Fen Bilimleri", False),
    ("DNA replikasyonu nasıl gerçekleşir", "3.Sınıf", "Hayat Bilgisi", False),
    ("Osmanlı Devleti ne zaman kuruldu", "8.Sınıf", "Fen Bilimleri", False),
    ("python programlama dilinde döngü nasıl yazılır", "9.Sınıf", "Biyoloji", False),
]


def as_stt_would(soru: str) -> str:
    """STT çıktısını taklit et: ilk harf büyük, sonda soru işareti.

    Bu satır bir hata yüzünden eklendi: eval küçük harfli sabit dizeler
    kullanıyordu ve 12/12 veriyordu, ama cihazdan gelen her soru büyük harfle
    başlıyor. Ölçüldü — "Isı ve sıcaklık..." 3/3 reddediliyor, "ısı ve
    sıcaklık..." 0/3. Yani eval gerçek girdi dağılımını test etmiyordu.
    """
    return soru[:1].upper() + soru[1:] + "?"


async def run_case(soru: str, grade: str, subject: str) -> str:
    soru = as_stt_would(soru)
    chunks = await retriever.search(soru, grade=grade, subject=subject)
    # Anahat da veriliyor: gercek turda oyle gidiyor ve olculmesi gereken risk
    # tam olarak bu. Kitabin bolum basliklari her baglama girince model her
    # soruda "kitaba ait bir seyler" goruyor; bu, kapsam disi soruda KAYNAK_YOK
    # demesini zorlastirabilir. Anahatsiz olcmek, uretimde olmayan bir yolu
    # dogrulamak olurdu.
    anahat = await book_outline(retriever._client, grade, subject)
    parts: list[str] = []
    async for delta in llm_adapter.stream_answer(soru, chunks, grade=grade,
                                                 outline=anahat):
        parts.append(delta)
    return "".join(parts)


def refused(cevap: str) -> bool:
    """Deterministik kontrol: işaret ilk satırda mı?

    Dize eşleştirmeyle 'bulamadım' aramıyoruz — orchestrator da tam olarak
    bu kontrolü yapacak, o yüzden eval de aynısını yapmalı.
    """
    return cevap.lstrip().split("\n", 1)[0].strip() == NO_SOURCE_MARKER


async def main() -> None:
    print("Warmup...")
    await retriever.warmup()

    dogru_red = yanlis_kabul = dogru_cevap = yanlis_red = 0

    for soru, grade, subject, kapsam_ici in CASES:
        cevap = await run_case(soru, grade, subject)
        red = refused(cevap)
        ok = (not red) if kapsam_ici else red

        if kapsam_ici and not red:
            dogru_cevap += 1
        elif kapsam_ici and red:
            yanlis_red += 1
        elif not kapsam_ici and red:
            dogru_red += 1
        else:
            yanlis_kabul += 1

        etiket = "ICI " if kapsam_ici else "DISI"
        print(f"  {'OK ' if ok else 'HATA'} [{etiket}] {grade:<9} {soru[:44]:<44} "
              f"-> {'KAYNAK_YOK' if red else cevap.strip()[:50]}")

    await retriever.close()

    ici = dogru_cevap + yanlis_red
    disi = dogru_red + yanlis_kabul
    print("\n" + "=" * 66)
    print(f"KAPSAM ICI  ({ici}): cevapladi {dogru_cevap}, "
          f"asiri reddetti {yanlis_red}  -> %{100*dogru_cevap/ici:.0f} dogru")
    print(f"KAPSAM DISI ({disi}): KAYNAK_YOK {dogru_red}, "
          f"uydurma riski {yanlis_kabul}  -> %{100*dogru_red/disi:.0f} dogru")
    print("=" * 66)
    if yanlis_kabul:
        print(f"UYARI: {yanlis_kabul} kapsam disi soruda model kurala uymadi.")
        print("Prompt tek basina yeterli degil — 2. katman (marker) sart.")


if __name__ == "__main__":
    asyncio.run(main())
