## Mimari Tasarım Dokümanı

## MEB Kaynaklı RAG Destekli Sesli AI Asistan — Demo Sürümü

Tarih: 16 Ağustos 2026 Kapsam: Tek geliştirici, 2 hafta, tek cihazlı çalışan demo Durum: Tasarım onaylandı, uygulamaya hazır

## 1. Sistem Özeti

Sistem, öğrencinin ESP32-S3 tabanlı fiziksel bir cihazda ekrana basılı tutarak sorduğu soruyu, MEB ders kitaplarından oluşturulmuş hibrit (dense + sparse) bir vektör veritabanında arayıp LLM ile cevaplayan ve cevabı hem sesli hem ekranda kaynak bilgisiyle sunan bir sesli asistandır. Cihaz ince istemcidir (thin client): ses yakalar, ses çalar, ekran çizer; tüm mantık tek bir Python/FastAPI sürecinde (modüler monolit) toplanmıştır. STT, LLM ve TTS dış API’ler üzerinden, adapter arayüzleri arkasında çalışır ve zincir akışlıdır — LLM’den gelen her cümle anında TTS’e verilerek ilk sese kadar geçen sürenin 4 saniyenin altında kalması hedeflenir. Veli/öğretmen paneli aynı süreçten sunulur ve canlı cihaz durumu, sohbet geçmişi ve arama/model parametrelerini gösterir. Vektör verisi (parse → chunk → embed → index hattı) proje başlamadan önce üretilmiş ve Qdrant’a yüklenmiş durumdadır; bu, iki haftalık sürenin ses ve firmware tarafına ayrılmasını mümkün kılan temel avantajdır.

## 1.1 Donanım Envanteri

| Bileşen | Model | Rol | Arayüz |
| --- | --- | --- | --- |
| İşlemci kartı | ESP32-S3 N8R2 (Wi-Fi + | Cihazın tamamını sürer: ses | 8 MB flash, 2 MB PSRAM |
|   | Bluetooth) | yakalama/çalma, ekran, ağ |   |
| Mikrofon | INMP441 | Öğrencinin sorusunu dijital | I2S (giriş) |
|   |   | olarak yakalar |   |
| Amplifikatör | MAX98357A | Backend’den gelen ses akışını | I2S (çıkış), Class-D |
|   |   | hoparlöre sürer |   |
| Ekran | 2.4” dokunmatik TFT, ILI9341 | Kurulum, sınıf/ders seçimi, | SPI (ekran) + dokunmatik |
|   |   | durum animasyonu, cevap metni | denetleyici |

## Mimariyi etkileyen donanım gerçekleri:

- I2S dijital ses. Hem INMP441 hem MAX98357A I2S üzerinden çalışır; analog ADC/DAC ve harici ses kodeki gerekmez. Kayıt ve çalma iki ayrı I2S kanalı olarak yapılandırılır.

- PSRAM (2 MB) ses tamponu için kritiktir. Dahili RAM ses tamponu, LVGL çerçeve tamponu ve TLS/WebSocket tamponlarını aynı anda taşımakta zorlanır; ses tamponları PSRAM’e alınır (V-05).

- Bluetooth kullanılmıyor. Kart destekliyor olsa da tüm iletişim Wi-Fi üzerinden; Bluetooth yığını kapatılarak bellek ve derleme boyutu kazanılır.

- Tek hoparlör, bas-konuş. MAX98357A mono çıkış verir; ADR-02 gereği kayıt ve çalma aynı anda olmadığı için yankı engelleme gerekmez.

- Ekran çözünürlüğü (320×240) metin tasarımını sınırlar. Cevap metni kaydırılabilir sayfa olarak gösterilir; Türkçe karakter içeren bir LVGL fontu gömülmelidir.

- Dokunmatik klavye yavaştır. Wi-Fi şifresi ve backend adresi bu ekrandan girileceği için değerler NVS’te kalıcı saklanır (ADR-09).

