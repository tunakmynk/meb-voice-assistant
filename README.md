# Curriculum-Grounded Voice AI Assistant

A push-to-talk device that answers students' questions out loud from Turkish
national curriculum (MEB) textbooks and shows the source of each answer.
Built during my THINKBRO internship (Jul–Sep 2026) with Claude Code as the main
coding tool. The knowledge base was built as a team effort.

- **Stack:** Python, FastAPI, WebSocket, PostgreSQL, Qdrant, Docker Compose, ESP32-S3 (ESP-IDF)
- **Latency:** time to first audio fell from 11.0 s to 2.7–3.8 s in 3 of 4 measured runs
- **Evaluation:** 6/6 correct refusals out of scope, 6/6 correct answers in scope
- **Tests:** 37 pytest tests; most use an in-memory store rather than a real database

The rest of this README is in Turkish.
Case study and demo: https://tunakimyonok-portfolio.vercel.app/projects/meb-voice-assistant/

---

# MEB Kaynaklı, RAG Destekli Sesli AI Asistan

Öğrencinin kendi ders kitabından konuşarak cevap alabildiği bağımsız cihaz.
THINKBRO stajı, Temmuz – Eylül 2026.

Öğrenci cihazdaki panele basılı tutar, sorusunu sorar, bırakır. Birkaç saniye
içinde sesli cevabı duyar ve ekranda cevabın hangi üniteden geldiğini görür.

Cevap genel bir dil modelinden değil, öğrencinin kendi sınıfına ait MEB ders
kitabından üretilir. Konu kitapta yoksa sistem uydurmaz, bulamadığını söyler.
Projenin ayırt edici iddiası budur: doğru cevap değil, **kaynağı gösterilebilir
cevap**.

