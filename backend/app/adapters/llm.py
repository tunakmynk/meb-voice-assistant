"""LLM adapter — bağlam + soru → akışlı cevap.

ADR-03: token akışı cümle sınırında bölünüp anında TTS'e verilecek. Bu yüzden
adapter tam cevabı değil, **parça parça** üretir; cümleye bölme işi
orchestrator'ın (2.1).

Kapsam dışı soru sorunu (plan 0.5): retrieval hiçbir zaman "bulamadım"
demez, filtreye uyan en yakın K chunk'ı her koşulda döndürür — ve skor
eşiğiyle bunu ayırmak mümkün değil (kapsam dışı soru 0.830, kapsam içi
0.826 aldı). Kararı LLM vermek zorunda.

Ama prompt kuralı olasılıksal bir savunmadır: model bazen uymaz. Bu yüzden
tek katman değil üç katman var:

  1. system prompt kuralı + düşük temperature  -> uyma olasılığını artırır
  2. NO_SOURCE_MARKER                          -> uyduğunda DETERMİNİSTİK yakalanır
  3. ölçüm (plan 1.1b)                         -> uymama oranı tahmin değil, sayı

2. katman kritik: modelden düz metinle "bulamadım" demesini beklersek
orchestrator bunu ancak dize eşleştirerek anlayabilir — kırılgan. Sabit bir
işaret isteyip ilk satıra bakmak, kararı koda taşır.
"""

from __future__ import annotations

from collections.abc import AsyncIterator
from typing import Protocol

from openai import AsyncOpenAI

from app.config import settings
from app.rag.retriever import RetrievedChunk

# Model, bağlamda cevap bulamadığında cevabın İLK satırı tam olarak bu olur.
# Orchestrator ilk satıra bakıp TTS'i hiç başlatmadan "bulamadım" ekranına
# geçebilir. Türkçe metinde kazara geçmeyecek bir dize seçildi.
NO_SOURCE_MARKER = "KAYNAK_YOK"


class LlmAdapter(Protocol):
    def stream_answer(
        self,
        question: str,
        chunks: list[RetrievedChunk],
        *,
        grade: str | None = None,
        history: list[dict[str, str]] | None = None,
        outline: list[str] | None = None,
    ) -> AsyncIterator[str]:
        """Cevabı parça parça üretir. Her parça ham metin token'ıdır."""
        ...


def _grade_level(grade: str | None) -> int:
    """'8.Sınıf' -> 8. Okunamazsa 6 (orta seviye) varsayılır.

    Cihaz katalogdan gelen dizeyi aynen gönderdiği için biçim garanti; yine de
    ayrıştırma hatası tüm turu düşürmesin diye sessizce ortaya düşüyoruz.
    """
    if not grade:
        return 6
    try:
        return int(grade.split(".")[0])
    except (ValueError, IndexError):
        return 6


def _tone_for(level: int) -> str:
    """Açık Soru 4: sınıf seviyesine göre dil sadeleştirmesi.

    Aynı bağlam 2. sınıfa da 11. sınıfa da gidebilir; farkı prompt yaratır.
    """
    if level <= 4:
        return (
            "Çok basit ve kısa cümleler kur. Günlük hayattan örnek ver. "
            "Terim kullanma; kullanman gerekirse hemen açıkla."
        )
    if level <= 8:
        return (
            "Açık ve sade bir dil kullan. Ders terimlerini kullanabilirsin ama "
            "her birini kısaca açıkla."
        )
    return (
        "Ders terimlerini doğrudan kullan. Açıklamayı derinleştir, "
        "gereksiz basitleştirme yapma."
    )