- Pin çakışması riski. ILI9341 (SPI) ile iki I2S hattının pin ataması, kart üzerindeki ayrılmış pinlerle çakışmayacak şekilde ilk gün sabitlenmelidir (bkz. Açık Sorular).

## 2. Bağlam Diyagramı


Sınırlar: Cihaz panelle doğrudan haberleşmez. Panel cihazı yalnızca backend üzerinden izler. Ham ses hiçbir kalıcı

depoya yazılmaz; yalnızca STT sağlayıcısına akar.

## 3. Bileşen Diyagramı


- 3.1 Modül Sorumlulukları


| Modül device-gateway | Sorumluluk Bağımlılık WebSocket oturumu, olay sözleşmesi, ses orchestrator parçalarının aktarımı |
| --- | --- |
| orchestrator | Tur akışını yürütür, iptal edilebilir görev, stt, rag, llm, tts, conversation |
| stt-adapter | aşama sürelerini ölçer speech_end sonrası ses tamponunu metne dış API |
|   | çevirir |
| rag llm-adapter | Sorguyu vektörleştirir, hybrid arama + Qdrant grade / subject filtresi, Top-K Bağlam + geçmiş + soru → akışlı token dış API üretimi |
| tts-adapter | Cümle bazlı akışlı ses üretimi, iptal edilebilir dış API |
| conversation | ConversationStore arayüzü; oturum ve PostgreSQL |
|   | mesaj kalıcılığı |
| rest-api | Katalog, ayarlar, sohbet listesi — cihaz ve conversation, Qdrant |
|   | panel ortak kullanır |
| panel | Canlı durum, zaman çizelgesi, sohbetler, rest-api, orchestrator olayları parametre ayarı |

## 3.2 Cihaz–Backend Olay Sözleşmesi

## Cihaz → Backend (WebSocket)

| Olay İçerik Anlam |
| --- |
| speech_start profil (grade, subject) Ekrana basıldı, kayıt başlıyor (binary) PCM ses parçaları Kayıt akışı |
| speech_end — El çekildi, işleme başla |
| cancel — Cevap çalınırken araya girildi |

## Backend → Cihaz (WebSocket)

| Olay İçerik Cihazdaki etki transcript soru metni Ekranda soru görünür thinking — Düşünme animasyonu answer_text cevap metni + kaynak ( grade , subject , Metin sayfası hazırlanır |
| --- |
| unit , breadcrumb ) |
| (binary) ses parçaları Hoparlörden çalınır (SPEAKING) done tur özeti Kaydırılabilir metin sayfasına geçilir |
| error kod + mesaj Hata ekranı, IDLE’a dönüş |

REST (cihaz ve panel ortak): GET /catalog (sınıf/ders listesi, Qdrant payload’undan türetilir), GET|PUT /settings , GET

/conversations .

## 4. Veri Modeli

## 4.1 Vektör verisi — Qdrant ( meb-books-v2 , hazır)

Hibrit koleksiyon: dense ( intfloat/multilingual-e5-large , 1024 boyut, Cosine) + sparse ( Qdrant/bm25 , IDF modifier). Embedding girdisi contextualized_text , LLM’e verilen bağlam text .

Payload alanları: unit , section , topic , subtopic , grade , subject , chunk_index , source_file , token_count ,

char_count , breadcrumb , text , contextualized_text .

- Filtreleme: grade + subject üzerinden, cihazın aktif profiline göre.

- Kaynak gösterimi: grade + subject + breadcrumb .


Not: grade ve subject için Qdrant payload index’i açılmalı (bkz. Açık Sorular).

## 4.2 İşlemsel veri — PostgreSQL

- retrieved_chunks : chunk_id, breadcrumb, grade, subject, skor.

- model_config : kullanılan STT/LLM/TTS model adları, Top-K, temperature, max tokens.

- latencies : stt_ms , retrieval_ms , llm_first_token_ms , tts_first_audio_ms , total_first_audio_ms .

- Ham ses saklanmaz.

- Kısa dönem bağlam (son 3-5 tur) bellekte tutulur; kalıcı kayıt aynı anda Postgres’e yazılır.

