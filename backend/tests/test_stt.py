"""_pcm_to_wav testi.

Saf fonksiyon: ağa çıkmıyor, mock gerekmiyor. OpenAI'a hiç gitmeden, WAV
başlığının config'teki ses ayarlarıyla (16kHz, mono, 16-bit) birebir
eştiğini burada doğruluyoruz.
"""

import wave

from app.adapters.stt import _pcm_to_wav
from app.config import settings


def test_pcm_to_wav_basligi_config_ile_esiyor():
    pcm = b"\x00\x01" * settings.audio_sample_rate  # 1 saniyelik sahte ses

    wav_buffer = _pcm_to_wav(pcm)

    with wave.open(wav_buffer, "rb") as wav_file:
        assert wav_file.getnchannels() == settings.audio_channels
        assert wav_file.getsampwidth() == settings.audio_sample_width
        assert wav_file.getframerate() == settings.audio_sample_rate
        assert wav_file.readframes(wav_file.getnframes()) == pcm
