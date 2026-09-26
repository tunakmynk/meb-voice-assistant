# Manuel Test Rehberi

Şu ana kadar yapılanları elle doğrulamak için. Testler **ucuzdan pahalıya**
sıralı: ilk ikisi hiç para harcamaz, sonrakiler OpenAI kredisi kullanır.

---

## 0. Ön koşullar

Her testten önce bu ikisi ayakta olmalı.

### Docker (Qdrant + Postgres)

```bash
cd "c:\Users\tunak\Desktop\Internship Project"
docker compose up -d
docker compose ps
```

İkisi de `healthy` görünmeli. Docker Desktop kapalıysa önce onu başlatın.

### Backend

```bash
cd "c:\Users\tunak\Desktop\Internship Project\backend"
.venv\Scripts\python.exe -m uvicorn app.main:app --reload
```

Açılışta ~2 GB embedding modeli yükleniyor; `Warmup tamam, servis hazır.`
satırını görmeden test etmeyin.

Kontrol:

```bash
curl http://127.0.0.1:8000/health
```

`"api_key_set": true` görmelisiniz. `false` ise `backend/.env` içindeki
`OPENAI_API_KEY` boş demektir.

---

## 1. Birim testler — ağsız, ücretsiz

```bash
cd backend
.venv\Scripts\python.exe -m pytest tests/ -v
```

**Beklenen:** 21 test geçer, birkaç saniye sürer.

Neyi doğrular: cümle bölücü (`1923.`, `8.`, `T.C.` cümle bitirmiyor),
`KAYNAK_YOK` tespiti, olay sözleşmesinin sırası, işaretin asla
seslendirilmemesi. Hiçbiri OpenAI'a gitmez — sahte adapter'lar kullanılır.

---

## 2. Retrieval duman testi — ücretsiz (yerel model)

```bash
.venv\Scripts\python.exe scripts\smoke_retrieval.py
```

**Beklenen:** 6 senaryo, hepsi `[GECTI]`.

Elle bakılacak şey sadece "geçti" değil, **dönen breadcrumb'lar mantıklı mı**:
Lozan sorusu `3.6 LOZAN BARIŞ ANTLAŞMASI` getirmeli, cam sorusu
`5.3 Günlük Hayatta Hâl Değişimleri`.

Koleksiyonun kendisini görmek için:

```bash
.venv\Scripts\python.exe scripts\check_qdrant.py
```

8577 nokta, `grade`/`subject` index'leri açık, sınıf ve ders değerlerinin
gerçek yazımları listelenir.

---

## 3. Tarayıcı testi — en gerçekçi (mikrofonlu)

Sunucu açıkken tarayıcıda:

```
http://localhost:8000
```

1. Üstten **sınıf** seçin — ders listesi otomatik daralır.
   (Bunu deneyin: 7. sınıfı seçin, **Matematik çıkmaz**. 8. sınıfta
   `T.C. İnkılap Tarihi...`, 12. sınıfta `İnkılap Tarihi...` yazar. Bu bir
   hata değil, kitapların gerçek adları — plan 0.4.)
2. **"Bas ve konuş"** düğmesine basılı tutun, Türkçe soruyu sorun, bırakın.
   İlk seferde tarayıcı mikrofon izni ister.
3. Sırayla görmeniz gerekenler: soru metni → "Düşünüyor…" → cevap metni
   büyüyerek yazılır → ses çalar → altta kaynak breadcrumb'ı.
4. En altta `done` satırında aşama süreleri yazar.

**Denemeye değer iki senaryo:**

| Ne sorun | Beklenen |
|---|---|
| 8. Sınıf / Fen Bilimleri → "ısı ve sıcaklık arasındaki fark nedir" | Gerekçeli cevap + breadcrumb |
| 6. Sınıf / Fen Bilimleri → "kışın camlar neden terler" | "Bunu kitabında bulamadım" (konu 6. sınıf kitabında yok) |

Aynı sorunun sınıfa göre farklı davranması sistemin çalıştığının en iyi
kanıtı.

---

## 4. Uçtan uca script — mikrofonsuz

Mikrofon yoksa ya da tekrarlanabilir ölçüm isteniyorsa. Soruyu TTS ile sese
çevirip cihaz kaydetmiş gibi geri besler.

```bash
.venv\Scripts\python.exe scripts\e2e_turn.py
.venv\Scripts\python.exe scripts\e2e_turn.py "yoğunluk nasıl hesaplanır" "6.Sınıf" "Fen Bilimleri"
```

**Beklenen:** aşama süreleri ve `ILK SESE KADAR ... GECTI`.

Tipik 2.7-3.8 sn. Ara sıra `ASILDI` görebilirsiniz — bu genelde OpenAI'ın
yavaş cevap verdiği anlamına gelir, kodda bir bozulma değil. Emin olmak için
2-3 kez koşun.

Maliyet: koşu başına ~2 sent.

---

## 5. Cevap kalitesi eval'i

```bash
.venv\Scripts\python.exe scripts\eval_no_source.py
```

12 soru: 6'sının cevabı kitapta **var**, 6'sının **yok**.

**Beklenen:**

```
KAPSAM ICI  (6): cevapladi 6, asiri reddetti 0  -> %100 dogru
KAPSAM DISI (6): KAYNAK_YOK 6, uydurma riski 0  -> %100 dogru
```

İki oran birlikte bakılmalı. Yalnızca kapsam dışını ölçmek yanıltıcıdır:
"her şeye KAYNAK_YOK de" diyen bir prompt o metriği %100 yapar ama asistanı
işe yaramaz hale getirir. `asiri reddetti` sayısı bunu yakalar.

`temperature 0.2` olduğu için tek koşu garanti değildir; prompt'ta değişiklik
yaptıysanız 2 kez koşun.

Maliyet: koşu başına ~1 sent.

---

## Sık karşılaşılanlar

| Belirti | Sebep |
|---|---|
| `All connection attempts failed` | Docker kapalı → `docker compose up -d` |
| `insufficient_quota` | OpenAI bakiyesi bitti |
| `api_key_set: false` | `backend/.env` içinde `OPENAI_API_KEY` boş |
| Tarayıcıda ses yok | Sayfaya bir kez tıklayın; tarayıcılar etkileşim öncesi ses çalmayı engeller |
| Katalog boş | Sunucu Qdrant'a ulaşamıyor; `check_qdrant.py` ile bakın |
| Ayarı değiştirdim, etkisi yok | Sunucuyu yeniden başlatın (`--reload` yoksa) |