## 5. Teknoloji Yığını


| Katman | Seçim | Gerekçe (tek cümle) |
| --- | --- | --- |
| Donanım | ESP32-S3 N8R2 + INMP441 + MAX98357A | Tümü I2S/SPI dijital arayüz; ek ses kodeki |
|   | + ILI9341 2.4” dokunmatik | gerektirmez, PSRAM ses tamponunu taşır. |
| Firmware çatısı | ESP-IDF + LVGL | I2S ve PSRAM üzerinde tam kontrol; |
|   |   | Arduino katmanının ses/ekran kısıtlarından |
|   |   | kaçınmak. |
| Cihaz depolama | NVS | Wi-Fi bilgisi, backend adresi ve profil |
|   |   | yeniden derlemeden saklansın. |
| Backend dili/çatısı | Python 3.12 + FastAPI | Geliştiricinin en deneyimli olduğu yığın; |
|   |   | async WebSocket ve görev iptali birinci sınıf |
|   |   | desteklenir. |
| İletişim | WebSocket (oturum) + REST (durumsuz) | Panel zaten REST konuşacağı için aynı uçlar |
|   |   | paylaşılır, akış WebSocket’te kalır. |
| Vektör DB | Qdrant | Hibrit koleksiyon ve payload filtresi hazır; |
|   |   | snapshot elde. |
| İşlemsel DB | PostgreSQL | Sohbet ve ölçüm kalıcılığı; ürünleşmede |
|   |   | taşıma adımını baştan ortadan kaldırır. |
| STT | OpenAI (dosya tabanlı transkripsiyon) | Bas-konuş yapısında akışlı STT gereksiz; |
|   |   | ~5× daha ucuz. |
| LLM | OpenAI (akışlı) | Token akışı cümle bazlı TTS’i mümkün kılar. |
| TTS | OpenAI (akışlı), Türkçe testine göre | Tek sağlayıcıyla başlama hızı; adapter |
|   | değiştirilebilir | sayesinde değiştirmesi ucuz. |
| Panel | FastAPI içinden sunucu tarafı şablon + SSE İkinci derleme zinciri ve CORS ayarı kritik |   |
|   |   | yoldan zaman çalmasın. |
| Çalıştırma | Docker Compose (backend + Qdrant + | Demo dizüstünde çalışacak; TLS ve alan adı |
|   | Postgres), yerel | ihtiyacı yok. |

## 6. Mimari Kararlar (ADR)

## ADR-01 — Modüler monolit

Karar: Backend tek async süreç; modüller doğrudan fonksiyon çağrısıyla haberleşir, kuyruk yok. Bağlam: Tek geliştirici, 2 hafta, tek eşzamanlı oturum. Alternatifler: Mikroservisler; mesaj kuyruğu (RabbitMQ/Redis). Gerekçe: Tek oturumda kuyruk hiçbir throughput kazancı sağlamaz, yalnızca kurulum ve hata ayıklama yükü ekler. Sonuçları: Hızlı geliştirme ve tek yerden izleme; yatay ölçekleme için ileride oturum durumunun süreç belleğinden çıkarılması gerekir.

ADR-02 — Bas-konuş (push-to-talk) etkileşimi

Karar: Konuşma ekrana basılı tutarak başlar, el çekilince biter. Bağlam: Wake word veya sürekli dinleme, ESP32’de ek model ve CPU yükü demek. Alternatifler: Wake word; VAD ile otomatik algılama. Gerekçe: Mikrofon ve hoparlör aynı anda açık olmadığı için echo cancellation ihtiyacı ortadan kalkar. Sonuçları: Birkaç günlük iş tasarrufu ve daha az yanlış tetikleme; doğal “eller serbest” hissi kaybedilir.

ADR-03 — Uçtan uca akışlı zincir