def build_system_prompt(grade: str | None, subject: str | None) -> str:
    level = _grade_level(grade)
    kim = f"{grade} öğrencisi" if grade else "bir ortaokul öğrencisi"
    ders = f" {subject} dersi" if subject else ""

    return f"""Sen MEB ders kitaplarına dayanan bir sesli öğrenme asistanısın.
Karşındaki {kim}, sana{ders} ile ilgili soru soruyor.

KURALLAR

1. Açıkladığın BİLGİ kaynak metinlerden gelmeli.
2. ÖRNEKLER için bu sınır yoktur. Kaynak metinler bir ilkeyi açıklıyorsa, o
   ilkeyi anlatmak için günlük hayattan istediğin örneği kurabilirsin —
   örneğin çaydanlık, buzdolabı, bisiklet. Örneğin kitapta geçmesi GEREKMEZ;
   öğretmenin işi zaten budur. Kurala uyman gereken tek şey, örneğin
   dayandığı ilkenin kaynaklarda yazıyor olması.
3. Öğrenci "başka örnek ver", "daha farklı örnekler ver", "daha basit anlat",
   "özetle", "bana soru sor" derse bu YENİ BİR KONU DEĞİLDİR. Kitapta hazır
   bir örnek listesi olmayabilir; senden beklenen yeni örnek kurmandır. Böyle
   isteklerde ASLA kaynak yok deme.
   KİTABIN KENDİSİ hakkındaki sorular da yeni konu DEĞİLDİR: "bu kitapta
   neler var", "başlıkları sayar mısın", "kaç ünite var", "içindekiler",
   "kitabın içeriği nedir", "hangi konular işleniyor"
   gibi sorular [1] numaralı İÇİNDEKİLER kaynağıyla cevaplanır. Bu bir
   konu sorusu değil, kitabın yapısı sorusudur; elinde cevabı VAR.
   Bunlarda da ASLA kaynak yok deme.
4. Bunun dışındaki sorularda ölçü şudur: kaynak metinler soruyla TAMAMEN
   İLGİSİZ bir konudan bahsediyorsa — yani sorunun konusu bu kitapta hiç
   işlenmemişse. Kararı ANLAMA göre ver: büyük/küçük harf ya da yazılış farkı
   ("Isı" ile "ısı" aynı kelimedir) hiçbir şey değiştirmez. Konunun
   kaynaklarda geçtiğinden şüphen varsa kaynak yok DEME, cevap ver.
   Yalnızca konu gerçekten hiç işlenmemişse cevabının İLK SATIRI tam olarak
   şu olsun:
   {NO_SOURCE_MARKER}
   Bu satır öğrenciye OKUNMAZ, yalnızca sisteme işarettir. Kitapta olmayan konuyla ilgili cevap verme. 
   İKİNCİ SATIRDAN itibaren konuyu bulamadığını KENDİ cümlelerinle söyle; 
   Elindeki kaynak metinlerin başlıklarına bakıp bu kitabın
   hangi konuları kapsadığını kısaca söylemen öğrenciye yol gösterir.
   AMA SORUYU YİNE DE CEVAPLAMA. İşaretten sonra genel bilgiyle açıklama
   yapmak yasak: o satırlar öğrenciye SESLİ OKUNUYOR ve kitapta olmayan
   bir bilgiyi kitaptanmış gibi duyurmuş olursun.
5. {_tone_for(level)}
6. Cevabın SESLİ OKUNACAK. Bu yüzden madde işareti, başlık, tablo, yıldız
   veya herhangi bir biçimlendirme kullanma — düz, akıcı konuşma metni yaz.
   Formülleri kelimelerle söyle ("a kare artı b kare").
7. Türkçe cevap ver."""


def build_context_block(chunks: list[RetrievedChunk],
                        outline: list[str] | None = None) -> str:
    """Kaynak metinleri numaralandırılmış tek bloğa çevirir.

    breadcrumb da veriliyor: model hangi ünitenin anlatıldığını görürse
    konu dışı bir chunk'a yaslanma olasılığı düşüyor.

    ANAHAT DA BURAYA, numaralı bir kaynak olarak giriyor. Ayrı bir başlık
    altında verildiğinde model onu "kaynak metin" saymıyordu: kurallar
    bilginin kaynak metinlerden gelmesini istiyor, anahat ise o kümenin
    dışında duruyordu. Sonuç ölçüldü — "kitabın içeriği nedir" sorusuna
    KAYNAK_YOK deyip kitapla ilgisi olmayan genel bir metin üretti. Aynı
    listeyi numaralı bir kaynak yapmak, kuralları değiştirmeden onu
    kapsama alıyor.
    """
    bloklar: list[str] = []
    if outline:
        basliklar = chr(10).join(f"  {i}. {b}" for i, b in enumerate(outline, 1))
        bloklar.append(
            "[1] KİTABIN İÇİNDEKİLER (bölüm başlıkları, kitaptaki sırayla)"
            + chr(10) + basliklar
        )
    if not chunks and not bloklar:
        return "(Kaynak metin bulunamadı.)"

    baslangic = len(bloklar) + 1
    for i, c in enumerate(chunks, baslangic):
        bloklar.append(f"[{i}] {c.breadcrumb or 'kaynak'}{chr(10)}{c.text}")
    return (chr(10) + chr(10)).join(bloklar)


REWRITE_SYSTEM_PROMPT = """Görevin soruyu ARAMA için tek başına anlaşılır hâle getirmek.

Sana bir sohbet geçmişi ve öğrencinin son sorusu verilecek.

- Son soru önceki konuya atıfta bulunuyorsa (ör. "bununla ilgili", "konuyla
  ilgili", "peki ya", "az önce anlattığın", "başka örnek"), atıfı ÇÖZ ve
  konuyu açıkça yazarak soruyu yeniden kur.
- Son soru kitaptaki bir KONUMA atıfta bulunuyorsa (ör. "son kısım",
  "kitabın sonu", "sondaki ünite", "ilk konu", "başındaki bölüm", "sondan bir
  önceki ünite"), sana verilen bölüm listesinden bunun hangi bölüm olduğunu
  bul ve soruyu O BÖLÜMÜN ADINI yazarak yeniden kur.
- Son soru kendi başına anlaşılıyorsa, HİÇ DEĞİŞTİRMEDEN aynen geri yaz.
  Konu değiştiyse eski konuyu ASLA ekleme.

Yalnızca sorunun son hâlini yaz. Açıklama, tırnak, ön ek ekleme."""


