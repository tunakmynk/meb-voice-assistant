# Uygulama Planı — Sesli AI Asistan

Mimari doküman kararları veriyor, bu dosya **sırayı** veriyor. Her satır bir
"derin-öğren" parçası: tek başına anlamlı, yazılıp çalıştırılabilir bir artım.

Durum işaretleri: `[x]` bitti · `[ ]` bekliyor · `[~]` kısmen

---

## Bitmiş temel

- [x] `docker-compose.yml` — Qdrant + Postgres (ADR-10)
- [x] `scripts/ingest_qdrant.py` — parquet → Qdrant, payload index dahil
- [x] `app/config.py` — tek yapılandırma kaynağı
- [x] `app/rag/retriever.py` — hibrit arama (dense+sparse, RRF, grade/subject filtresi, ADR-06)
- [x] `app/adapters/stt.py` — SttAdapter sözleşmesi + PCM→WAV + OpenAI implementasyonu (ADR-08)
- [x] `tests/test_stt.py` — WAV başlığı config ile eşleşiyor

---

## Aşama 0 — Zemin kontrolü (yarım gün, her şeyin ön koşulu)

Bunlar 5 dakikalık kontroller ama yanlış çıkarlarsa sonraki her adımı boşa
düşürürler. Açık Soru 1, 2 ve 3 burada kapanır.

- [x] **0.1** Docker Desktop + `docker compose up -d` — qdrant ve postgres healthy
- [x] **0.2** Koleksiyon `meb-books-v2`: **8577 nokta**, dense 1024 Cosine, sparse IDF — `retriever.py`'nin beklediğiyle birebir
- [x] **0.3** `grade` ve `subject` payload index'i **açık** (keyword, 8577 nokta) → **Açık Soru 1 kapandı**, R-06 gerçekleşmedi
- [x] **0.4** Gerçek değerler listelendi → **Açık Soru 2 kapandı** (bulgular aşağıda)
- [x] **0.5** `scripts/smoke_retrieval.py` — 6 senaryo, hepsi geçiyor (bulgu aşağıda)

### 0.4'ün bulguları — katalog tasarımını belirliyor

**`grade` biçimi:** `"12.Sınıf"` — nokta var, **boşluk yok**. Cihaz bu dizeyi
birebir üretmek zorunda; `"12. Sınıf"` sessizce sıfır sonuç döndürür.

**`subject` sınıfa bağlı, düz liste değil.** Ders adları sınıftan sınıfa
değişebiliyor ve her sınıfta her ders yok:

| Sınıf | Ders sayısı | Dikkat |
|---|---|---|
| 1-2 | 2 | yalnızca Hayat Bilgisi/Matematik, Matematik/Türkçe |
| 7 | 3 | **Matematik yok** |
| 8 | 6 | `T.C. İnkılap Tarihi ve Atatürkçülük` |
| 10 | 10 | en zengin sınıf |
| 12 | 7 | `İnkılap Tarihi ve Atatürkçülük` — **"T.C." öneki yok** |

İnkılap dersinin iki farklı yazımı aynı dersin 8. ve 12. sınıf kitaplarından
geliyor. Düz bir ders listesi gösterirsek 8. sınıf öğrencisi 12. sınıfın
dizesini seçebilir ve **filtre hata vermeden boş döner** — ADR-06'nın en
sinsi başarısızlık biçimi.

**Kararlar:**
- `GET /catalog` düz iki liste değil, **grade → subject[] eşlemesi** döndürür.
- Cihaz ekranında önce sınıf seçilir, ders listesi ona göre daralır.
- Normalizasyon **yapılmaz**; payload'daki dize tek doğru kaynaktır.

### 0.5'in bulgusu — "kaynak bulunamadı" skorla tespit edilemez

`scripts/smoke_retrieval.py` altı senaryoyu koşuyor: filtrenin gerçekten
uygulandığı (aynı soru iki sınıfta farklı chunk döndürüyor **ve** dönen her
chunk'ın payload `grade`'i isteneni tutuyor), BM25 kolunun özel isimde
(`Lozan`) çalıştığı, dense kolunun kelime örtüşmesi olmadan konuyu bulduğu,
var olmayan sınıf/ders çiftinin çökmeden boş döndüğü ve yanlış yazılmış
`grade` dizesinin sessizce boşluk ürettiği.

**Asıl bulgu 6. senaryoda:** cevabı o kitapta bulunmayan bir soru sorulduğunda
retrieval yine de en yakın K chunk'ı döndürüyor. Ölçülen ham cosine skorları:

| Durum | Ham cosine |
|---|---|
| Kapsam içi (8.Sınıf, konu var) | 0.826 |
| **Kapsam dışı** (6.Sınıf, konu kitapta yok) | **0.830** |
| Tamamen alakasız ("ofsayt kuralı" Fen kitabında) | 0.775 |

Kapsam dışı soru, kapsam içi sorudan **daha yüksek** skor aldı. Yani:

- **RRF skoru** sıra tabanlıdır; tepe sonuç alakasız olsa da 1.0'a çıkar.
- **Ham cosine** e5 modelinde 0.77-0.83 gibi dar bir bantta yaşar, alakayı
  ölçmez.
- Dolayısıyla `score_threshold` ile "kaynak bulunamadı" **tespit edilemez**.

**Karar:** "bağlamda cevap yok" kararını LLM verir. `1.1`'deki system prompt
açık bir kural taşımak zorunda: *verilen bağlamda cevap yoksa uydurma,
"bunu kitabında bulamadım" de.* Bu, doküman 7. bölümdeki "cevap doğruluğu"
hedefinin tek gerçek savunma hattı.

## Aşama 1 — Walking skeleton (Gün 1-4): zincir bir kez baştan sona çalışsın

Amaç doğru cevap değil, **çalışan zincir** ve ölçülmüş gecikme tabanı.
Akış şart değil, blok blok olabilir.

