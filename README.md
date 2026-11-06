[English](README.md) | [Türkçe](README.tr.md)

# Curriculum-Grounded, RAG-Backed Voice AI Assistant

A standalone device that lets students ask questions out loud and get spoken
answers from their own textbook. THINKBRO internship, July – September 2026.

Claude Code was my main coding tool. The textbook data was cleaned and chunked
as a team effort, and another team produced the vectors.

The student holds down the panel on the device, asks a question and lets go.
Within a few seconds they hear the answer and see on screen which unit it came
from.

The answer is generated from the MEB (Turkish national curriculum) textbook for
the student's own grade, not from a general language model. If the topic is not
in the book, the system does not make something up; it says it could not find
it. This is the project's defining claim: not just a correct answer, but **an
answer whose source can be shown**.

> Case study, photos and voice demo:
> [tunakimyonok-portfolio.vercel.app/projects/meb-voice-assistant](https://tunakimyonok-portfolio.vercel.app/projects/meb-voice-assistant/)

## Measured results

| Measurement | Result |
|---|---|
| Time to first audio, first measurement | 11,020 ms |
| Time to first audio, after the streaming chain | 2,697 – 3,777 ms |
| Correct refusals on out-of-scope questions | 6 / 6 |
| Correct answers on in-scope questions | 6 / 6 |
| Automated tests | 37, all passing |
| Indexed text chunks | 8,577 (12 grade levels) |

Latency was measured over four consecutive runs: 2697, 5592, 2717 and 3777 ms.
Three met the target. The run that missed it was caused by the provider being
slow that day (the language model's first token, normally 600–1000 ms, rose to
3378 ms).

## How a turn works

```
Recording      →  STT         →  Search         →  Answer         →  Audio
16 kHz PCM,       speech is      hybrid search     LLM relies only   spoken sentence
streamed over     transcribed    filtered by       on these chunks   by sentence and
WebSocket                        grade/subject                       sent right away
```

The whole chain is streaming, so nothing waits for the full answer. As soon as
the first sentence is finished it is spoken and sent, and the device is already
talking while the remaining sentences are generated.

## Architecture

| Layer | Contents |
|---|---|
| Device | ESP32-S3. Recording, playback, UI and WebSocket client. |
| Backend | A single FastAPI process: WebSocket gateway, turn orchestration, provider adapters, panel. |
| Knowledge base | Qdrant. MEB textbooks are chunked, and each chunk is tagged with grade, subject and unit. |
| Persistence | PostgreSQL. For each turn: question, answer, the sources it relied on, stage latencies, model settings. |
| Panel | Live monitoring from the browser, past conversations, settings changes at runtime. |

## Hardware

| Component | Role | Interface |
|---|---|---|
| ESP32-S3 (8 MB flash, 2 MB PSRAM) | Main processor, Wi-Fi | — |
| ILI9341 2.4" touch display | UI, push-to-talk panel | SPI |
| XPT2046 | Touch reading, calibration | SPI |
| INMP441 | Audio capture | I2S |
| MAX98357A + 8 Ω speaker | Audio playback | I2S |

There is no audio decoder on the device: the backend sends uncompressed audio
and the bytes are written straight to the hardware. This lowered latency and
removed a whole layer of code from the device.

## Running it

Requirements: Docker Desktop, Python 3.12+, and ESP-IDF for the firmware.

### 1. Stateful services

```bash
docker compose up -d
```

Qdrant runs on `:6333` (dashboard: http://localhost:6333/dashboard) and
PostgreSQL on `:5432`. The schema is created from
`backend/db/init/001_schema.sql` when the container first starts.

### 2. Backend

```bash
cp backend/.env.example backend/.env    # fill in: OPENAI_API_KEY, QDRANT_URL, POSTGRES_DSN, PANEL_PASSWORD
python -m venv backend/.venv
backend/.venv/Scripts/pip install -r backend/requirements.txt
```

To start it, from the project root:

```
backend\baslat.bat          :: demo mode
backend\baslat.bat izle     :: --reload, for working on the prompt
```

`baslat.bat` first stops any old process listening on the same port. This is
deliberate: without `--reload`, uvicorn imports the module once at startup and
keeps it in memory. A prompt change in `llm.py` therefore does not affect the
running server, and nothing reports an error: the device simply keeps talking
with the old prompt.

### 3. Knowledge base

**The vector data is not in this repository.** It is generated data and contains
text chunks from MEB textbooks, so it was kept out.

`scripts/ingest_qdrant.py` only handles the parquet → Qdrant transfer and expects
the dense and sparse vectors to be computed already:

```bash
cd backend
.venv\Scripts\pip install -r requirements-ingest.txt
.venv\Scripts\python.exe scripts\ingest_qdrant.py --recreate
```

The offline pipeline that produces its input, the `vectors/v2/**/*.parquet` files
(Docling → Chonkie → FastEmbed), is not in this repository. Cloning the repository
alone is not enough to rebuild the knowledge base from scratch; the parquet files
must be supplied separately.

### 4. Firmware

```
firmware\derle.bat
firmware\derle.bat flash monitor
```

> **Warning:** `derle.bat` and `baslat.bat` contain absolute paths from the
> development machine (ESP-IDF location, Python path). Update those lines before
> running them on another machine.

## Repository layout

```
backend/
├── app/
│   ├── adapters/       STT · LLM · TTS behind a contract (changing a provider touches one file)
│   ├── gateway/        WebSocket gateway
│   ├── orchestrator/   turn flow, cancellable asyncio tasks
│   ├── rag/            hybrid search, RRF fusion
│   ├── conversation/   follow-up question resolution, conversation history
│   └── panel/          admin panel (server-side templates + SSE)
├── db/init/            PostgreSQL schema
├── scripts/            ingest, end-to-end tests, evaluation scripts
└── tests/
firmware/               ESP-IDF project (main/, sdkconfig, partitions.csv)
docs/                   project summary, implementation plan, demo guide, manual tests
```

## Known limits

- One concurrent user: a deliberate scope decision.
- Local network with unencrypted connections, assuming a classroom setting.
- The panel is not a real authorization boundary, just a single-password gate.
- The on-screen keyboard has no Turkish characters.

## Documents

These documents are in Turkish.

| File | Contents |
|---|---|
| [docs/proje-ozeti.pdf](docs/proje-ozeti.pdf) | Project summary and technical report |
| [docs/uygulama-plani.md](docs/uygulama-plani.md) | Measurements and details of the bug fixes |
| [docs/demo-kilavuzu.md](docs/demo-kilavuzu.md) | Demo day steps |
| [docs/manuel-test.md](docs/manuel-test.md) | Manual test scenarios |
| [Mimari_Tasarim_Dokumani.md](MEB%20KAYNAKLI%20VE%20RAG%20DESTEKL%C4%B0%20SESL%C4%B0%20AI%20AS%C4%B0STAN%20Mimari_Tasarim_Dokumani.md) | Full reasoning behind the decisions (ADRs) |