Karar: LLM token’ları cümle sınırında bölünüp anında TTS’e verilir; üretilen ses parçaları anında cihaza akar. Bağlam: İlk sese kadar hedef ≤ 4 sn. Alternatifler: Blok blok (STT→LLM tam cevap→TTS tam ses) zincir. Gerekçe: Blok zincirde bekleme 6-10 sn’ye çıkar ve demo’da algılanan kalite çöker. Sonuçları: Belirgin şekilde daha karmaşık akış yönetimi ve hata ayıklama; buna karşılık hedefin tutması.

## ADR-04 — İptal edilebilir oturum görevi (barge-in)

Karar: Her tur iptal edilebilir bir asyncio görevi olarak yürütülür; cancel olayı LLM ve TTS akışlarını keser. Bağlam: Kullanıcının cevabı yarıda kesebilmesi istendi. Alternatifler: Cevap bitene kadar kilitli akış. Gerekçe: Doğal konuşma hissi; ayrıca uzun/yanlış cevaplarda demo’yu kurtarır. Sonuçları: Tüm sağlayıcı adapter’ları “akış durdurulabilir” sözleşmesine uymak zorunda; cihazda ses tamponunun boşaltılması gerekir.

ADR-05 — WebSocket + REST ayrımı


Karar: Oturum (ses + olaylar) WebSocket’te, katalog/ayar/sohbet listesi REST’te. Bağlam: Panel zaten REST konuşacak. Alternatifler: Her şeyi tek WebSocket üzerinden taşımak. Gerekçe: Tek WS seçilseydi aynı işlevler hem REST hem WS mesaj tipi olarak iki kez yazılırdı; REST uçları tarayıcıdan tek başına test edilebilir. Sonuçları: Firmware’de ikinci bir istemci (HTTP) yazma maliyeti (~yarım gün); buna karşılık daha kolay hata ayıklama.

## ADR-06 — Sorgu anında grade / subject filtresi

Karar: Retrieval, cihazın aktif profiline göre Qdrant payload filtresiyle daraltılır. Bağlam: ~420 kitaplık ortak havuz; payload alanları mevcut. Alternatifler: Filtresiz tüm havuzda arama; dersi LLM’in tahmin etmesi. Gerekçe: Doğruluğu en düşük maliyetle artıran karar — 3. sınıf sorusuna 12. sınıf kitabından parça gelmez. Sonuçları: Cihazda profil seçim ekranı ve backend’de katalog ucu gerekir; profil yanlış seçilirse doğru cevap bulunamaz.

## ADR-07 — PostgreSQL ile kalıcı sohbet ve ölçüm

Karar: Sohbetler ve tur ölçümleri ConversationStore arayüzü arkasında Postgres’e yazılır. Bağlam: Panelde “son sohbetler” bellekte tutulan geçmişle çelişiyordu. Alternatifler: Yalnızca canlı akış (kalıcılık yok); SQLite. Gerekçe: Panelin en görünür özelliği içi dolu olur; ürünleşmede taşıma adımı baştan ortadan kalkar. Sonuçları: Bir container daha; buna karşılık ayrı bir metrik altyapısına gerek kalmaz.

## ADR-08 — Sağlayıcılar adapter arayüzü arkasında, tek sağlayıcıyla başlangıç

Karar: STT/LLM/TTS ayrı arayüzler; başlangıçta tek sağlayıcı (OpenAI), TTS Türkçe kulak testine göre değiştirilebilir. Bağlam: Türkçe TTS kalitesi sağlayıcıdan sağlayıcıya belirgin değişiyor. Alternatifler: Baştan best-of-breed üç ayrı entegrasyon; yerel (self-hosted) modeller. Gerekçe: Tek anahtar/tek SDK ile en hızlı başlangıç; GPU olmadığı için yerel çalıştırma 4 sn hedefini karşılamaz. Sonuçları: Tek sağlayıcı kesintisi tüm zinciri durdurur; buna karşılık değiştirme maliyeti arayüz sayesinde düşük.

## ADR-09 — Cihaz yapılandırması ekrandan, NVS’te kalıcı