- [x] **1.1** `app/adapters/llm.py` — LlmAdapter sözleşmesi + OpenAI akışlı üretim + sınıf seviyesine göre system prompt (Açık Soru 4)
- [x] **1.2** `app/adapters/tts.py` — TtsAdapter sözleşmesi + OpenAI akışlı ses üretimi
- [x] **1.1b** `scripts/eval_no_source.py` — KAYNAK_YOK kuralının **iki yönlü** ölçümü — 12/12 (bulgu aşağıda)
- [x] **1.3** `app/orchestrator/pipeline.py` — turu yürüten akış: STT → retrieval → LLM → TTS, aşama sürelerini ölç
- [x] **1.4** `app/main.py` — FastAPI uygulaması + lifespan'de retriever warmup + API anahtarı kontrolü
- [x] **1.5** `app/gateway/ws.py` — WebSocket olay sözleşmesi (doküman 3.2), ses parçalarının aktarımı
- [x] **1.6** `app/static/test.html` — tarayıcı test sayfası: bas-konuş, mikrofon kaydı → WS → gelen sesi çal (R-03'ün yedek yolu)
- [x] **1.7** Uçtan uca ilk tur + gecikme tabanını yaz: STT / retrieval / LLM ilk token / TTS ilk ses

### 1.1b'nin bulgusu — prompt'taki örnek bilgi kaynağına dönüşüyor

Eval iki yönlü ölçüyor çünkü tek yön yanıltıcı: "her şeye KAYNAK_YOK de" diyen
bir prompt kapsam dışı metriğini %100 yapar, asistanı ise işe yaramaz hale
getirir. İlk ölçüm bunu doğruladı:

| Sürüm | Kapsam içi | Kapsam dışı |
|---|---|---|
| İlk prompt | 4/6 — **2 aşırı reddetme** | 6/6 |
| Kural 2 gevşetildi | 6/6 | 5/6 — **1 uydurma** |
| Örnek kaldırıldı | **6/6** | **6/6** |

**Aşırı reddetmenin sebebi** retrieval değildi: kitap *ilkeyi* anlatıyor
(“sıcaklık taneciklerin ortalama hareket enerjisidir”), öğrenci ise *günlük
örnekle* soruyor (“camlar neden terler”). Model "kaynak yetmiyor" diye okudu.
Kural 2'ye "ilkeyi öğrencinin örneğine uygulaman beklenir" eklendi.

**Ama gevşetme yeni bir hata doğurdu ve sebebi prompt'un kendisiydi.** Kural 2
örnek olarak *"Kitap 'sıcak hava soğuk yüzeye değince yoğuşur' diyorsa..."*
cümlesini taşıyordu. Model 6. sınıf sorusunda — kaynaklar genleşme, yoğunluk
ve **mağaza güvenlik aynası** olmasına rağmen — bu örneği bilgi kaynağı olarak
kullanıp doğru ama dayanaksız bir cevap üretti; hatta "yoğuşma ilkesine
dayanır" diye prompt'u neredeyse birebir tekrarladı.

**Ders:** system prompt'taki örnekler modelin bağlamının parçasıdır ve kaynak
metin gibi kullanılabilir. Somut örnek kaldırılıp yerine "uygulayacağın ilke
kaynak metinlerde yazıyor olmalı; bu kuralların içinde geçen hiçbir örneği
bilgi kaynağı olarak kullanma" kuralı kondu.

Son durum **12/12**. En keskin kanıt: *aynı soru* 8. sınıfta gerekçeli cevap,
6. sınıfta `KAYNAK_YOK` alıyor. (temperature 0.2 olduğu için tek koşu %100
kanıtı değil, ama eğilim gürültü değil.)

### 1.3'ün bulgusu — işaret tespiti akışı geciktirmiyor

`KAYNAK_YOK` işaretini görmek için ilk satırın bitmesini beklemek gerekmiyor;
akışın işaret **önekinden sapmasını** beklemek yeterli. Cevap "Isı bir
enerji..." diye başlıyorsa daha ilk parçada önek tutmuyor ve akış anında
serbest bırakılıyor — maliyet bir cümle değil, bir token. Tampon yalnızca
metin hâlâ işaret olabilecekken doluyor; öyle çıkarsa da seslendirilecek bir
cevap zaten yok. Yani ADR-03 ile çelişmiyor.

### 1.7'nin bulgusu — gecikme tabanı: 11.0 sn (hedef 4.0)

`scripts/e2e_turn.py` gerçek WebSocket üzerinden tam bir tur koşuyor. Mikrofon
olmadığı için soru TTS ile sese çevrilip cihaz kaydetmiş gibi geri besleniyor;
böylece baytlar gerçek bir söylemin yolunu izliyor ve STT gerçekten Türkçe
tanımak zorunda kalıyor.

İlk tur **çalıştı**: soru doğru yazıya çevrildi, cevap doğru breadcrumb ile
gerekçelendi (`5. BÖLÜM: MADDENİN ISI İLE ETKİLEŞİMİ`).

| Aşama | Süre |
|---|---|
| STT | 1754 ms |
| Retrieval | **50 ms** |
| LLM ilk token | 1073 ms |
| LLM kalanı | 2084 ms |
| **TTS (cevabın tamamı tek blok)** | **6034 ms** |
| **İlk sese kadar** | **11020 ms** |

**Retrieval sorun değil** (50 ms, bütçenin %1'i). Darboğaz TTS: cevabın tamamı
üretilip tek parça seslendiriliyor, tek bayt bile çıkmadan 6 saniye geçiyor.
ADR-03'ün cümle bazlı akışı tam olarak bunun için var — 2.1 bu sayıyı
düşürmenin en yüksek getirili adımı.

## Aşama 2 — Akışlı zincir ve kalıcılık (Gün 4-7)

- [x] **2.1** Cümle bölücü — LLM token akışını cümle sınırında bölüp anında TTS'e ver (ADR-03)
- [x] **2.2** İptal edilebilir tur görevi — `cancel` olayı LLM ve TTS akışlarını kesiyor (ADR-04, barge-in)
- [x] **2.3** `backend/db/init/001_schema.sql` — conversations + messages şeması, `retrieved_chunks` / `model_config` / `latencies` alanları (4.2)
- [x] **2.4** `app/conversation/store.py` — ConversationStore arayüzü + asyncpg implementasyonu (ADR-07)
- [x] **2.5** Ölçümlerin her turda Postgres'e yazılması + son 3-5 turun bellekte bağlam olarak taşınması
- [x] **2.6** `app/api/routes.py` — `GET /catalog` (Qdrant payload'undan türetilir), `GET|PUT /settings`, `GET /conversations` (ADR-05)
- [x] **2.7** İlk sese kadar süreyi ölç: 4 sn hedefi tutuyor mu (bulgu aşağıda)

### 2.1 + 2.7'nin bulgusu — 11.0 sn'den 2.7 sn'ye

Üç ayrı değişiklik, her biri ölçülerek:

| Değişiklik | İlk sese kadar |
|---|---|
| Taban (1.7) | 11020 ms |
| Cümle bazlı TTS (2.1) | 4245 ms |
| `top_k` 5 → 3 | 4144 ms |
| **Akışlı PCM TTS** | **2697-3777 ms** |

**1. Cümle bölücü.** LLM token akışı cümle sınırında bölünüp her cümle ayrı
seslendiriliyor. Türkçe'de `split(".")` yanlış: `1923.`, `8.`, `T.C.`, `vb.`
hepsi nokta taşıyor ama cümle bitirmiyor — ve `T.C.` korpusta bir ders adının
parçası. Bölücü kısaltma/sayı ayrımı yapıyor, testleri gerçek cevaplardan
alınmış cümlelerle yazıldı.

**2. `top_k` 5 → 3.** Bağlam ~3180 token'dan ~1897'ye indi, LLM ilk token
1009 → 781 ms. Eval **12/12 kaldı**, yani ölçülebilir bir kalite bedeli yok.
`config.py` varsayılanı değiştirildi.

**3. Akışlı PCM TTS — en büyük kazanç.** Ölçüldü:

| TTS biçimi | İlk bayt |
|---|---|
| Tam dosya (mp3/pcm/wav farketmiyor) | ~1700 ms |
| Akışlı mp3 | 1282 ms |
| **Akışlı PCM** | **683 ms** |

PCM'de kare tamponlama yok, örnekler üretildikçe çıkıyor. Ek fayda: **cihazda
mp3 çözücü gerekmiyor**, örnekler doğrudan I2S'e verilebiliyor (Aşama 3.4'ü
sadeleştirir). Bedeli ~3x yük, yerel ağda önemsiz. Tarayıcı test sayfası da
Web Audio ile parçaları sıraya dizecek şekilde güncellendi.

### Dürüst uyarı — hedef tutuyor ama garanti değil

Dört ardışık koşu: **2697, 5592, 2717, 3777 ms**. Üçü geçti, biri aşıldı.
Aşan koşuda tek sebep LLM ilk token'ının 3378 ms'ye sıçramasıydı (normalde
600-1000 ms) — yani bizim kodumuz değil, sağlayıcı gecikmesi. Kalan tampon
dar; demo gününde tek yavaş cevap hedefi deler.

Elimizdeki tek büyük kaldıraç STT (~1100 ms): ses zaten kayıt sırasında
akıyor, dolayısıyla akışlı transkripsiyon `speech_end` anında biterdi. Bu
yapılırsa ~1 sn daha tampon kazanılır. R-02'nin altına yazıldı.

### Manuel testte çıkan iki hata (2026-08-21)

Tarayıcı testinde bulundu, ikisi de düzeltildi ve teste bağlandı.

**1. Takip soruları çalışmıyordu.** "Isı ve sıcaklık farkı nedir" sorusundan
sonra "konuyla ilgili birkaç örnek verebilir misin" denince asistan
`KAYNAK_YOK` diyordu.

Sebep beklenenden farklı çıktı: geçmiş **LLM'e gidiyordu ama retrieval'a
gitmiyordu**. Arama yalnızca güncel soruyu görüyor, o soruda da hiç içerik
kelimesi yok:

| Arama sorgusu | Dönen bölümler |
|---|---|
| yalnız takip sorusu | Solunum (0.500), Fiziksel Değişimler (0.500) |
| önceki soru + takip sorusu | **Isı ile Etkileşimi 5.1 (0.750)**, 5.3, 5.2 |

Yani model doğru davranıyordu — eline alakasız kaynak verilmişti.

**Önce iki ucuz yol denendi, ikisi de ölçümle elendi:**

*Her zaman önceki soruyu eklemek.* Gerçek konu değişimini yok ediyor.
Geçmişte ısı/sıcaklık varken "basınç birimi nedir" araması **üç ısı chunk'ı ve
sıfır basınç chunk'ı** döndürdü — sessiz yanlış cevap, elimizdeki en kötü
başarısızlık biçimi.

*Kelime listesi sezgiseli* (`bu/şu/o` ailesi, `peki`, `ayrıca`). Manuel testte
"Konuyla ilgili birkaç örnek verebilir misin?" sorusunu kaçırdı; "az önce
anlattığın", "aynı konuda", "söylediğin şey" gibi biçimleri de kaçırırdı.
Türkçe geriye atıfta sınırsız; liste asla tamamlanmaz.

**Karar: kararı zaten sohbeti okuyan modele bırak.** `rewrite_query`, atıfı
çözüp soruyu tek başına anlaşılır hâle getiriyor; konu değiştiyse soruya
dokunmuyor. Ölçülen davranış:

| Soru | Sonuç |
|---|---|
| "Konuyla ilgili birkaç örnek verebilir misin?" | "Isı ve sıcaklık arasındaki farkla ilgili..." |
| "az önce anlattığın şeyi biraz daha basit anlatır mısın" | "Isı ve sıcaklık arasındaki farkı..." |
| "basınç birimi nedir" | değişmedi |
| "hücre zarının görevi nedir" | değişmedi |

**Bedeli ve neden kabul edildi:** çağrı yalnızca **geçmiş varsa** yapılıyor,
oturumun ilk sorusu etkilenmiyor. Üç turluk ölçüm:

| Tur | retrieval aşaması | yeniden yazma | ilk ses |
|---|---|---|---|
| 1 (geçmiş yok) | 48 ms | — | 3058 ms |
| 2 (takip) | 883 ms | ~835 ms | 3076 ms |
| 3 (konu değişimi) | 772 ms | ~724 ms | 3012 ms |

Üçü de 4 sn hedefinin altında; ek maliyet soğuruldu. Çağrı hata verirse ham
soruya düşülüyor — demo ortasında tur düşmesin diye.

**2. Dilbilgisi hatası.** Kapsam dışı cevabı "Bu konuyu kitabında
bulamadığını söylemek isterim" çıkıyordu — yanlış şahıs.

Sebep yine prompt'un kendisiydi: kural *"öğrenciye bu konuyu kitabında
**bulamadığını** söyle"* diyordu; bu ifade "senin bulamadığını" diye de
okunuyor ve model bunu kopyaladı. Prompt artık söylenecek cümleyi birebir
veriyor: *"İKİNCİ SATIRA da tam olarak şunu yaz: Bu konuyu kitabında
bulamadım."* Üç ayrı kapsam dışı soruda üçü de birebir doğru çıktı.

Eval her iki düzeltmeden sonra **12/12** kaldı.

### Takip sorusu hatası sürdü — asıl sebep prompt'tu

Yeniden yazma eklendikten sonra bile "Bununla ilgili daha farklı örnekler
verebilir misin?" `KAYNAK_YOK` alıyordu. Boru hattının her aşaması ölçüldü ve
**hepsi doğruydu**: yeniden yazma "Isı ve sıcaklık arasındaki farkla ilgili
daha farklı örnekler verebilir misin?" üretiyor, retrieval doğru ISI
chunk'larını (0.643, 0.533) getiriyordu. Reddeden şey modeldi.

**Sebep 1 — kurallar çelişiyordu.** Kural 4 "kaynaklar sorulanı açıklamıyorsa
reddet" diyordu; kitapta "farklı örnekler" listesi bulunmadığı için model
reddediyordu. Kural 3 ise "ilkeyi uygula" diyordu. Çözüm kuralları **kapsama
göre ayırmak** oldu: kural 3 artık yalnızca *bu sohbette daha önce
cevaplanmış* konular için geçerli (örnek çoğaltma, basitleştirme, özetleme),
kural 4 ise yeni sorular için sıkı ölçüyü koruyor.

**Sebep 2 — asıl bulgu: eval yanlış girdi dağılımını test ediyordu.**

Kural düzeltildikten sonra bu kez oturumun **ilk** sorusu reddedilmeye
başladı. Değişkenlik değildi; 5/5 tekrarlanıyordu. İzole edildi:

| Soru biçimi | Chunk kümesi A | Chunk kümesi B |
|---|---|---|
| "**I**sı ve sıcaklık… nedir?" | 3/3 RED | 3/3 RED |
| "**ı**sı ve sıcaklık… nedir" | 0/3 RED | 0/3 RED |

Chunk'lar aynıyken sonucu belirleyen şey **sorunun ilk harfiydi**. Model
yüzey biçimine bakıp konuyu kaynaklarda "göremiyordu".

Kritik nokta: **STT her cümleyi büyük harfle başlatır.** Yani cihazdan gelen
her gerçek soru bozuk biçimde geliyordu — ama eval küçük harfli sabit dizeler
kullandığı için 12/12 veriyor ve hatayı hiç görmüyordu. Eval, ürünün gerçek
girdisini test etmiyordu.

İki düzeltme:
- `eval_no_source.py` artık `as_stt_would()` ile STT çıktısını taklit ediyor
  (ilk harf büyük + soru işareti). Gerçekçi girdiyle ilk ölçüm 5/6'ya düştü —
  gizli hata görünür oldu.
- Kural 4'e eklendi: kararı **anlama** göre ver, "Isı" ile "ısı" aynı
  kelimedir; şüphedeysen reddetme, cevap ver.

Sonuç: gerçekçi girdiyle **12/12**, büyük harfli soru 0/5 red. Canlı
WebSocket'te dört turluk sohbet — düz soru, iki takip sorusu ve konu
değişimi — dördü de doğru.

**Ders:** eval'in girdisi ürünün gerçek girdisiyle aynı biçimde olmalı; aksi
hâlde yeşil bir metrik hatayı gizler.

### Açık kalan gözlem — örnek üretirken sayı uyduruyor

3 turluk testte 2. turun cevabında "kaynayan suyun sıcaklığı yüz seksen derece
Celsius'tur" geçti. Kaynakta böyle bir sayı yok ve fizik olarak da yanlış
(100 °C). `KAYNAK_YOK` kuralı **konu** kapsamda olmadığında çalışıyor; ama
konu kapsamdayken model örnek uydururken **sayı** uydurabiliyor. Prompt'a kural 5 olarak
"kaynaklarda geçmeyen sayı, tarih, oran ya da ölçü verme; gerekirse niteliksel
konuş" eklendi. Sonraki koşularda kaynayan su doğru şekilde "yüz derece"
olarak geçti, ama bu tek gözlem; Aşama 5.1 kulak testinde tekrar bakılacak.

### 2.2 — Barge-in: iki cevap üst üste çalıyordu

Manuel testte bulundu: cevap seslendirilirken yeni soru sorulunca model iki
soruyu birden cevaplıyor ve **sesler birbirine karışıyordu**.

Üç ayrı kusur aynı anda buna sebep oluyordu; üçü de düzeltildi:

1. **`speech_start` çalışan turu iptal etmiyordu.** Düğmeye basmak zaten
   araya girmektir; ayrıca `cancel` beklemek eski turu akar hâlde bırakıyor.
2. **`speech_end` `session.task`'ı üzerine yazıyordu.** Eski görev
   iptal edilmediği için çalışmaya ve **aynı sokete yazmaya** devam ediyordu.
   Karışmanın asıl sebebi buydu.
3. **`cancel` görevi `await` etmiyordu.** `Task.cancel()` yalnızca *istek*tir;
   görev bir sonraki `await` noktasına kadar çalışır ve o sırada hâlâ ses
   paketi gönderebilir. Beklemeden yeni tur başlatmak tam olarak iki cevabın
   iç içe geçmesi demektir.

Ayrıca cihaza yeni bir `cancelled` olayı gönderiliyor: iptalden **önce**
yollanmış baytlar hâlâ ağda olabilir, cihazın kendi tamponunu boşaltması
gerekir. Tarayıcı sayfasında `stopPlayback()` zamanlanmış tüm `BufferSource`
düğümlerini durduruyor — yalnızca `playAt`'i sıfırlamak yetmiyor, çünkü
parçalar `start()` ile Web Audio kuyruğuna çoktan girmiş oluyor.

**Testler gerçekten hatayı yakalıyor mu diye doğrulandı:** düzeltme geçici
olarak geri alındığında `tests/test_ws_bargein.py` içindeki iki test düştü
(eski turun ses paketleri akmaya devam ediyor, `cancelled` hiç gelmiyor).
Düzeltme geri konunca 29 testin tamamı geçiyor.

### 2.3-2.5 — Kalıcılık ve iki ders

Şema (`conversations` + `messages`) hem `docker-entrypoint-initdb.d` ile hem
de **her açılışta uygulama tarafından** uygulanıyor. İkincisi şart: init
dizini yalnızca volume boşken çalışır, yani şema sonradan eklendiğinde mevcut
kurulumlar onu hiç görmezdi.

Doğrulandı: 4 turluk sohbet kaynakları, aşama süreleri ve **o turdaki**
ayarlarla kaydedildi (panel çalışma anında `top_k` değiştirebildiği için
yapılandırmayı sonradan okumak geçmişi yanlış gösterirdi). Toplam 48 kB —
ham ses saklanmıyor (doküman 4.2).

**Ders 1 — yazma anı yarışa girdi.** İlk deneme **hiçbir şey kaydetmedi**:
yazma `done`'dan sonra yapılıyordu, istemci ise `done`'ı görür görmez sonraki
soruyu gönderiyor; `speech_start` görevi tam insert sırasında iptal ediyordu.
Soketin kapanması da aynı etkiyi yapıyordu. Dört tur çalıştı, sıfır satır
yazıldı. Çözüm görev ömrüyle uğraşmak değil, **`done` gönderilmeden önce
yazmak** oldu — yarış yönetilmiyor, ortadan kaldırılıyor. Ses zaten
gönderilmiş oluyor, birkaç ms öğrenciye hiçbir şeye mal olmuyor.

**Ders 2 — testler yanlış sebepten geçebiliyor.** "Bozuk veritabanı turu
düşürmez" testi ilk hâlinde düzeltme kaldırıldığında da geçiyordu: hata
`done`'dan **sonra** geldiği için, `done`'da duran drain onu hiç görmüyordu.
Aynı şekilde yavaş yazma testi olmadan yarış hiç görünmüyordu — bellek içi
store anında döndüğü için. İki test de düzeltme geri alınarak doğrulandı:
biri 0/2 tur, diğeri hata sızdırma ile düşüyor.

### Prompt'un gevşetilmesi (kullanıcı düzenlemesi) ve yan etkisi

Prompt elle gevşetildi; gerekçe yerindeydi: kurallar fazla katıydı ve model
kitapta geçmeyen günlük örnekleri kuramıyordu.

Ama düzenleme iki kuralı da götürdü ve bunlar **cevaplamayı artıran**
kurallardı, yani niyetin tersine çalıştı:

- Kural 3 (örnek çoğaltma/basitleştirme yeni konu değildir) tamamen silinmiş.
- Kural 4'ten *"kararı anlama göre ver, 'Isı' ile 'ısı' aynı kelimedir,
  şüphedeysen cevap ver"* cümlesi çıkmış.

Ölçülen etki: `Isı ve sıcaklık arasındaki fark nedir?` **8/8 reddedildi**,
eval kapsam içi 5/6'ya düştü. Retrieval değişmemişti.

Kurallar, kullanıcının niyetini koruyacak şekilde geri kondu ve ayrım
netleştirildi: **BİLGİ kaynaklardan gelir, ÖRNEK serbesttir.** Kural 2 artık
açıkça "örneğin kitapta geçmesi gerekmez, uyman gereken tek şey örneğin
dayandığı ilkenin kaynaklarda yazıyor olması" diyor.

Sonuç: büyük harfli soru **0/8 red**, eval **12/12**, örnek isteyen dört
sorunun dördü de cevaplanıyor (çaydanlık örneği kitapta geçmiyor).

## Aşama 3 — Firmware (Gün 5-10, paralel iş kolu)

Backend Aşama 2'de dururken buraya geçilebilir. Sıra R-03'ün önerdiği
doğrulama sırası: **ekran → mikrofon → hoparlör → ağ**.

- [x] **3.1** GPIO pin haritasını sabitle (Açık Soru 8, 9, 10 — V-06) → `firmware/main/pins.h`
### 3.1'in kararları — Açık Soru 8, 9, 10 kapandı

**Açık Soru 8 (pin ataması).** `firmware/main/pins.h` sabitlendi. Kaçınılan
pinler ve gerekçeleri dosyanın başında yazılı; atama yalnızca GPIO 4-18, 21,
47, 48 aralığını kullanıyor.

| Kullanılamaz | Sebep |
|---|---|
| 26-32 | Dahili SPI flash — kullanılırsa kart açılmaz |
| 19, 20 | Yerleşik USB (D-/D+) |
| 43, 44 | UART0 seri konsol |
| 0, 3, 45, 46 | Strapping pinleri |

**33-37 bu kartta serbest**, çünkü N8R2 quad PSRAM kullanıyor. Octal PSRAM'li
bir modele (N8R8) geçilirse bu pinler PSRAM'e gider — kart değişirse ilk
bakılacak yer burası.

Ekran ve dokunmatik **aynı SPI veri yolunu** paylaşıyor, ayrı CS ile. İkisi
aynı anda konuşmadığı için ikinci bir SPI çevre birimi harcamaya gerek yok.
Mikrofon ve hoparlör ise **iki ayrı I2S kanalı** (doküman 1. bölüm); tek
kanalı yön değiştirerek kullanmak her bas-konuş geçişinde sürücüyü yeniden
kurmayı gerektirirdi.

**Açık Soru 9 (dokunmatik).** XPT2046 — 2.4" ILI9341 modüllerinin standardı.
**Kalibrasyon gerekiyor:** direnç tabanlı panelde ham ADC değerleri panelden
panele kayar. Dört köşe ölçülüp ölçek/kaydırma NVS'e yazılacak (ADR-09 zaten
NVS kullanıyor). Kalibrasyonsuz sapma, sınıf/ders listesinde yanlış satır
seçilmesi demek.

**Açık Soru 10 (MAX98357A SD ve GAIN).**

`SD` sabit VDD'ye değil, **GPIO 17'ye** bağlanıyor. Sebep: SD bu yongada tek
başına aç/kapa değil, aynı zamanda kanal seçici (>1.4 V = sol kanal, <0.16 V =
kapalı). GPIO'dan HIGH sürmek mono çıkışı veriyor, LOW sürmek amfiyi tamamen
kapatıyor. ADR-02 gereği kayıt ve çalma aynı anda olmadığı için konuşma dışında
amfi kapatılabiliyor — Class-D'nin boştaki cızırtısı sessiz bir sınıfta
duyulur. Ayrıca barge-in'de (ADR-04) hoparlörü anında susturmanın en kesin
yolu bu.

`GAIN` **boşta bırakılıyor (9 dB)**. Tek küçük hoparlör için yeterli; daha
yüksek kazanç, TTS'in zaten normalize çıkışında kırpılma yapar. Ses zayıf
kalırsa önce yazılımda ölçekleme, sonra GND (12 dB).

**Bonus:** akışlı PCM kararı (plan 2.1) sayesinde cihazda **MP3 çözücü
gerekmiyor** — baytlar doğrudan I2S'e yazılıyor. 3.4'ü belirgin şekilde
sadeleştiriyor.

- [x] **3.2** Ekran + LVGL + Türkçe font — **gerçek donanımda doğrulandı**
### 3.2 — ESP-IDF iskeleti ve Türkçe font

Araç zinciri **ESP-IDF** seçildi (LVGL, iki ayrı I2S kanalı, PSRAM tahsisi ve
NVS için birinci sınıf destek).

`sdkconfig.defaults` iki kritik satır taşıyor:
- `CONFIG_SPIRAM_MODE_QUAD=y` — N8R2 quad PSRAM kullanır. Octal seçilirse
  **kart açılmaz.**
- `CONFIG_BT_ENABLED=n` — doküman 1. bölüm gereği Bluetooth kapalı.

LVGL tamponu ekranın 1/8'i (`320*30`), çift tamponlu ve DMA'lı. Tam çerçeve
tamponu 150 KB eder ve V-05'in uyardığı bellek baskısını doğrudan üretirdi.

**Türkçe font — `lv_font_conv` yerine `lv_tiny_ttf`.** İki gerekçe:

1. Bu makinede node/npm yok; standart dönüştürücü derleme hattının parçası
   olamazdı.
2. `lv_font_conv` karakter aralığını önceden sabitlemeyi gerektirir. "ASCII +
   çğıİöşü" yeterli görünür, ama breadcrumb'larda değişik tırnak/tire
   biçimleri de var; aralık eksik kalırsa ekranda **sessizce boş kutu** çıkar.

`lv_tiny_ttf` TTF'yi ikiliye gömüp glifleri çalışma anında çiziyor — aralık
sabitleme yok, node bağımlılığı yok. Bedeli CPU ve önbellek RAM'i, 320x240'ta
kabul edilebilir. Font olarak **DejaVuSans** seçildi: serbest lisanslı (Arial
ve Segoe UI hazır duruyor ama ikisi de lisanslı, gömülü üründe telif sorunu)
ve noktasız `ı` / noktalı `İ` dahil Türkçe'nin tamamını taşıyor.

`main.c` şu an bir doğrulama ekranı basıyor: `ÇĞİÖŞÜçğıöşü` satırı ve boştaki
dahili/PSRAM yığın miktarı. İkincisi 3.3'te I2S tamponu eklendiğinde kıyas
noktası olacak (V-05).

**Font yerinde.** `main/fonts/DejaVuSans.ttf` (347 KB, TTF imzası doğrulandı).
Not: DejaVu arşivinin tamamı açılırsa 21 font + belge (8.2 MB) ESP-IDF bileşen
dizinine düşüyor; yalnızca `DejaVuSans.ttf` gerekiyor ve `main/` içine değil
`main/fonts/` içine konmalı.

**Durum: derleniyor.** ESP-IDF v5.3.2 kuruldu, `idf.py build` hatasız geçiyor.
İkili 945 KB (8 MB flash'ın %11'i), font sembolü (`_binary_DejaVuSans_ttf_start`)
bağlandı, LVGL 9.5.0 + ILI9341 1.2.0 bileşenleri çekildi.
**Kartta çalışıyor.** COM3 (CH343 USB-UART) üzerinden yüklendi, açılış log'u temiz:

```
SPI Flash Size : 8MB
Found 2MB PSRAM device / SPI SRAM memory test OK
Adding pool of 2048K of PSRAM memory to heap allocator
ili9341: LCD panel create success, version: 1.2.0
display: Ekran hazir: 320x240, font 346664 bayt
main: Bos yigin  dahili: 242619 bayt
main: Bos yigin  PSRAM : 2094528 bayt
```

Font bayt sayısı TTF dosyasıyla birebir (346,664), yani gömme ve
`lv_tiny_ttf` yüklemesi çalışıyor. **V-05 için taban değerler:** dahili
242.619 bayt, PSRAM 2.094.528 bayt — 3.3'te I2S tamponu eklendiğinde bu
sayılarla karşılaştırılacak.

### Kurulumda ve yapılandırmada çıkan üç tuzak

**1. `.venv` ESP-IDF kurulumunu iki kez düşürdü.** Hata mesajı
`This script was called from a virtual environment, can not create a virtual
environment again` idi. Sebep ortam değişkeni değil: backend'in
`.venv\Scripts` dizini **PATH'in başında** duruyor, dolayısıyla `python`
komutu venv'inkini çağırıyor ve `idf_tools.py` `sys.prefix != sys.base_prefix`
kontrolünden geçemiyor. `VIRTUAL_ENV` temizlemek yetmedi; PATH'ten venv'i
çıkarmak gerekti. `firmware\derle.bat` bunu her çağrıda yapıyor — aksi hâlde
her derleme aynı yere takılır.

**2. Sessiz yapılandırma hatası: flash 8 MB yerine 2 MB kaldı.**
`sdkconfig.defaults`'a `CONFIG_ESPTOOLD_FLASHSIZE_8MB` yazılmıştı — doğrusu
`ESPTOOLPY`. **Geçersiz anahtar hata vermez, sessizce yok sayılır** ve 2 MB
varsayılanı kalır. 945 KB'lik ikili 2 MB'a sığar, yani derleme başarılı
görünürdü; sorun ancak OTA bölümü eklenince patlardı. Üretilen `sdkconfig`
okunarak yakalandı — `sdkconfig.defaults` yazmak yetmiyor, çıktısını
doğrulamak gerekiyor.

**3. İki ayar ESP-IDF 5.3'te karşılıksızdı.** `CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE`
yok, çünkü `esp_websocket_client` artık yerleşik değil, bileşen kayıt
defterinden geliyor (3.6'da `idf_component.yml`'e eklenecek).
`CONFIG_LV_COLOR_16_SWAP` de LVGL 9'da kaldırılmış; bayt takası artık ekran
sürücüsünde (`display.c` -> `.swap_bytes = true`).

Doğrulanan ayarlar: `FLASHSIZE=8MB`, `SPIRAM=y`, `SPIRAM_MODE_QUAD=y`
(N8R2 için şart), `BT_ENABLED` kapalı, `LV_USE_TINY_TTF=y`.

### Karta yüklerken çıkan üç tuzak daha

**4. Uygulama bölümü %90 doluydu.** Açılış log'undaki bölüm tablosu
okununca görüldü: varsayılan tablo uygulamaya 1 MB veriyor, ikili ise
945 KB'a ulaşmıştı — 100 KB boş. Wi-Fi yığını tek başına bundan fazlasını
ekler, yani 3.5'te taşardı ve sebebi anlaşılmayan bir bağlantı hatası gibi
görünürdü. `partitions.csv` eklendi: uygulamaya 3 MB, ayrıca 1 MB spiffs.
8 MB flash'ta bu bedava. Şu an doluluk %30.

**5. Türkçe Windows kod sayfası yüklemeyi düşürdü.**
`UnicodeDecodeError: 'charmap' codec can't decode byte 0x9e` — cp1254,
karttan gelen ham baytları çözemiyor. `derle.bat`'e `PYTHONUTF8=1` ve
`PYTHONIOENCODING=utf-8` eklendi.

**6. Bölüm tablosu değişince eski `build/` bozuk çıktı üretti.**
`flash_args` içindeki uygulama satırı **adressiz** kaldı
(`  meb_sesli_asistan.bin`), esptool da `Address ... must be a number` dedi.
`idf.py fullclean` çözdü. Bölüm yapılandırması değiştiğinde temiz derleme
şart — kısmi derleme eski adresi taşıyor.

### 3.2'nin donanım turu — dört tuzak

Ekran ilk açılışta tamamen bozuktu. Sırayla elenen sebepler ve çıkan dersler:

**1. Düz renk dolgusu YÖN ve STRIDE hatalarını gizliyor.** İlk teşhis
denemesinde ekranı kırmızı/yeşil/mavi ile doldurdum, üçü de kusursuz çıktı ve
"SPI çalışıyor, sorun LVGL'de" sonucuna vardım. Yanlıştı: her piksel aynı
değerdeyse satır uzunluğu hatası **göze batmaz** — 320 piksellik satır 240'lık
panele taşsa da sonuç yine tek renk. Hata ancak metin gibi detaylı içerikte
ortaya çıkıyor. Doğru araç **asimetrik desen**: ekranı dört çeyreğe bölüp
farklı renklerle boyamak yönü tek bakışta gösterdi.

**2. Panel 240x320 DİKEY, 320x240 yatay değil.** `swap_xy(true)` ile yatay
varsayılmıştı; LVGL satır başına 320 piksel yazıyor, panel 240 bekliyordu.
Fotoğraftaki yatay bant gürültüsü bunun imzasıydı. Panel yerel yönünde
bırakıldı. **Doküman 320x240 diyor ama cihaz 240x320 dikey kalacak** — elde
tutulan bir cihaz için zaten daha uygun, 3.6 arayüzü buna göre tasarlanacak.

**3. `esp_lcd_panel_mirror` LVGL kurulumundan ÖNCE çağrılırsa sessizce yok
sayılıyor.** `lvgl_port_add_disp()` panel yönünü kendi yapılandırmasına göre
yeniden kuruyor. İki tur bu yüzden kaybedildi: `mirror(true,false)` ve
`mirror(true,true)` denendi, ikisi de ekranda **hiçbir** değişiklik yaratmadı
ve çağrı hata da vermedi. Doğru yer `lvgl_port_add_disp` **sonrası**.
Doğru değer bu modülde `mirror(false, true)` — yalnızca Y ekseni ters.

**4. Yön hatasını metne bakarak teşhis etmek yanıltıcı.** İçerik yalnızca
Y'de tersken kartı fiziksel olarak 180° çevirmek hem X hem Y'yi çeviriyor, bu
kez X ters kalıyor — hiçbir tutuşta okunmuyor ve "ayna" gibi görünüyor. Dört
ayna kombinasyonunu sırayla gösteren tek bir yükleme belirsizliği bitirdi.

**Elenen ama sebep OLMAYAN iki şey** (ileride tekrar şüphelenmemek için):
SPI hızı (40 → 10 MHz hiçbir şeyi değiştirmedi) ve PSRAM önbellek/DMA
tutarsızlığı (ölçüldü: tampon zaten %100 dahili RAM'de, PSRAM'den sıfır).

`display_selftest()` kodda duruyor, ekranla ilgili yeni bir şüphede tek
satırla geri açılabilir.

**Son durum:** 240x320, `mirror(false,true)`, SPI 10 MHz, Türkçe karakterler
(`ı`, `İ`, `ğ`, `ş` dahil) doğru çiziliyor, metin `LV_LABEL_LONG_WRAP` ve
genişlik sınırıyla ekrana sığıyor.

- [x] **3.3** I2S mikrofon kaydı (INMP441), PSRAM tamponu (V-05) — **donanımda doğrulandı**
### 3.3 — mikrofon çalışıyor

Ölçülen: **sessizde seviye 0-4, konuşurken 99.** Bu genişlikte bir fark
elektriksel gürültüyle açıklanamaz; mikrofon gerçekten sesi duyuyor.

Doğrulama yöntemi bilinçli seçildi: kaydın çalıştığı gözle görülmüyor, ham
baytlara bakmak da bir şey söylemiyor. Bu yüzden `audio_in_level()` tepe
değerini 0-100'e ölçekleyip ekrana çubuk olarak bastık — konuşurken yükselen
bir çubuk, kaydın gerçekten mikrofondan geldiğinin tek pratik kanıtı.
Seviyenin yalnızca *değişmesi* yetmezdi, çünkü gürültü de değişken görünür;
sesle ilişkili olması gerekiyordu.

**INMP441'in iki tuzağı koda not düşüldü:**

*24 bitlik veri 32 bitlik yuvada gelir.* Yuva 16 bit yapılandırılırsa
mikrofon susmaz ama örneklerin üst bitleri kesilir ve sonuç gürültüye benzer —
"çalışıyor gibi görünen" sinsi bir hata. Yuva 32 bit okunup `>> 16` ile 16
bite indiriliyor (backend PCM16 bekliyor).

*`L/R` pini kanalı seçer.* GND'de = sol kanal. Boşta ya da VDD'de bırakılırsa
mikrofon sağ kanala geçer ve okunan her örnek **sıfır** olur; kod hatasız
çalışır, ses gelmez. Sessiz kayıtta ilk bakılacak yer burası.

**Bellek:** kayıt tamponu 480 KB (15 sn) ve PSRAM'de. Dahili RAM'de 3.2'de
ölçülen boş yer ~242 KB olduğu için seçenek bile değildi — V-05'in uyardığı
durum tam olarak bu. Buna karşılık I2S okuma tamponu dahili RAM'de kalmak
zorunda; DMA PSRAM'e erişemiyor. İki katmanlı yapı bu yüzden var.

- [x] **3.4** I2S hoparlör çıkışı (MAX98357A) — **konuşma anlaşılır**; ses kalitesinde açık kalan kısıt var
### 3.4 — kod tamam, donanımda eksik parça vardı

Hoparlörden hiç ses gelmedi. Sırayla elenenler:

| Şüpheli | Sonuç |
|---|---|
| Kanal maskesi (SOL/SAĞ/İKİSİ) | Üçü de sessiz → sebep değil |
| `SD` pini sürülmüyor | Geri okuma **1** → pin doğru sürülüyor |
| Kablolama | Doğru |

**Asıl sebep: hoparlör yoktu.** MAX98357A yalnızca amplifikatör — I2S dijital
veriyi hoparlör sürecek analog güce çevirir, üzerinde hoparlör bulunmaz.
Zincirin son halkası eksikti:

```
ESP32 --I2S--> MAX98357A --analog--> [HOPARLÖR] --> ses
```

**Hoparlör geldikten sonra çıkan ikinci sorun: kablo teması.** Klemens
vidası kablonun yalıtkanına bastığı için bağlantı elle sıkı hissediliyordu
ama elektriksel temas yoktu. Yalıtkan 5-7 mm sıyrılıp teller toparlanınca
ses geldi. "Her şey doğru görünüyor ama ses yok" tablosunun üçüncü fiziksel
sebebi.

**Üçüncü sorun: cızırtı.** Uzun bir teşhis zinciri gerektirdi ve sırayla
elenenler şunlar:

| Şüpheli | Nasıl elendi |
|---|---|
| Örnek kaybı | DMA tamponu 6x240 → 8x512, ekran güncellemesi seyreltildi — değişmedi |
| Kırpılma (kayıt) | Kazanç yarıya indirildi — ses kısıldı, cızırtı sürdü |
| Bit hizalaması | Dört I2S çerçeve biçimi denendi — dördü de bozuk |
| Mikrofon | Cihazın kendi ürettiği ezgi çalındı — o da bozuk, mikrofon suçsuz |
| Aşırı sürme | Güç hesabı: %15 genlikte 0.04 W, hoparlör 1.5 W — mümkün değil |

**Bulunan iki gerçek sorun:**

*DC kayması.* Ham örnek dökümü aldığında görüldü: sessizken örnekler -4000
civarında salınıyor, konuşurken -10000 ile -6000 arası — hepsi negatif.
Dalga biçimi düzgündü (ardışık fark ort. 281), yani mikrofon okuması
doğruydu; sinyal yalnızca eksenden kaymıştı. Bu kayma hoparlörde koniyi bir
uca itip gerçek sesi onun üstüne bindiriyor. Tek kutuplu yüksek geçiren
süzgeç (R=0.995, ~12 Hz) eklendi; örnekler sıfır etrafına oturdu.

**Buradaki ders:** 3.3'te mikrofonu "çalışıyor" diye işaretlerken yalnızca
SEVİYENİN konuşmayla değiştiğini görmüştük. O test "enerji var" diyor, dalga
biçiminin doğru olduğunu SÖYLEMİYOR — DC kayması varken de seviye konuşmayı
takip ediyordu. Sinyalin kendisine bakmadan iki tur yanlış hedefe harcandı.

*Kutusuz hoparlör.* Frekans taramasında 200-800 Hz bozuk, 3200 Hz temiz
çıktı; hoparlörü kitap arasına koymak da düzeltti. Kutusuz küçük hoparlörün
baffle'ı yok, ön ve arka dalgalar birbirini götürüyor, koni telafi için
aşırı hareket ediyor. Çalma yoluna ~300 Hz yüksek geçiren süzgeç kondu —
telefon bandı zaten 300 Hz-3.4 kHz olduğu için konuşma anlaşılırlığından
hiçbir şey götürmüyor.

**Açık kalan kısıt.** Çıkış %45'e ölçeklendikten sonra konuşma anlaşılır ama
hafif cızırtı sürüyor. Kök sebep tam çözülemedi: %6 genlikte temiz, %15'te
bozuk olması bir baffle sorunundan daha keskin bir eşik. Kalan iki şüpheli:

1. **Hoparlörün kendisi** (sürtünen bobin tam böyle davranır) — ikinci bir
   hoparlörle test edilmeli, kod değişikliği gerekmiyor
2. **Besleme çökmesi** — Class-D amfi darbeli akım çeker; ince dupont
   kablolarla kart üzerindeki 5V hattından beslenirken gerilim çökebilir.
   Standart çözüm amfinin besleme uçlarına yakın 100-470 µF elektrolitik
   kondansatör

Demo için mevcut durum yeterli; ikisi de ucuz ve kodu etkilemiyor.

**Bu arada bir teşhis hatası da düzeltildi.** `SD` pininin durumunu
log'larken pin `GPIO_MODE_OUTPUT` yapılandırılmıştı; bu modda giriş tamponu
kapalı olduğu için `gpio_get_level()` pinin gerçek durumundan bağımsız olarak
**0** döndürüyor. "Pin sürülmüyor" gibi yanlış bir ipucu üretti.
`GPIO_MODE_INPUT_OUTPUT`'a çevrilince doğru değer (1) okundu. Bir çıkış
pinini geri okumak isteniyorsa bu mod şart.

Ayrıca çıkışın `slot_mask`'i açıkça `I2S_STD_SLOT_LEFT` yapıldı — mikrofonda
belirtilmişti ama çıkışta unutulmuştu. MAX98357A `SD` yüksekken sol kanalı
dinler; ESP32 sağ kanala yazsaydı amfi sessiz kalır ve hiçbir hata üretmezdi.

- [~] **3.5** Wi-Fi + NVS (ADR-09) — **bağlantı çalışıyor**; dokunmatik kurulum ekranı 3.5b'ye kaldı
### 3.5 — Wi-Fi bağlandı, ama üç ağ engeli çıktı

Cihaz ağa girdi: `IP 192.168.1.183`, dizüstü `192.168.1.89` — aynı /24.
Ayarlar NVS'te; kopan bağlantı 2 sn'de bir sınırsız yeniden deneniyor
(kalabalık 2.4 GHz bandında kopma olağan, kendiliğinden toparlanmazsa cihaz
sessizce ölür).

**Bellek: V-05'in uyardığı baskı gerçekleşti.**

| | 3.2'de | 3.5'te |
|---|---|---|
| Dahili RAM | 242.619 | **62.427** |
| PSRAM | 2.094.528 | 1.613.312 |

Wi-Fi yığını + I2S tamponları dahili RAM'den ~180 KB aldı. WebSocket ve TLS
tamponları da aynı havuzdan beslenecek; 3.6'da yakından izlenmeli.

**3.6'yı engelleyen üç ağ sorunu (R-01'in erken hâli):**

