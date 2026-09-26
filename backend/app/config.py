"""Tek yapılandırma kaynağı.

Her modül (rag, adapters, conversation, panel) ayarları burada okur; hiçbir modül
kendi başına os.environ'a bakmaz. Sebep: aynı ayarın iki yerde farklı varsayılanla
durması, demo günü hata ayıklaması en zor sorun tipidir.

Alanlar iki gruba ayrılır:
  - Sabit ayarlar (.env'den gelir, süreç boyunca değişmez): anahtarlar, adresler.
  - Runtime ayarları (panelden değiştirilebilir, ADR bkz. 7. bölüm): top_k,
    temperature, max_tokens, tts_model/voice.
"""

from functools import lru_cache

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        extra="ignore",
        case_sensitive=False,
    )

    # --- Sağlayıcı: STT / LLM / TTS (ADR-08) ---
    openai_api_key: str = ""
    stt_model: str = "gpt-4o-mini-transcribe"
    llm_model: str = "gpt-4o-mini"
    tts_model: str = "gpt-4o-mini-tts"
    tts_voice: str = "alloy"

    # --- Retrieval (ADR-06) ---
    qdrant_url: str = "http://localhost:6333"
    qdrant_collection: str = "meb-books-v2"
    # DİKKAT: bu iki model, koleksiyon oluşturulurken kullanılanlarla birebir
    # aynı olmak zorunda. Koleksiyon 1024 boyutlu ve hazır; burası değişirse
    # arama sonuçları anlamını kaybeder.
    dense_model: str = "intfloat/multilingual-e5-large"
    sparse_model: str = "Qdrant/bm25"
    dense_vector_size: int = 1024
    # Ölçülerek bulundu: korpus Türkçe stemmer ile üretilmiş. Varsayılan
    # "english" ile üretilen indekslerin yalnızca %42'si tutuyordu, yani
    # BM25 kolu sessizce yarı yarıya boşa çalışıyordu.
    bm25_language: str = "turkish"
    # ~2.2 GB'lık e5 modeli buraya iner; süreç yeniden başlayınca tekrar inmesin.
    fastembed_cache_dir: str = r"C:\Users\tunak\.cache\fastembed"

    # --- Runtime ayarları (panelden değiştirilebilir) ---
    # 5'ten 3'e cekildi (plan 2.1): baglam ~3180 -> ~1897 token, LLM ilk token
    # 1009 -> 781 ms. Eval 12/12 kaldi, yani kalite bedeli olculemedi.
    top_k: int = 3
    # RRF birleştirmesinden önce her koldan kaç aday çekilsin.
    # top_k * bu çarpan. Küçük olursa iki kol da aynı sonuçları getirip
    # füzyon anlamsızlaşır; büyük olursa Qdrant boşuna iş yapar.
    prefetch_multiplier: int = 4
    temperature: float = 0.5
    max_tokens: int = 400

    # --- Kalıcılık (ADR-07) ---
    postgres_dsn: str = "postgresql://meb:meb@localhost:5432/meb"

    # --- Ses formatı (Açık Soru 5: INMP441 + STT sağlayıcısı ile doğrulanacak) ---
    audio_sample_rate: int = 16000
    audio_channels: int = 1
    audio_sample_width: int = 2  # 16-bit PCM

    # TTS cikis bicimi. Olculdu (plan 2.1): akisli PCM'de ilk bayt 683 ms,
    # akisli mp3'te 1282 ms, akissiz her bicimde ~1700 ms. PCM ayrica cihazda
    # mp3 cozucuye gerek birakmiyor; ornekler dogrudan I2S'e verilebiliyor.
    # Bedeli ~3x daha buyuk yuk (yerel agda sorun degil).
    tts_format: str = "pcm"
    tts_sample_rate: int = 24000  # OpenAI PCM cikis hizi

    # --- Sunucu / panel (ADR-10: düz ws://, yerel ağ) ---
    host: str = "0.0.0.0"
    port: int = 8000
    panel_password: str = "demo"

    # --- Hedef (7. bölüm) ---
    first_audio_target_ms: int = 4000


@lru_cache
def get_settings() -> Settings:
    """Süreç ömrü boyunca tek Settings örneği.

    lru_cache sayesinde .env yalnızca bir kez okunur; panelden yapılan runtime
    değişiklikleri bu tek örneğin üstünde yaşar ve tüm modüller anında görür.
    """
    return Settings()


settings = get_settings()