Karar: Wi-Fi bilgileri ve backend adresi ilk açılışta ekrandan girilir, NVS’e yazılır. Bağlam: Demo dizüstünde çalışacak; yerel IP her ağda değişir. Alternatifler: Firmware’e gömülü sabit değerler. Gerekçe: Ağ değişiminde yeniden derleme zorunluluğu ortadan kalkar — demo günü en kritik esneklik. Sonuçları: Dokunmatik klavyeyle şifre girmek yavaş ve hataya açık; ayarların bir kez girilip kalıcı olması önemli.

## ADR-10 — Yerel çalıştırma, düz ws://

Karar: Sistem demo dizüstünde Docker Compose ile çalışır; cihaz yerel ağdan bağlanır. Bağlam: Demo tek cihazla, tek mekânda yapılacak. Alternatifler: VPS + alan adı + TLS/WSS. Gerekçe: Sertifika ve WSS kurulumu ESP32 tarafında yarım-bir gün götürür, demo değerine katkısı yok. Sonuçları: Ağ bağımlılığı en büyük demo riski hâline gelir (bkz. R-01); ürünleşmede WSS zorunlu.

## 7. Fonksiyonel Olmayan Gereksinimlerin Karşılanması

| Hedef Nasıl karşılanıyor Nasıl doğrulanıyor İlk sese kadar ≤ 4 sn Akışlı zincir (ADR-03), dosya tabanlı STT, messages.latencies + panelde canlı cümle bazlı TTS zaman çizelgesi 1 eşzamanlı oturum Tek async süreç (ADR-01) Kapsam kararı |
| --- |
| Cevap doğruluğu Hybrid (dense+sparse) arama + Kaynak breadcrumb’ı ekranda ve panelde |
| grade / subject filtresi (ADR-06) + kaynak görünür gösterimi Ham ses kalıcı tutulmaz Ses yalnızca bellekte tamponlanır, STT’ye Kod incelemesi; messages şemasında ses akar, diske yazılmaz alanı yok Sağlayıcı değiştirilebilirliği STT/LLM/TTS adapter arayüzleri (ADR-08) TTS’in çalışma anında değiştirilebilmesi Gözlemlenebilirlik Yapılandırılmış log + messages kaydı + Demo sırasında canlı izleme panelde SSE zaman çizelgesi Erişilebilirlik / yedeklilik Hedef yok — demo kapsamı — |

## 8. Riskler ve Önlemler


| # Risk Etki Önlem Yedek yol R-01 Demo günü ağ: misafir Cihaz backend’e hiç Dizüstünde kendi Ayarların ekrandan Wi-Fi’de cihaz izolasyonu, bağlanamaz, demo durur hotspot’u; ikinci anında yeniden port engeli, IP değişimi bağlantıdan internet girilebilmesi (ADR-09); erişimi son çare tarayıcı test sayfası R-02 Türkçe cevap kalitesi ve Jüri algısı doğrudan düşer Erken Türkçe kulak testi; TTS adapter’ının çalışma TTS telaffuzu beklenenin system prompt’un sınıf anında değiştirilmesi; altında seviyesine göre panelden Top-K / ayarlanması temperature / model ayarı R-03 Firmware’de sıfır Cihaz susar veya Donanımı adım adım Tarayıcıdan aynı deneyimle I2S ses anlaşılmaz ses üretir doğrulama sırası: ekran → WebSocket’e bağlanan |
| --- |
| (INMP441 kaydında mikrofon kaydı → mikrofonlu test sayfası — |
| bozulma, MAX98357A hoparlör → ağ; PSRAM’de hem geliştirme hem demo |
| çıkışında gürültü, tampon ses tamponu yedeği |
| taşması) R-04 Tek sağlayıcı kesintisi Zincirin tamamı durur Adapter arayüzleri, hata İkinci sağlayıcı (ADR-08 sonucu) durumunda cihazda anahtarının hazır error ekranı bekletilmesi R-05 Kapsam kayması — panel 2 hafta tutmaz Kapsam kilitli: 4 akış + Panel özellikleri son gün veya ek özelliklerin ses salt izleme/ayar paneli kesilir, ses döngüsü tarafından zaman çalması korunur R-06 grade / subject payload Filtre yavaş çalışır İlk gün Qdrant’ta index Filtresiz arama + kaynak index’i olmaması kontrolü ve gerekirse gösterimi (doğruluk açılması düşer) |

