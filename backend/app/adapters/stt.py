"""STT adapter sözleşmesi.

ADR-08: STT/LLM/TTS sağlayıcıları ayrı arayüzler arkasında durur; orchestrator
hiçbir zaman doğrudan OpenAI SDK'sına dokunmaz. Bugün tek sağlayıcı (OpenAI)
olsa da, yarın ikinci bir sağlayıcı eklenirse (R-04: tek sağlayıcı kesintisi)
orchestrator kodu hiç değişmemeli — sadece hangi adapter'ın kullanıldığı
değişmeli.
"""

from __future__ import annotations

import io
import wave
from typing import Protocol

from openai import AsyncOpenAI

from app.config import settings


class SttAdapter(Protocol):
    async def transcribe(self, pcm_audio: bytes, *, subject: str | None = None) -> str:
        """Ham PCM ses baytlarını (config: 16kHz, mono, 16-bit) metne çevirir.

        `subject` verilirse o dersin terimleri STT'ye ipucu olarak geçilir.
        """
        ...


# Ders bazlı terim ipuçları.
#
# Cihazda ölçüldü: kayıt kalitesi iyi (%98.9 örnek, uzun cümleler kusursuz
# çevriliyor) ama KISA terimler yanlış duyuluyordu — "ısı" üç denemede
# "sıvı", "süre" ve "su" olarak geçti. Sebep donanım değil: "ısı" Türkçe'de
# kısa ve akustik olarak bu kelimelere çok yakın.
#
# Hangi dersten soru sorulduğunu zaten biliyoruz (cihaz profili gönderiyor).
# OpenAI transkripsiyon ucu `prompt` ile kelime dağarcığı ipucu kabul ediyor;
# doğru terimleri vermek modelin o yöne eğilmesini sağlıyor.
_DERS_TERIMLERI: dict[str, str] = {
    "Fen Bilimleri": (
        "ısı, sıcaklık, yoğunluk, basınç, hâl değişimi, buharlaşma, yoğuşma, "
        "erime, donma, genleşme, büzülme, kütle, hacim, madde, enerji, kuvvet"
    ),
    "Fizik": (
        "ısı, sıcaklık, basınç, kuvvet, enerji, iş, güç, hız, ivme, elektrik, "
        "manyetizma, dalga, optik"
    ),
    "Kimya": (
        "atom, molekül, element, bileşik, tepkime, asit, baz, tuz, mol, "
        "çözelti, derişim, bağ"
    ),
    "Biyoloji": (
        "hücre, hücre zarı, sitoplazma, çekirdek, DNA, gen, protein, enzim, "
        "fotosentez, solunum, mitoz, mayoz"
    ),
    "Matematik": (
        "sayı, kesir, ondalık, yüzde, oran, orantı, denklem, üslü, köklü, "
        "açı, üçgen, dörtgen, çember, alan, hacim, çevre"
    ),
    "Coğrafya": (
        "iklim, hava durumu, sıcaklık, yağış, basınç, rüzgâr, nem, harita, "
        "ölçek, enlem, boylam, yer şekilleri"
    ),
    "Tarih": (
        "antlaşma, savaş, devlet, padişah, imparatorluk, fetih, ıslahat, "
        "cumhuriyet, inkılap"
    ),
    "T.C. İnkılap Tarihi ve Atatürkçülük": (
        "Mustafa Kemal Atatürk, Kurtuluş Savaşı, Lozan, Mudanya, Sakarya, "
        "cumhuriyet, inkılap, TBMM, misakımillî"
    ),
    "İnkılap Tarihi ve Atatürkçülük": (
        "Mustafa Kemal Atatürk, Kurtuluş Savaşı, Lozan, cumhuriyet, inkılap, "
        "TBMM, misakımillî"
    ),
}


def _ipucu(subject: str | None) -> str | None:
    """Derse göre terim ipucu. Bilinmeyen derste ipucu verilmez — yanlış
    dersin terimlerini vermek doğruluğu düşürürdü."""
    if not subject:
        return None
    terimler = _DERS_TERIMLERI.get(subject)
    if not terimler:
        return None
    return f"Bir öğrenci {subject} dersinden soru soruyor. Geçebilecek terimler: {terimler}."


def _pcm_to_wav(pcm_audio: bytes) -> io.BytesIO:
    """Ham PCM baytlarını bellekte bir WAV konteynerine sarar.

    OpenAI'ın transkripsiyon ucu dosya bekler; cihazdan gelen veriyse çıplak
    PCM örnekleridir (RIFF/WAV başlığı yok — sadece sayılar). Aradaki farkı
    kapatmak için stdlib'in wave modülünü kullanıyoruz; diske hiç dokunmadan,
    tek async süreçte (ADR-01) gereksiz G/Ç eklemeden.
    """
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as wav_file:
        wav_file.setnchannels(settings.audio_channels)
        wav_file.setsampwidth(settings.audio_sample_width)
        wav_file.setframerate(settings.audio_sample_rate)
        wav_file.writeframes(pcm_audio)
    buffer.seek(0)
    # OpenAI SDK, içeriğin formatını dosya adının uzantısından çıkarır;
    # BytesIO'ya sonradan isim vermek bu çıkarımı çalıştırmanın en ucuz yolu.
    buffer.name = "speech.wav"
    return buffer


class OpenAiSttAdapter:
    """SttAdapter sözleşmesinin OpenAI implementasyonu (ADR-08)."""

    def __init__(self) -> None:
        # Tembel kurulum: AsyncOpenAI() ctor'da anahtarı doğruluyor. Burada
        # eagerly kursaydık, modülü sadece _pcm_to_wav'ı test etmek için
        # import etmek bile geçerli bir OPENAI_API_KEY ister hale gelirdi.
        self._client: AsyncOpenAI | None = None

    def _get_client(self) -> AsyncOpenAI:
        if self._client is None:
            self._client = AsyncOpenAI(api_key=settings.openai_api_key)
        return self._client

    async def transcribe(self, pcm_audio: bytes, *, subject: str | None = None) -> str:
        wav = _pcm_to_wav(pcm_audio)
        ipucu = _ipucu(subject)
        response = await self._get_client().audio.transcriptions.create(
            model=settings.stt_model,
            file=wav,
            **({"prompt": ipucu} if ipucu else {}),
            # Dil ipucu vermezsek kısa/belirsiz söylemlerde model yanlış dile
            # kayabilir; korpus ve kullanıcı kitlesi tamamen Türkçe.
            language="tr",
        )
        return response.text


# retriever.py'deki tekil nesne kalıbıyla aynı: HTTP istemcisi süreç boyunca
# bir kez kurulur, orchestrator bunu doğrudan import edip kullanır.
stt_adapter = OpenAiSttAdapter()