1. **Backend `127.0.0.1`'e bağlıydı.** Oturum boyunca uvicorn
   `--host 127.0.0.1` ile başlatılmıştı — yalnızca dizüstünün içinden
   erişilebilir. Cihaz için `--host 0.0.0.0` şart.
2. **Ağ profili "Public"** — Windows Güvenlik Duvarı bu profilde gelen
   bağlantıları varsayılan reddediyor.
3. **Port 8000 için açık kural yok.**

Doküman R-01: *"misafir Wi-Fi'de cihaz izolasyonu, port engeli → cihaz
backend'e hiç bağlanamaz, demo durur."* Aynı üçlü demo günü de karşımıza
çıkacak; `docs/manuel-test.md`'ye kontrol listesi olarak eklenmeli.

**Ayrıca bir açılış döngüsü hatası düzeltildi.** `audio_out_init()`
yanlışlıkla iki kez çağrıldı; ikincisi I2S1 zaten alındığı için
"no available channel" döndürdü, `ESP_ERROR_CHECK` paniğe düştü ve kart
sürekli yeniden başladı. Seri log'da art arda gelen aynı hata **tek bir
hatanın tekrarı değil, her yeniden başlatmanın izidir** — `rst:0xc
(RTC_SW_CPU_RST)` satırını aramak bunu ayırt etmenin en hızlı yolu.

