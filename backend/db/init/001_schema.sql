-- Sohbet ve tur ölçümü şeması (ADR-07, doküman 4.2).
--
-- Bu dosya iki yoldan çalışır:
--   1. Konteyner İLK kez oluşurken docker-entrypoint-initdb.d tarafından.
--   2. Her açılışta uygulama tarafından (app/conversation/store.py).
-- İkincisi şart, çünkü init dizini yalnızca volume boşken çalışır; şema
-- sonradan eklendiğinde mevcut kurulumlar onu hiç görmez. Bu yüzden her ifade
-- IF NOT EXISTS ile yazıldı — iki kez çalışması zararsız.

CREATE TABLE IF NOT EXISTS conversations (
    id               BIGSERIAL PRIMARY KEY,
    device_id        TEXT,
    grade            TEXT,
    subject          TEXT,
    started_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_message_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS messages (
    id               BIGSERIAL PRIMARY KEY,
    conversation_id  BIGINT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    role             TEXT NOT NULL CHECK (role IN ('user', 'assistant')),
    content          TEXT NOT NULL,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),

    -- Aşağıdakiler yalnızca assistant satırlarında dolu olur.

    -- Cevabın hangi chunk'lara dayandığı: chunk_id, breadcrumb, grade,
    -- subject, skor. Panelde kaynak gösterimi bunu okur (doküman 4.2).
    retrieved_chunks JSONB,

    -- O turda GERÇEKTEN kullanılan ayarlar. Panelden top_k/temperature
    -- değiştirilebildiği için (4.4), sabit bir yapılandırmayı sonradan
    -- okumak yanıltıcı olurdu; tur anındaki değer kaydediliyor.
    model_config     JSONB,

    -- stt_ms, retrieval_ms, llm_first_token_ms, tts_first_byte_ms, ...
    latencies        JSONB,

    -- Model "bu konuyu kitabında bulamadım" dediyse true.
    no_source        BOOLEAN NOT NULL DEFAULT FALSE
);

-- Panelin ana sorgusu "son sohbetler", bu yüzden zamana göre azalan index.
CREATE INDEX IF NOT EXISTS messages_conversation_idx
    ON messages (conversation_id, created_at);
CREATE INDEX IF NOT EXISTS conversations_recent_idx
    ON conversations (last_message_at DESC);

-- Ham ses SAKLANMAZ (doküman 4.2). Kayıtta yalnızca metin ve ölçüm var.
