# Demo kılavuzu

Telefon hotspot'u üzerinde çalışan kurulum için. Buradaki her madde bir
provada gerçekten yaşandı; sıralama tahmin değil, ölçüm sonucu.

---

## 1. Açılış sırası — bu sıra önemli

Sıra bozulursa cihaza yanlış adres girilir ve sebebi görünmez.

1. **Telefonun hotspot'unu aç.** Bandı **2,4 GHz** olmalı — ESP32-S3 5 GHz
   görmez. Telefonun hotspot ekranını açık bırak; bazı telefonlar istemci
   yokken hotspot'u uykuya alıyor.

2. **Dizüstünü hotspot'a bağla.**

3. **IP'yi öğren.** Terminalde:

   ```
   ipconfig
   ```

   *Kablosuz LAN bağdaştırıcısı Wi-Fi* bölümündeki **IPv4 Adresi**.
   Bu adres her bağlanışta değişebilir — mutlaka bak.

4. **Backend'i başlat.**

   ```
   backend\baslat.bat
   ```

   Pencerede son satır şu olmalı ve **pencere demo boyunca kapanmamalı**:

   ```
   INFO:  Uvicorn running on http://0.0.0.0:8000
   ```

5. **Cihazı aç.** Ekranda 10-20 saniye "Wi-Fi araniyor" yazabilir —
   **bekle**, kendisi bağlanır. Ölçümde ilk iki deneme reddedilip üçüncüsü
   tutmuştu.

6. **Adres değiştiyse cihazdan gir:** profil şeridi → *Wi-Fi / Backend
   ayarlari* → ağ → parola → `ws://<yeni IP>:8000/ws`. Kaydedince cihaz
   yeniden başlar.

---

## 2. Başlamadan önce üç kontrol

| Kontrol | Nasıl | Beklenen |
|---|---|---|
| Backend ayakta mı | Tarayıcıda `http://localhost:8000/health` | `"status":"ok"` |
| Qdrant ayakta mı | `http://localhost:6333/collections` | `meb-books-v2` |
| Cihaz bağlı mı | Cihaz ekranı | Üstte profil şeridi, altta "Basili tutun" |

Cihaz ekranında **"Backend'e baglanilamadi"** yazıyorsa önce backend
penceresine bak — çoğu zaman sebep cihaz değil, kapanmış sunucudur.

---

## 3. Demo akışı

Panele basılı tutup konuş, bırak. Sırasıyla:

1. **Düz soru** — seçili dersten bir konu sor. Ekranda soru, cevap ve
   altta kaynak ünitesi görünür.
2. **Takip sorusu** — "bununla ilgili bir örnek verir misin?" Atıf çözülüp
   doğru kaynağa gider.
3. **Araya girme** — cevap konuşurken panele bas. Ses kesilir, yeni soru
   başlar; duyulan kısım hafızada kalır.
4. **Kaynak satırına dokun** — breadcrumb'ın tamamı açılır.
5. **Ders değiştir** — profil şeridine dokun, sınıf ve ders seç.
6. **Kapsam dışı soru sor** — cihaz kitapta olmadığını söyler, uydurmaz.
   Koruma katmanı ölçülüyor: `backend\.venv\Scripts\python.exe
   scripts\eval_no_source.py` → 12/12.

---

## 4. Bir şey ters giderse

| Belirti | Sebep | Ne yapmalı |
|---|---|---|
| "Backend'e baglanilamadi" | Sunucu kapalı ya da IP değişti | `baslat.bat`; `ipconfig` ile IP'yi doğrula, cihazdan güncelle |
| Ekranda "Wi-Fi araniyor" kalıyor | Hotspot kapalı/uykuda | Telefonda hotspot'u aç, ekranını açık bırak. Cihaz kendisi bağlanır |
| Ağ listesi boş | Hotspot 5 GHz ya da kapalı | Telefonda bandı 2,4 GHz yap, "Yeniden tara" |
| Kaydettim, bağlanmıyor | Parola yanlış | Parola ekranda **görünür**; telefondaki ile harf harf karşılaştır. Büyük harf ⇧, rakam `1#` |
| "Katalog alinamadi" | Backend'e HTTP erişimi yok | Sunucu ayakta mı, IP doğru mu |
| Ses gelmiyor | Amfi kapalı kalmış | Cihazı sıfırla |
| Cihaz tamamen çalışmıyor | — | **Yedek yola geç** (aşağıda) |

### Sebep kodu okuma

Seri logda `baglanti koptu (sebep N)`:

- **15** — parola yanlış (en yaygın)
- **201** — ağ bulunamadı; hotspot kapalı ya da 5 GHz
- **202** — kimlik doğrulama reddedildi; hotspot uyanıyor olabilir, birkaç
  deneme sonra tutabilir
- **205** — genel bağlantı hatası

---

## 5. Yedek yol (R-03)

Cihaz hiç çalışmazsa **dizüstü tarayıcısından** aynı sistemi göster:

```
http://localhost:8000/
```

Aynı backend, aynı RAG, aynı kaynak gösterimi. Sınıf/ders açılır menüden
seçilir, mikrofon düğmesine basılı tutulur. Araya girme de çalışır.

Bu sayfa cihazla **aynı olay sözleşmesini** kullanıyor; yani cihazda
gösterdiğin her şeyi burada da gösterebilirsin.

---

## 6. Bilinen pürüzler

Sorulursa dürüst cevap; ikisi de bilinçli tercihlerin sonucu.

- **"Kitabın içeriği nedir?"** sorusunda cevap doğru ve kitaptan geliyor,
  ama sistem onu yanlışlıkla "kaynak bulunamadı" diye etiketliyor: kaynak
  satırı boş kalır, renk sarıya döner. Yanlış bilgi değil, yanlış etiket.

- **Cevaplar uzun olabiliyor.** Cevap uzunluk sınırı promptdan bilerek
  kaldırıldı (daha doğal anlatım için). "Başlıkları sayar mısın" gibi bir
  soruda 44 başlığın tamamı okunuyor — yaklaşık iki dakika. Demo'da bu
  soruyu sormamak ya da araya girip kesmek daha iyi.

- **Dokunmatik klavyede Türkçe karakter yok.** SSID listeden seçildiği için
  sorun çıkarmıyor; parolada Türkçe karakter varsa sorun olur.
