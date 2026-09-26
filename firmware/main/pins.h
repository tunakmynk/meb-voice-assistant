/*
 * GPIO pin haritası — ESP32-S3 N8R2 (plan 3.1, Açık Soru 8/9/10).
 *
 * Doküman 1. bölüm: "Pin çakışması riski. ILI9341 (SPI) ile iki I2S hattının
 * pin ataması, kart üzerindeki ayrılmış pinlerle çakışmayacak şekilde ilk gün
 * sabitlenmelidir." Bu dosya o sabitlemedir.
 *
 * ============================================================
 * KULLANILAMAZ PİNLER — neden kaçınıldığı
 * ============================================================
 *
 *   GPIO 26-32   Dahili SPI flash. Kullanılırsa kart açılmaz.
 *   GPIO 33-37   N8R2'de (quad PSRAM) SERBEST. Dikkat: N8R8 gibi octal PSRAM
 *                modellerinde bu pinler PSRAM'e gider. Kart N8R2 olduğu için
 *                kullanılabilir, ama farklı bir karta geçilirse ilk bakılacak
 *                yer burasıdır.
 *   GPIO 19, 20  Yerleşik USB (D-/D+). USB ile programlama/konsol
 *                kullanılacaksa dokunulmamalı.
 *   GPIO 43, 44  UART0 TX/RX — seri konsol.
 *   GPIO 0       Boot strapping (BOOT düğmesi).
 *   GPIO 3       Strapping (JTAG kaynak seçimi).
 *   GPIO 45      Strapping (VDD_SPI gerilimi).
 *   GPIO 46      Strapping (boot modu); ayrıca çoğu kartta yalnızca giriş.
 *
 *   ADC2 kanalları Wi-Fi açıkken okunamaz — bu projede analog giriş yok,
 *   dolayısıyla sorun oluşturmuyor.
 *
 * Aşağıdaki atama yalnızca GPIO 4-18, 21, 47, 48 aralığını kullanır; hiçbiri
 * yukarıdaki listede değildir.
 */

#pragma once

/* ---------------- Ekran: ILI9341, SPI2 (FSPI) ---------------- */
/* Dokunmatik denetleyici AYRI veri yolunda (SPI3) — gerekçesi aşağıda. */
#define PIN_LCD_SCK    12
#define PIN_LCD_MOSI   11
/* MISO BAĞLANMIYOR. Panelden hiç okuma yapmıyoruz ve dokunmatik artık ayrı
 * veri yolunda (aşağıya bakın), dolayısıyla ekranın SDO'su boşta bırakılıyor.
 * Veri yolu -1 ile kuruluyor; GPIO 13 başka bir işe serbest. */
#define PIN_LCD_CS     10
#define PIN_LCD_DC     14
#define PIN_LCD_RST    21
#define PIN_LCD_BL     47   /* arka ışık; LEDC ile karartma yapılabilir */

/* ---------------- Dokunmatik: XPT2046, SPI3 (Açık Soru 9) ---------------- */
/* 2.4" ILI9341 modüllerinde standart denetleyici XPT2046'dır. Direnç tabanlı
 * olduğu için KALİBRASYON GEREKİR: ham ADC değerleri panelden panele kayar,
 * iki karşıt köşe ölçülüp ölçek/kaydırma NVS'e yazılmalı (ADR-09'daki NVS
 * kullanımı zaten var). Kalibrasyonsuz dokunmatik, ekranın kenarlarında birkaç
 * piksele kadar sapar — sınıf/ders listesinde yanlış satır seçilmesi demektir.
 *
 * NEDEN AYRI VERİ YOLU
 * İlk tasarım SPI2'yi ekranla paylaştırıyordu; elektriksel olarak doğru ama
 * PROTOTİPTE KURULAMIYOR. Paylaşım, modülün SCK/SDI pinlerine ikinci birer
 * kablo takmayı gerektiriyor; Dupont kablolarla bir pine iki kablo takılamaz
 * ve elde breadboard yok. SPI3 bu projede zaten boşta durduğu için ayırmanın
 * maliyeti sıfır — üstelik ekran 10 MHz'de kalırken dokunmatik 2 MHz'de
 * bağımsız çalışıyor, veri yolu çekişmesi hiç olmuyor.
 *
 * GPIO 39-42 varsayılan olarak JTAG'e gider. Kart USB-Serial-JTAG kullandığı
 * için (konsol GPIO 19/20 üzerinden) bu pinler serbest. */
/* Modül üstündeki serigrafi üreticiye göre değişiyor; ikinci sütun aynı
 * bacağın diğer yaygın adı. XPT2046'nın DOUT bacağı hem T_DO hem T_OUT hem de
 * T_MISO olarak yazılabiliyor — üçü de aynı pin. */
#define PIN_TOUCH_SCK  39   /* modülde T_CLK */
#define PIN_TOUCH_MOSI 40   /* modülde T_DIN  ya da T_IN  */
#define PIN_TOUCH_MISO 41   /* modülde T_DO   ya da T_OUT */
#define PIN_TOUCH_CS   15
/* T_IRQ kablosu bağlı ama şu an KULLANILMIYOR: sürücüye pin verilince
 * esp_lvgl_port LVGL'i olay kipine alıyor ve XPT2046 varsayılan ayarda
 * PENIRQ'yu sürmediği için hiç okuma yapılmıyor (touch.c'deki gerekçe).
 * Kablo yerinde duruyor — ileride güç tasarrufu istenirse kesme kipi
 * yazılımdan açılabilir. */