- [x] **3.6** WebSocket istemcisi + sınıf/ders seçimi + durum makinesi

### 3.6b — dokunmatik: dört ayrı katman, her biri tek başına doğru

Dokunmatik çalışana kadar dört hata çıktı ve **hiçbiri kendi katmanında
yanlış görünmüyordu**. Bu, bu projede tekrar eden kalıbın en net örneği.

**1. Paylaşılan SPI kurulamıyordu.** Tasarım XPT2046'yı ekranın SPI2 hattına
ikinci aygıt olarak koyuyordu — elektriksel olarak doğru. Ama kurmak için
modülün `SCK`/`SDI` pinlerine ikinci birer kablo takmak gerekiyor ve Dupont
kablolarla bir pine iki kablo takılamıyor, breadboard da yok. Çözüm: boşta
duran SPI3'ü dokunmatiğe vermek. *Ders: elektriksel doğruluk yetmiyor, montaj
da tasarımın parçası.*

**2. Kalibrasyon ölü koddu.** NVS'e yazılıyor, okunuyor, loglanıyordu —
ama koordinat dönüşümünde hiç KULLANILMIYORDU. Sürücünün kendi
`(ham/4096)*genişlik` eşlemesi devredeydi ve panelin gerçek aralığını
(~200-3900, üstelik kaymalı) bilmiyordu. İlk donanım ölçümü hatayı sayıyla
gösterdi: sol üst köşe (192, 271), sağ alt köşe (44, 49) — hem ölçek yanlış
hem iki eksen ters.