> Vaka analizi, fotoğraflar ve sesli demo:
> [tunakimyonok-portfolio.vercel.app/projects/meb-voice-assistant](https://tunakimyonok-portfolio.vercel.app/projects/meb-voice-assistant/)

## Ölçülen sonuçlar

| Ölçüm | Sonuç |
|---|---|
| İlk sese kadar geçen süre — ilk ölçüm | 11.020 ms |
| İlk sese kadar — akışlı zincirden sonra | 2.697 – 3.777 ms |
| Kapsam dışı soruda doğru reddetme | 6 / 6 |
| Kapsam içi soruda doğru cevaplama | 6 / 6 |
| Otomatik test | 37, tamamı geçiyor |
| İndekslenen metin parçası | 8.577 (12 sınıf düzeyi) |

Gecikme dört ardışık koşuda ölçüldü: 2697, 5592, 2717 ve 3777 ms. Üçü hedefi
karşıladı; aşan koşuda sebep sağlayıcının o günkü yavaşlığıydı (dil modelinin
ilk kelimesi normalde 600–1000 ms sürerken 3378 ms'ye çıktı).

## Bir turun akışı

```
Kayıt          →  STT         →  Arama          →  Cevap          →  Ses
16 kHz PCM,       ses yazıya     sınıf/ders        LLM yalnızca      cümle cümle
WebSocket'ten     çevrilir       filtresiyle       bu parçalara      seslendirilip
akar                             hibrit arama      dayanır           anında gönderilir
```

Zincirin tamamı akışlıdır: cevabın tamamı beklenmez. İlk cümle biter bitmez
seslendirilip yola çıkar, kalan cümleler üretilirken cihaz zaten konuşmaktadır.

## Mimari

| Katman | İçerik |
|---|---|
| Cihaz | ESP32-S3. Kayıt, çalma, arayüz ve WebSocket istemcisi. |
| Backend | Tek FastAPI süreci: WebSocket ağ geçidi, tur orkestrasyonu, sağlayıcı adapterleri, panel. |
| Bilgi tabanı | Qdrant — MEB kitapları parçalanmış, her parça sınıf/ders/ünite bilgisiyle etiketli. |
| Kayıt | PostgreSQL — her tur: soru, cevap, dayandığı kaynaklar, aşama gecikmeleri, model ayarları. |
| Panel | Tarayıcıdan canlı izleme, geçmiş sohbetler, çalışma anında ayar değişimi. |

## Donanım

| Bileşen | Görevi | Arayüz |
|---|---|---|
| ESP32-S3 (8 MB flash, 2 MB PSRAM) | Ana işlemci, Wi-Fi | — |
| ILI9341 2.4" dokunmatik ekran | Arayüz, bas-konuş paneli | SPI |
| XPT2046 | Dokunma okuma, kalibrasyon | SPI |
| INMP441 | Ses yakalama | I2S |
| MAX98357A + 8 Ω hoparlör | Ses çalma | I2S |

Cihazda ses çözücü yok: backend sıkıştırılmamış ses gönderiyor ve baytlar
doğrudan donanıma yazılıyor. Bu hem gecikmeyi düşürdü hem de cihazdan bütün
bir kod katmanını kaldırdı.

## Çalıştırma

Gereksinimler: Docker Desktop, Python 3.12+, firmware için ESP-IDF.

### 1. Durum tutan servisler

```bash
docker compose up -d
```

Qdrant `:6333` (panel: http://localhost:6333/dashboard), PostgreSQL `:5432`.
Şema `backend/db/init/001_schema.sql` ile konteyner ilk açılışta kurulur.

### 2. Backend

```bash
cp backend/.env.example backend/.env    # doldur: OPENAI_API_KEY, QDRANT_URL, POSTGRES_DSN, PANEL_PASSWORD
python -m venv backend/.venv
backend/.venv/Scripts/pip install -r backend/requirements.txt
```

Başlatmak için, proje kökünden:

```
backend\baslat.bat          :: demo modu
backend\baslat.bat izle     :: --reload, prompt üzerinde çalışırken
```

`baslat.bat` aynı portu dinleyen eski süreci önce kapatır. Bu tesadüfi değil:
`--reload` olmadan uvicorn modülü açılışta bir kez içe aktarıp bellekte tutuyor,
dolayısıyla `llm.py` içindeki prompt değişikliği çalışan sunucuyu etkilemiyor ve
bu hiçbir yerde hata vermiyor — cihaz eski promptla konuşmaya devam ediyor.

### 3. Bilgi tabanı

**Vektör verisi bu depoda yok.** Üretilmiş veri olduğu ve MEB ders kitaplarının
metin parçalarını içerdiği için dışarıda tutuldu.

`scripts/ingest_qdrant.py` yalnızca parquet → Qdrant aktarımını yapar; dense ve
sparse vektörlerin hesaplanmış olmasını bekler:

```bash
cd backend
.venv\Scripts\pip install -r requirements-ingest.txt
.venv\Scripts\python.exe scripts\ingest_qdrant.py --recreate
```

Bunun girdisi olan `vectors/v2/**/*.parquet` dosyalarını üreten çevrimdışı hat
(Docling → Chonkie → FastEmbed) bu depoda bulunmuyor. Yani depo tek başına
klonlanarak bilgi tabanı sıfırdan kurulamaz; parquet dosyalarının ayrıca
sağlanması gerekir.

### 4. Firmware

```
firmware\derle.bat
firmware\derle.bat flash monitor
```

> **Uyarı:** `derle.bat` ve `baslat.bat`, geliştirme makinesine ait mutlak
> yollar içeriyor (ESP-IDF konumu, Python yolu). Başka bir makinede çalıştırmadan
> önce bu satırlar güncellenmeli.

## Depo düzeni

```
backend/
├── app/
│   ├── adapters/       STT · LLM · TTS, sözleşme arkasında (sağlayıcı değişimi tek dosya)
│   ├── gateway/        WebSocket ağ geçidi
│   ├── orchestrator/   tur akışı, iptal edilebilir asyncio görevleri
│   ├── rag/            hibrit arama, RRF birleştirme
│   ├── conversation/   takip sorusu çözümleme, sohbet geçmişi
│   └── panel/          yönetim paneli (sunucu taraflı şablon + SSE)
├── db/init/            PostgreSQL şeması
├── scripts/            ingest, uçtan uca testler, değerlendirme betikleri
└── tests/
firmware/               ESP-IDF projesi (main/, sdkconfig, partitions.csv)
docs/                   proje özeti, uygulama planı, demo kılavuzu, manuel test
```

## Bilinen sınırlar

- Tek eşzamanlı kullanıcı — bilinçli bir kapsam kararı.
- Yerel ağ, şifresiz bağlantı; sınıf ortamı varsayımı.
- Panel gerçek bir yetki sınırı değil, tek şifreli bir kapı.
- Dokunmatik klavyede Türkçe karakter yok.

## Dokümanlar

| Dosya | İçerik |
|---|---|
| [docs/proje-ozeti.pdf](docs/proje-ozeti.pdf) | Proje özeti ve teknik rapor |
| [docs/uygulama-plani.md](docs/uygulama-plani.md) | Ölçümler, hata çözümlerinin ayrıntısı |
| [docs/demo-kilavuzu.md](docs/demo-kilavuzu.md) | Demo günü adımları |
| [docs/manuel-test.md](docs/manuel-test.md) | Elle test senaryoları |
| [Mimari_Tasarim_Dokumani.md](MEB%20KAYNAKLI%20VE%20RAG%20DESTEKL%C4%B0%20SESL%C4%B0%20AI%20AS%C4%B0STAN%20Mimari_Tasarim_Dokumani.md) | Kararların tam gerekçeleri (ADR'ler) |