#define PIN_TOUCH_IRQ  16

/* ---------------- Mikrofon: INMP441, I2S_NUM_0 (giriş) ---------------- */
/* Doküman: kayıt ve çalma İKİ AYRI I2S kanalı olarak yapılandırılır.
 * Tek kanalı yön değiştirerek kullanmak, bas-konuş geçişlerinde sürücüyü
 * yeniden kurmayı gerektirirdi. */
#define PIN_MIC_SCK     4   /* BCLK */
#define PIN_MIC_WS      5   /* LRCL / word select */
#define PIN_MIC_SD      6   /* veri girişi */
/* INMP441'in L/R pini GND'ye bağlanır -> sol kanal. VDD'ye bağlanırsa sağ
 * kanaldan yayın yapar ve sol kanalı okuyan sürücü SESSİZLİK alır — sessiz
 * kayıtta ilk bakılacak yer burası. */

/* ---------------- Hoparlör: MAX98357A, I2S_NUM_1 (çıkış) ---------------- */
#define PIN_SPK_BCLK    7
#define PIN_SPK_LRC     8
#define PIN_SPK_DIN     9

/* SD pini (Açık Soru 10) — GPIO'ya bağlanıyor, sabit VDD'ye DEĞİL.
 *
 * MAX98357A'da SD tek başına "kapat/aç" değil, aynı zamanda kanal seçicidir:
 *     < 0.16 V   kapalı (shutdown)
 *     0.16-0.77  sağ kanal
 *     0.77-1.4   (sol+sağ)/2
 *     > 1.4 V    sol kanal
 * Doğrudan GPIO'dan HIGH sürmek 3.3 V verir -> sol kanal, yani mono çıkışımız.
 * LOW sürmek amfiyi tamamen kapatır.
 *
 * Neden GPIO: ADR-02 gereği kayıt ve çalma aynı anda olmuyor. Konuşma dışında
 * amfi kapatılırsa Class-D'nin boştaki cızırtısı duyulmaz — sessiz bir sınıfta
 * sürekli hışırdayan bir cihaz kötü izlenim bırakır. Ayrıca barge-in'de
 * (ADR-04) hoparlörü anında susturmanın en kesin yolu budur. */
#define PIN_SPK_SD     17

/* GAIN pini bağlanmıyor — boşta bırakılıyor (9 dB).
 *     boşta        9 dB   <- seçilen
 *     GND'ye       12 dB
 *     VDD'ye        6 dB
 *     100k-GND     15 dB
 *     100k-VDD      3 dB
 * Tek küçük hoparlör için 9 dB yeterli; yüksek kazanç, Türkçe TTS'in zaten
 * normalize çıkışında kırpılmaya yol açar. Demo'da ses zayıf kalırsa önce
 * yazılım tarafında ölçekleme denenmeli, sonra GND'ye çekilmeli. */

/* ---------------- Bas-konuş ---------------- */
/* ADR-02: konuşma ekrana basılı tutarak başlar. Dokunmatik ekran birincil
 * giriş; ayrıca fiziksel bir yedek düğme öngörülmüştü (R-03'ün "adım adım
 * doğrulama" yaklaşımı).
 *
 * Test aşamasında GPIO 0 kullanılıyor: kart üzerindeki BOOT düğmesi bu pine
 * bağlı ve basılınca GND'ye çekiyor. Hiç kablo gerektirmiyor — prototip
 * kablolamasında boşta GND pini kalmamıştı.
 *
 * GPIO 0 bir strapping pini: RESET ANINDA basılı tutulursa kart bootloader
 * moduna girer. Açılıştan sonra normal giriş olarak kullanmak sorunsuz, ama
 * ürün kartında ayrı bir düğme (aşağıdaki 18) tercih edilmeli. */
#define PIN_PTT_BUTTON 0

/* Ürün kartındaki fiziksel düğme için ayrılmış pin. Test sırasında BOOT
 * kullanıldığı için şu an bağlı değil. */
#define PIN_PTT_BUTTON_URUN 18

/* ---------------- Ses formatı ---------------- */
/* Açık Soru 5: PCM 16 kHz mono varsayımı hem INMP441 hem STT için doğrulandı.
 * INMP441 24-bit örnek üretir; sürücüde 32-bit slot okunup üst 16 bit alınır.
 * Backend config'i (audio_sample_rate) ile birebir aynı olmalı. */
#define MIC_SAMPLE_RATE   16000
#define MIC_BITS_PER_SAMPLE 32  /* slot genişliği; 16 bit'e indirilerek gönderilir */

/* TTS geri dönüşü 24 kHz mono 16-bit ham PCM (backend: settings.tts_format
 * = "pcm", tts_sample_rate = 24000). Akışlı PCM seçildiği için cihazda MP3
 * ÇÖZÜCÜ GEREKMİYOR — baytlar doğrudan I2S'e yazılır (plan 2.1 ölçümü). */
#define SPK_SAMPLE_RATE   24000
#define SPK_BITS_PER_SAMPLE 16