**3. `sdkconfig.defaults` işe yaramamıştı.** ESP-IDF onu yalnızca `sdkconfig`
yokken uyguluyor; mevcut değer kazanıyor. `CONFIG_XPT2046_CONVERT_ADC_TO_COORDS`
`=y` kalmıştı, derleme yine temiz çıkıyordu. `CONFIG_ESPTOOLD_FLASHSIZE_8MB`
yazım hatasıyla aynı sınıf: **sessizce yok sayılan yapılandırma.**

**4. LVGL hiç okumuyordu.** `esp_lvgl_port`, `int_gpio_num` dolu olduğunda
LVGL'i `LV_INDEV_MODE_EVENT`'e alıyor — artık yoklamıyor, yalnızca PENIRQ
kesmesinde okuyor. XPT2046 ise `CONFIG_XPT2046_INTERRUPT_MODE` kapalıyken
PENIRQ'yu hiç sürmüyor. İki makul varsayılan birleşince okuma hiç
yapılmıyordu. Kalibrasyonun çalışıyor olması da yanıltıcıydı: o kod
`esp_lcd_touch_read_data()`'yı doğrudan çağırıp bu yolu atlıyordu.

Sonuçta yön bayrakları tamamen kaldırıldı: **iki köşeden kurulan doğrusal
eşleme, ters ekseni işaretli aritmetikle kendisi düzeltiyor.** `mirror_x`/
`mirror_y` tahmin etmek, kalibrasyonun zaten ölçtüğü bilgiyi ikinci kez elle
kodlamak olurdu.