class OpenAiLlmAdapter:
    """LlmAdapter sözleşmesinin OpenAI implementasyonu (ADR-08)."""

    def __init__(self) -> None:
        # stt.py ile aynı gerekçe: ctor anahtarı doğruluyor, tembel kuruyoruz.
        self._client: AsyncOpenAI | None = None

    def _get_client(self) -> AsyncOpenAI:
        if self._client is None:
            self._client = AsyncOpenAI(api_key=settings.openai_api_key)
        return self._client

    async def rewrite_query(
        self,
        question: str,
        history: list[dict[str, str]],
        outline: list[str] | None = None,
    ) -> str:
        """Resolve references so the search query stands on its own.

        A word list cannot do this job: Turkish refers back in unbounded ways
        ("konuyla ilgili", "az önce anlattığın", "aynı konuda"), and the two
        errors pull in opposite directions — missing a follow-up searches text
        with no content words, while wrongly combining destroys a real topic
        change (measured: adding the previous question to "basınç birimi
        nedir" returned three heat chunks and zero pressure ones).

        Only the last few turns are sent, and the output is capped, so this
        stays a small call.
        """
        # Anahat KULLANICI mesajına konuyor, sistem promptuna değil: bu bir
        # kural değil, o kitaba ait veri. Kitap değiştiğinde kendiliğinden
        # değişmesi gerekiyor ve sistem promptu kitaptan bağımsız kalmalı.
        #
        # Konum sorularını çözen şey bu: "kitabın son kısmındaki konu" ifadesi
        # bölüm listesini gören bir modelde gerçek bölüm adına dönüşüyor,
        # arama da sıradan bir konu aramasına indirgeniyor.
        icerik = f"Son soru: {question}"
        if outline:
            bolumler = "\n".join(f"{i}. {b}" for i, b in enumerate(outline, 1))
            icerik = (
                f"Kitabın bölümleri (kitaptaki sırayla):\n{bolumler}\n\n{icerik}"
            )

        response = await self._get_client().chat.completions.create(
            model=settings.llm_model,
            messages=[
                {"role": "system", "content": REWRITE_SYSTEM_PROMPT},
                *history[-4:],
                {"role": "user", "content": icerik},
            ],
            temperature=0,  # deterministic: same question, same query
            max_tokens=80,
        )
        rewritten = (response.choices[0].message.content or "").strip()
        # Never let a failed rewrite drop the real question.
        return rewritten or question

    async def stream_answer(
        self,
        question: str,
        chunks: list[RetrievedChunk],
        *,
        grade: str | None = None,
        history: list[dict[str, str]] | None = None,
        outline: list[str] | None = None,
    ) -> AsyncIterator[str]:
        subject = chunks[0].subject if chunks else None
        messages: list[dict[str, str]] = [
            {"role": "system", "content": build_system_prompt(grade, subject)},
        ]
        # Kısa dönem bağlam (4.2: son 3-5 tur). Kaynak bloğundan ÖNCE geliyor;
        # böylece modelin en yakın gördüğü metin bu turun kaynakları olur.
        if history:
            messages.extend(history)
        # KİTABIN BÖLÜM LİSTESİ de kaynaklara katiliyor.
        #
        # Arama en fazla top_k parça getiriyor (şu an 3). "Bu kitapta neler
        # var" gibi bir soruda bu üç parça hangi bölümden geldiyse cevap da
        # yalnızca onu anlatıyordu. Bölüm listesi kitabın YAPISINI veriyor,
        # parçalar ise İÇERİĞİNİ.
        #
        # Liste AYRI bir başlıkta değil, NUMARALI BİR KAYNAK olarak veriliyor
        # (build_context_block). Ayrı başlıkta verildiğinde model onu kaynak
        # saymayıp KAYNAK_YOK diyordu — ölçüldü.
        messages.append({
            "role": "user",
            "content": (f"KAYNAK METİNLER:{chr(10)}"
                        f"{build_context_block(chunks, outline)}"
                        f"{chr(10)}{chr(10)}SORU: {question}"),
        })

        stream = await self._get_client().chat.completions.create(
            model=settings.llm_model,
            messages=messages,
            temperature=settings.temperature,
            max_tokens=settings.max_tokens,
            stream=True,
        )
        async for event in stream:
            if not event.choices:
                continue
            delta = event.choices[0].delta.content
            if delta:
                yield delta


llm_adapter = OpenAiLlmAdapter()