## 9. Yol Haritası

## İlk çalışan dilim (walking skeleton) — Gün 1-4

Uçtan uca en ince dikey dilim: tarayıcı test sayfasından ses → backend → STT → Qdrant (tek sınıf/ders) → LLM → TTS → tarayıcıda ses. Cihaz yok, panel yok, akış yok (blok blok olabilir). Amaç: zincirin tamamının bir kez çalıştığını görmek ve gecikme tabanını ölçmek.

## Aşama 2 — Gün 4-7

Zinciri akışlı hâle getirmek (cümle bazlı TTS, iptal), grade / subject filtresi, Postgres kalıcılığı, messages.latencies

ölçümü, /catalog ucu.

## Aşama 3 — Gün 5-10 (paralel)

Firmware: ekran/LVGL → I2S mikrofon kaydı → I2S hoparlör → Wi-Fi/NVS kurulum ekranı → WebSocket istemcisi → sınıf/ders seçimi → durum makinesi.

## Aşama 4 — Gün 10-12

Panel: canlı durum, zaman çizelgesi (SSE), sohbet listesi, Top-K/temperature/model ayarı, tek şifre.

## Aşama 5 — Gün 12-14

Türkçe TTS kulak testi ve gerekirse sağlayıcı değişimi; barge-in doğrulaması; hotspot senaryosu provası; yedek yolların test edilmesi; demo provası.

## Demo sonrası (ürünleşme yönü)

WSS + sunucu barındırma; çok cihaz ve kullanıcı hesapları; oturum durumunun süreç belleğinden çıkarılması; KVKK hard delete ve veri yerleşimi kararının yeniden değerlendirilmesi; uzun süren işler için kuyruk; wake word.

## 10. Terimler Sözlüğü


| Terim | Tanım |
| --- | --- |
| İnce istemci (thin client) | Ağır işlemleri sunucuya bırakıp yalnızca giriş/çıkışı yöneten cihaz. |
| Modüler monolit (modular monolith) | Tek süreç olarak dağıtılan, ama içinde net modül sınırları bulunan |
|   | mimari. |
| Greenfield / brownfield | Sıfırdan geliştirme / mevcut bir sistem üzerine geliştirme. |
| Bas-konuş (push-to-talk) | Konuşmanın bir tuşa/ekrana basılı tutularak başlatılıp bırakılınca |
|   | bittiği etkileşim. |
| Yankı engelleme (echo cancellation) | Hoparlörden çıkan sesin mikrofona geri dönmesini filtreleyen işlem. |
| Araya girme (barge-in) | Sistem konuşurken kullanıcının sözü kesip yeni girdi vermesi. |
| İlk sese kadar geçen süre (time to first audio) | Kullanıcı girdiyi bitirdikten sonra ilk sesin duyulmasına kadar geçen |
|   | süre. |
| Akış desteği (streaming) | Cevabın tamamı beklenmeden parça parça üretilip iletilmesi. |
| Eşzamansız G/Ç (async I/O) | Bekleme sürelerinde işlemciyi boşa harcamadan çok işi tek süreçte |
|   | yürütme modeli. |
| Eşzamanlılık (concurrency) | Sistemin aynı anda yürütebildiği iş/oturum sayısı. |
| Olay (event) | Açık bir bağlantı üzerinden gönderilen “şu an şu oldu” mesajı. |
| Olay sözleşmesi (event contract) | Hangi olayların hangi alanlarla ve hangi sırayla aktığını tanımlayan |
|   | anlaşma. |
| Yük (payload) | Qdrant’ta her vektörün yanında tutulan, kaynak ve filtreleme bilgisi |
|   | taşıyan JSON. |
| Metadata filtreleme (metadata filtering) | Vektör aramasını belirli payload alanlarına göre daraltma. |
| Hibrit arama (hybrid search) | Anlamsal (dense) ve kelime tabanlı (sparse/BM25) aramanın birlikte |
|   | kullanılması. |
| Chunk / chunking | Uzun metnin embedding için anlamlı parçalara bölünmesi. |
| Bağlam (context) | LLM’e prompt içinde verilen, cevabı dayandıracağı kaynak metin. |
| Veri yerleşimi (data residency) | Verinin fiziksel olarak hangi ülkede işlendiği/saklandığı. |
| Kritik yol (critical path) | Projenin toplam süresini belirleyen, gecikmesi doğrudan teslimi |
|   | geciktiren iş zinciri. |
| Geri dönüş planı (fallback) | Ana yol çalışmadığında devreye giren yedek çözüm. |
| Walking skeleton | Tüm katmanlardan geçen, en ince ama uçtan uca çalışan ilk dilim. |
| NVS (Non-Volatile Storage) | ESP32’de yapılandırmanın kalıcı saklandığı flash alanı. |
| SSE (Server-Sent Events) | Sunucudan tarayıcıya tek yönlü canlı veri akışı. |
| RPO / RTO | Kabul edilebilir veri kaybı süresi / kabul edilebilir kurtarma süresi. |