Kalan gürültü ("dokunmadığım yere gitti") sürücünün 5 örnek ortalamasıyla
çözülmüyordu — **ortalama aykırı değeri reddetmez, seyreltir.** Süzgeç iki
kural işletiyor: her basışın ilk okuması yutuluyor (parmak oturmadan basınç
düşük, XPT2046 en gürültülü değerleri orada üretiyor) ve 300 ADC sayımından
uzak sıçramalar bir sonraki okumayla doğrulanmadan kabul edilmiyor. Gürültü
tamamen eleniyor, hızlı kaydırma yalnızca tek okuma gecikiyor.

**Katalog:** `/catalog` cevabı `{"grades": {...}}` biçiminde, kök nesne
doğrudan sınıf eşlemesi DEĞİL. Kökü kullanmak sessizce boş bir katalog
verirdi — ekran açılır, tek sınıf görünmezdi. Adres
`CONFIG_MEB_BACKEND_URL`'den türetiliyor; ayrı bir ayar olsaydı demo günü
ikisi birbirini tutmayabilirdi.

### 3.6c — yüz, akış yerleşimi ve LVGL'in kendi belleği

Tur ekranında yazılar üst üste biniyordu: etiketler sabit `y` konumlarına
çakılıydı, metnin boyu ise çalışma anında belli oluyor. Ekran dikey flex
akışına çevrildi; soru iki satırla, kaynak tek satırla sınırlandı, cevap
kalan yeri alıp kendi içinde kayıyor ve akarken en alta iniyor.

