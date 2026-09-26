"""Canlı retrieval dumanı testi (plan Aşama 0.5).

Birim testi değil: gerçek Qdrant ve gerçek embedding modeli ister. Amacı
"kod çalışıyor mu" değil, **arama gerçekten doğru şeyi mi yapıyor**.

Beş senaryo, her biri farklı bir sessiz başarısızlığı hedefler:

  1. Filtre gerçekten uygulanıyor mu   -> ADR-06'nın kalbi
  2. Sparse (BM25) kolu çalışıyor mu   -> özel isim birebir tutmalı
  3. Dense kolu çalışıyor mu           -> kelime örtüşmesi olmadan anlam
  4. Var olmayan sınıf/ders çifti      -> boş dönmeli, çökmemeli
  5. Yanlış yazılmış grade dizesi      -> cihaz tarafının en olası hatası

Kullanım (backend/ dizininden):
    .venv\\Scripts\\python.exe scripts\\smoke_retrieval.py
"""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path

BACKEND_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BACKEND_DIR))

from app.rag.retriever import RetrievedChunk, retriever  # noqa: E402

failures: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    print(f"  {'[GECTI]' if ok else '[KALDI]'} {name}" + (f" — {detail}" if detail else ""))
    if not ok:
        failures.append(name)


def show(chunks: list[RetrievedChunk], limit: int = 3) -> None:
    for c in chunks[:limit]:
        print(f"      {c.score:.4f}  {c.grade} / {c.subject} — {c.breadcrumb}")
    if not chunks:
        print("      (sonuc yok)")


async def senaryo_1_filtre_uygulaniyor_mu() -> None:
    """Aynı soru, iki farklı sınıf.

    Filtre sessizce yok sayılsaydı sorgu vektörü değişmediği için iki çağrı
    BİREBİR aynı chunk'ları döndürürdü. Ayrıca dönen her chunk'ın kendi
    payload'undaki grade'i istenen değere eşit olmak zorunda — bu, filtrenin
    çalıştığının dolaylı değil doğrudan kanıtı.
    """
    print("\n[1] Filtre uygulaniyor mu — 'isi ve sicaklik arasindaki fark nedir'")
    soru = "ısı ve sıcaklık arasındaki fark nedir"

    alti = await retriever.search(soru, grade="6.Sınıf", subject="Fen Bilimleri")
    sekiz = await retriever.search(soru, grade="8.Sınıf", subject="Fen Bilimleri")

    print("    6.Sinif:")
    show(alti)
    print("    8.Sinif:")
    show(sekiz)

    check("6.Sinif sonuclari bos degil", len(alti) > 0)
    check("8.Sinif sonuclari bos degil", len(sekiz) > 0)
    check(
        "donen her chunk istenen sinifa ait",
        all(c.grade == "6.Sınıf" for c in alti) and all(c.grade == "8.Sınıf" for c in sekiz),
        "payload dogrudan dogruluyor",
    )
    check(
        "iki sinif farkli chunk'lar donduruyor",
        {c.chunk_id for c in alti} != {c.chunk_id for c in sekiz},
        "ayni olsaydi filtre yok sayiliyor demekti",
    )


async def senaryo_2_sparse_kolu() -> None:
    """Özel isim testi — BM25 kolunun görevi.

    'Lozan' anlamsal olarak hiçbir şeye benzemez; onu bulmanın tek yolu
    kelimenin birebir eşleşmesidir. Sonuç alakasız gelirse sparse kolu
    (veya bm25_language ayarı) bozuk demektir.
    """
    print("\n[2] Sparse/BM25 kolu — ozel isim: 'Lozan Antlasmasi'")
    sonuc = await retriever.search(
        "Lozan Antlaşması", grade="8.Sınıf",
        subject="T.C. İnkılap Tarihi ve Atatürkçülük",
    )
    show(sonuc)
    check("sonuc dondu", len(sonuc) > 0)
    check(
        "en az bir chunk 'Lozan' kelimesini iceriyor",
        any("lozan" in (c.text or "").lower() for c in sonuc),
    )