## 11. Varsayımlar


| # | Varsayım | Doğrulanması gereken |
| --- | --- | --- |
| V-01 | Demo aşamasında öğrenci sesinin yurt dışı | Kurum/okul onayı |
|   | sağlayıcıya gitmesi kabul edilebilir; |   |
|   | ürünleşmede yeniden değerlendirilecek |   |
| V-02 | Tek eşzamanlı oturum yeterli; oturum | Demo senaryosu |
|   | durumu süreç belleğinde tutulabilir |   |
| V-03 | Tüm kitapların chunk metadata’sı örnek | Qdrant’ta rastgele örnek kontrolü |
|   | dosyayla aynı şemada ( grade , | subject , |
|   | unit , breadcrumb ) |   |
| V-04 | Sağlayıcı maliyeti demo ölçeğinde ihmal | Güncel fiyat listesi kontrolü |
|   | edilebilir (~$0,01-0,02/tur) |   |
| V-05 | ESP32-S3 N8R2’nin 2 MB PSRAM’i ses | İlk ses testlerinde ölçüm |
|   | tamponları + LVGL çerçeve tamponu için |   |
|   | yeterli |   |
| V-06 | ILI9341 SPI hattı ile iki I2S hattı aynı kartta | Kartın pin haritası ile doğrulama |
|   | pin çakışması olmadan kullanılabilir |   |

## 12. Açık Sorular

- 1. Qdrant’ta grade ve subject için payload index’i açık mı? Değilse ilk gün açılmalı.

- 2. grade alanı "3.Sınıf" biçiminde saklanıyor — cihaz ekranındaki seçimle bire bir eşleşme için normalizasyon gerekiyor mu?

- 3. Türkçe TTS için hangi sağlayıcı seçilecek? (Kulak testi sonucuna bağlı; ADR-08’in açık ucu.)

- 4. LLM olarak hangi model ve hangi system prompt kullanılacak — sınıf seviyesine göre dil sadeleştirmesi yapılacak mı?

- 5. Ses formatı ve örnekleme hızı (PCM 16 kHz mono varsayımı) hem INMP441 hem STT sağlayıcısı için doğrulanmalı.

- 6. Cevap uzunluğu sınırı ne olacak? (Uzun cevap hem TTS maliyetini hem ekran metnini büyütür.)

- 7. Panelin tek şifresi nasıl saklanacak (ortam değişkeni varsayımı)?

- 8. ILI9341, INMP441 ve MAX98357A için kesin GPIO pin ataması nedir — kart üzerinde ayrılmış/kullanılamaz pinler hangileri?

- 9. Dokunmatik denetleyici hangi yonga (XPT2046 vb.) ve kalibrasyon gerekiyor mu?

- 10. MAX98357A’nın kazanç (GAIN) ve SD pini nasıl bağlanacak — konuşma dışında hoparlör susturulacak mı?