Ayrıca dört ifadeli bir yüz eklendi (hazır / dinliyor / düşünüyor /
konuşuyor). İfade, durum yazısıyla **aynı çağrıda** ayarlanıyor — ayrı
çağrılar olsaydı ikisi zamanla birbirinden kopar ve ekran "Hazir" yazarken
yüz konuşuyor görünürdü.

**Asıl ders: LVGL'in belleği ESP yığını DEĞİLDİ.**

Cevap konuşurken profil şeridine dokunulduğunda ekran siyah kalıyor ve cihaz
donuyordu. Log iki farklı belirti gösterdi:

    assert failed: stbtt__new_active stb_truetype_htcw.h:3160
    task_wdt: CPU 0: taskLVGL     <- lv_draw_layer_alloc_buf icinde

İkisi de aynı kökten: `CONFIG_LV_USE_BUILTIN_MALLOC` ile LVGL sabit **64 KB**
kendi havuzunu kullanıyordu ve `LV_MEM_POOL_EXPAND_SIZE=0` yüzünden
genişleyemiyordu. `lv_tiny_ttf` glifleri çalışma anında O HAVUZDAN ayırıyor.
Tur ekranı + 12 satırlık Türkçe liste havuzu tüketti.

Yanıltıcı olan: ESP yığınında 30 KB boş görünüyordu — ama LVGL orayı
kullanmıyordu. **"Bellek var" ölçümü, ölçtüğü havuz doğru değilse hiçbir şey
söylemiyor.**

Ayırma başarısız olunca `lv_draw_layer_alloc_buf` sonsuza kadar yeniden
deniyor ve bunu **LVGL kilidini tutarak** yapıyor; `display_lock(-1)` ile
bekleyen her şey sonsuza kadar takılıyor. Süresiz kilit bekleyişleri bu
yüzden sınırlandı: hata artık logda görünüyor, sessizce donmuyor.

Düzeltme `CONFIG_LV_USE_CLIB_MALLOC`: LVGL ESP yığınına bağlandı, PSRAM
farkındalığı zaten yapılandırılmıştı. Ölçülen sonuç, sabit havuz kalkınca
boş dahili RAM **43.383 -> 73.975 bayt** ve artık tavan yok.

### 3.6d — uzun cevap ve yarıda kesilen tur

Prompt'tan cevap uzunluk sınırı kaldırılınca cihazda iki ayrı yerde metin
kesiliyordu ve ikisi de "ekran aşağı inmiyor" gibi görünüyordu — oysa
kaydırma değil, VERİ eksikti.

**1. 512 baytlık tampon.** `ws_client.c` cevabı `static char metin[512]`
içinde tutuyordu. Backend `max_tokens=400` üretiyor; Türkçe UTF-8'de bu
~1.2-1.6 KB. Sınır kaldırılana kadar 512 yetiyordu, sonra sessizce kesmeye
başladı. 2048'e çıkarıldı.