async def senaryo_3_dense_kolu() -> None:
    """Anlam testi — dense kolunun görevi.

    Soruda 'yoğuşma' kelimesi geçmiyor; kitapta ise konu bu adla anlatılıyor.
    Sadece BM25 çalışsaydı bu soru boşa düşerdi.

    Sınıf 8 seçildi çünkü ölçtük: 'yoğuşma' 6. sınıf Fen kitabında HİÇ
    geçmiyor (0/139 chunk), konu 8. sınıfta anlatılıyor. İlk yazımda 6. sınıf
    kullanmıştık ve test kaldı — arama bozuk olduğu için değil, sorunun
    cevabı o kitapta olmadığı için. Bkz. senaryo 6.
    """
    print("\n[3] Dense kolu — kelime ortusmesi olmadan: 'kisin camlarin ic yuzeyi neden islanir'")
    sonuc = await retriever.search(
        "kışın camların iç yüzeyi neden ıslanır",
        grade="8.Sınıf", subject="Fen Bilimleri",
    )
    show(sonuc)
    check("sonuc dondu", len(sonuc) > 0)
    metin = " ".join((c.text or "") + " " + (c.breadcrumb or "") for c in sonuc).lower()
    check(
        "yogusma/buharlasma/hal degisimi konusu yakalandi",
        any(k in metin for k in ("yoğuş", "buharlaş", "hâl değiş", "hal değiş")),
        "soruda bu kelimelerin hicbiri yok",
    )


async def senaryo_4_var_olmayan_cift() -> None:
    """7.Sınıf'ta Matematik kitabı yok (0.4 bulgusu).

    Sistem burada çökmemeli, boş dönmeli. Orchestrator'ın 'kaynak bulunamadı'
    dalını tetikleyecek durum tam olarak budur.
    """
    print("\n[4] Var olmayan sinif/ders cifti — 7.Sinif + Matematik")
    sonuc = await retriever.search("üçgenin alanı nasıl bulunur",
                                   grade="7.Sınıf", subject="Matematik")
    show(sonuc)
    check("bos donuyor, hata firlatmiyor", len(sonuc) == 0)


async def senaryo_5_yanlis_grade_bicimi() -> None:
    """Cihaz '6. Sınıf' (boşluklu) gönderirse ne olur?

    0.4'te gördük: payload'da boşluksuz duruyor. Bu senaryo, yanlış biçimin
    hata değil SESSİZ BOŞLUK ürettiğini kayda geçirir — cihaz tarafında
    katalogdan gelen dizeyi aynen kullanmanın neden zorunlu olduğunun kanıtı.
    """
    print("\n[5] Yanlis grade bicimi — '6. Sinif' (bosluklu)")
    sonuc = await retriever.search("ısı ve sıcaklık arasındaki fark nedir",
                                   grade="6. Sınıf", subject="Fen Bilimleri")
    show(sonuc)
    check("bos donuyor (hata degil, sessiz bosluk)", len(sonuc) == 0)


async def senaryo_6_kapsam_disi_soru() -> None:
    """En kritik senaryo: cevabı o kitapta OLMAYAN soru.

    Retrieval hiçbir zaman "bulamadım" demez — filtreye uyan en yakın K
    chunk'ı her koşulda döndürür. Ölçtüğümüz ham cosine skorları:

        kapsam ici (8.Sinif, konu var)      0.826
        kapsam disi (6.Sinif, konu yok)     0.830   <- daha YUKSEK
        tamamen alakasiz (ofsayt kurali)    0.775

    Yani skor eşiğiyle "kaynak bulunamadı" tespiti YAPILAMAZ: ne RRF skoru
    (sıra tabanlı, tepe her zaman 1.0'a kadar çıkar) ne de ham cosine
    (0.77-0.83 dar bandı) alakayı ölçmüyor.

    Sonuç mimari bir karara dönüşüyor: "bağlamda cevap yok" kararını LLM
    vermek zorunda. system prompt'ta açık kural şart (bkz. plan 1.1).

    Bu test o yüzden "sonuç boş gelsin" beklemiyor — tam tersini, sistemin
    alakasız bağlamı güvenle döndürdüğünü kayda geçiriyor.
    """
    print("\n[6] Kapsam disi soru — 6.Sinif Fen'de olmayan konu")
    sonuc = await retriever.search(
        "kışın camların iç yüzeyi neden ıslanır",
        grade="6.Sınıf", subject="Fen Bilimleri",
    )
    show(sonuc)
    metin = " ".join((c.text or "") for c in sonuc).lower()
    check("retrieval yine de sonuc donduruyor", len(sonuc) > 0)
    check(
        "donen baglam konuyla ilgisiz (beklenen)",
        not any(k in metin for k in ("yoğuş", "buharlaş")),
        "LLM bu baglamdan cevap uydurmamali",
    )


async def main() -> None:
    print("Modeller yukleniyor (warmup)...")
    await retriever.warmup()
    print("Hazir.")

    await senaryo_1_filtre_uygulaniyor_mu()
    await senaryo_2_sparse_kolu()
    await senaryo_3_dense_kolu()
    await senaryo_4_var_olmayan_cift()
    await senaryo_5_yanlis_grade_bicimi()
    await senaryo_6_kapsam_disi_soru()

    await retriever.close()

    print("\n" + "=" * 60)
    if failures:
        print(f"{len(failures)} senaryo kaldi:")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)
    print("Tum senaryolar gecti.")


if __name__ == "__main__":
    asyncio.run(main())