**2. Parçalı WebSocket çerçevesi.** Olay JSON'u `buffer_size`'ı aşınca
istemci onu parça parça veriyor ve **devam parçalarında `op_code`
sıfırlanıyor**. Kod yalnızca `op_code == 0x01`'e baktığı için devam
parçaları hiçbir dala girmeden düşüyordu. Üstelik yarım JSON'da `strstr`
sessizce başarısız olur: alan bulunamaz, ekran güncellenmez, hata görünmez.
Çerçeve türü artık ilk parçadan hatırlanıyor ve JSON tamamlanmadan
ayrıştırılmıyor. Aynı düzeltme ses tarafında da bir sızıntıyı kapattı.

**3. Yarıda kesilen tur hafızaya girmiyordu.** `except asyncio.CancelledError`
hiçbir şey kaydetmeden yeniden fırlatıyordu. Ama o tur öğrenci için YAŞANDI:
cevabın bir kısmını duydu ve sonraki sorusu neredeyse her zaman ona atıfta
bulunuyor. Kayıt düşünce yeniden yazma adımının çözecek bir şeyi kalmıyor,
arama içerik kelimesi olmayan bir soruyla yapılıyor ve alakasız kaynak
dönüyor — yanlış breadcrumb'lı yanlış cevap, cevapsızlıktan kötü.

Kesilen tur artık kısa dönem hafızaya **işaretlenerek** yazılıyor
(`[öğrenci sözümü burada kesti]`). İşaret şart: işaretsiz saklansa model
cevabı tamamladığını sanır ve "devam et" denince yeni konuya geçerdi.
Postgres'e yazılmıyor — `_persist` `done` özetine (parçalar, gecikmeler,
`no_source`) ihtiyaç duyuyor ve iptal edilen turda bunların hiçbiri yok;
boş alanlarla satır yazmak panelin sayılarını yalan söyletirdi.

Test eklendi (`test_interrupted_answer_stays_in_history`) ve düzeltme
geçici olarak kaldırılıp BAŞARISIZ olduğu doğrulandı. Mevcut
`SlowOrchestrator` bunu ölçemiyordu: `answer_text`'i sesin sonunda
üretiyor, yani araya girildiğinde ortada hiç cevap metni olmuyor.

### Prompt üzerinde çalışırken: backend'i yeniden başlatmak

Uvicorn `--reload` olmadan modülü açılışta bir kez içe aktarıyor. Prompt
değişikliği çalışan sunucuyu ETKİLEMİYOR ve bu hiçbir yerde hata vermiyor —
bir kez 44 dakika eski promptla test edildi. `backendaslat.bat` eski
süreci kapatıp yenisini başlatıyor; `baslat.bat izle` `--reload` ekliyor
(açılış ölçüldü: ~2 sn, embed modeli diskte önbellekli).

**Kalan iş:** kötü bir kalibrasyonun tek çaresi hâlâ NVS'i silmek, ki bu
Wi-Fi bilgilerini de götürüyor. Ayarlar ekranına bir "Kalibrasyon" girdisi
gerekiyor (3.5b'deki klavyeyle birlikte).

### 3.5b — ekrandan Wi-Fi kurulumu (demo günü ağ değişimi)

Demo telefon hotspot'unda yapılacağı için menuconfig köprüsü yetmiyordu:
ağ değişince firmware yeniden derlemek gerekiyordu. Kurulum ekranı eklendi
(ağ taraması + parola + backend adresi), profil şeridinden ulaşılıyor.

**Backend adresi Wi-Fi kadar kritik.** Ağ değişince backend'i çalıştıran
dizüstünün IP'si de değişiyor; yalnızca SSID/parola değiştirilebilseydi
cihaz ağa girer ama backend'e ulaşamazdı.

**Kaydedince yeniden başlatılıyor.** Backend adresi ws_baslat()'a yalnızca
açılışta veriliyor. Çalışma anında değiştiren bir yol yazılabilirdi ama yeni
ve az denenmiş olurdu; açılış yolu defalarca doğrulandı ve 3,5 saniye sürüyor.

**Bulunan dört hata — hepsi ölçümle:**

1. *Yanlış parola cihazı kullanılamaz bırakıyordu.* Kurulum ekranı profil
   şeridinden açılıyor, o da tur ekranında, tur ise Wi-Fi olmadan hiç
   başlamıyor. Tek çare yeniden derlemekti. Artık Wi-Fi yoksa kurulum/bekleme
   ekranı kendiliğinden açılıyor.

2. *Parola gizliydi.* Dokunmatik klavyedeki yazım hatası görünmez kalıyor ve
   sonuç yalnızca "baglanamadi" oluyordu. Sebep kodu eklendi:
   **15 = 4WAY_HANDSHAKE_TIMEOUT** yanlış parolanın kesin imzası. Parola
   artık ekranda görünüyor.

3. *Tarama sıfır ağ döndürüyordu.* İki katmanlı: yeniden bağlanma döngüsü
   radyoyu meşgul ediyordu, ama döngüyü durdurmak yetmedi — kopma işleyicisi
   `vTaskDelay(2000)` içindeydi ve uyandığında **iki saniye önce verilmiş
   bayat bir kararı** uyguluyordu. Bayrak kontrolü beklemeden sonraya taşındı.

4. *Katalog eski adrese gidiyordu.* `katalog.c` adresi
   `CONFIG_MEB_BACKEND_URL`'den (derleme sabiti) türetiyordu, WebSocket ise
   `net_backend()`'ten (NVS). Hotspot'a geçince ayrıştılar: cihaz çalışıyor
   görünürken ders değiştirmek imkânsız oldu. **Aynı değerin iki kaynağı
   olmamalı.**

**Sabit zaman aşımı yanlıştı.** Telefon hotspot'ları istemci yokken uykuya
geçiyor; ölçümde ilk iki bağlanma denemesi reddedilip üçüncüsü ~10. saniyede
tuttu — eski kod tam orada pes ediyordu. Artık bekleme ekranı süresiz
bekliyor, ağ ortaya çıkınca kendiliğinden bağlanıyor; ayarlara gitmek
isteğe bağlı.

### Anahat: yeniden yazmaya yetmedi, cevaba da gerekti

"Bu kitapta neler var" sorusunda cevap yalnızca ilk temayı anlatıyordu:
model eline yalnızca top_k (3) parça geçiyordu. Anahat cevap bağlamına da
eklendi — ama **ayrı bir başlık altında verildiğinde model onu kaynak
saymadı** ve KAYNAK_YOK deyip genel bilgiyle cevap uydurdu. `[1] KİTABIN
İÇİNDEKİLER` diye NUMARALI BİR KAYNAK yapılınca kurallar onu kapsadı.

Kural 4'e ayrıca "işaretten sonra soruyu yine de cevaplama" eklendi: o
satırlar seslendiriliyor ve kitapta olmayan bilgi kitaptanmış gibi
duyuluyordu.

## Aşama 4 — Panel (Gün 10-12)

- [ ] **4.1** Tek şifre ile giriş (Açık Soru 7)
- [ ] **4.2** Canlı cihaz durumu + zaman çizelgesi (SSE)
- [ ] **4.3** Sohbet listesi ve tur detayı (kaynak breadcrumb'ları görünür)
- [ ] **4.4** Top-K / temperature / model ayarı — çalışma anında

## Aşama 5 — Demo hazırlığı (Gün 12-14)

- [ ] **5.1** Türkçe TTS kulak testi, gerekirse sağlayıcı değişimi (Açık Soru 3, R-02)
- [ ] **5.2** Cevap uzunluğu sınırının ayarı (Açık Soru 6)
- [ ] **5.3** Barge-in doğrulaması
- [ ] **5.4** Hotspot senaryosu provası (R-01)
- [ ] **5.5** Yedek yolların testi: tarayıcı test sayfası, filtresiz arama
- [ ] **5.6** Demo provası

---

## Sıra neden böyle

- **Aşama 0 en başta**, çünkü `grade` değerlerinin gerçek biçimi hem
  `/catalog` ucunu hem cihaz ekranındaki seçim listesini belirliyor. Yanlış
  varsayımla ilerlersek iki yeri birden düzeltmek gerekir.
- **Akış (streaming) Aşama 2'ye bırakıldı.** Walking skeleton'ın amacı zinciri
  görmek; akışı ve iptali baştan karıştırmak, çalışmayan bir zinciri iki kat
  zor hata ayıklanır hale getirir.
- **Panel en sonda**, R-05 gereği: kapsam kayması olursa kesilecek ilk şey
  panel, korunacak son şey ses döngüsü.
