/*
 * Giriş noktası (plan Aşama 3).
 *
 * Şu an yalnızca 3.2'yi kapsıyor: ekran açılıyor ve Türkçe karakterlerin
 * doğru çizildiğini gösteren bir doğrulama ekranı basıyor.
 *
 * R-03'ün önerdiği sıra bilinçli: ekran -> mikrofon -> hoparlör -> ağ.
 * Ekran ilk sırada, çünkü sonraki adımların hata ayıklaması buna dayanıyor —
 * mikrofon sessiz kaldığında bunu ekrandan görebilmek gerekiyor.
 */

#include "audio_in.h"
#include "pins.h"
#include "audio_out.h"
#include "display.h"
#include "net.h"
#include "katalog.h"
#include "kurulum.h"
#include "secim.h"
#include "touch.h"
#include "tur.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "main";

/* Türkçe'nin ASCII dışına çıkan her harfi. Ekranda boş kutu görünen karakter
 * varsa font aralığı değil, gömülü TTF'nin kendisi eksik demektir — bu satır
 * o ayrımı gözle yapılabilir kılıyor. */
static const char *TR_PROBE = "ÇĞİÖŞÜçğıöşü — Isı ve sıcaklık";

/* Ekran 240x320 DİKEY. Doküman 320x240 yatay varsayıyordu ama 2.4" ILI9341
 * modülü dikey üretiliyor; panel katmanında döndürmek stride hatası ürettiği
 * için yerel yönde bırakıldı (bkz. display.c). Arayüz tasarımı 3.6'da buna
 * göre yapılacak — dikey, elde tutulan bir cihaz için zaten daha uygun. */
#define EKRAN_G 240
#define KENAR    12

static void build_probe_screen(void)
{
    if (!display_lock(-1)) {
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F1115), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "MEB Sesli Asistan");
    lv_obj_set_style_text_font(title, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6E8EE), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, KENAR);

    /* Genişlik sınırı + satır kaydırma ŞART. Bunlar olmadan uzun metin tek
     * satırda kalıp ekranın iki yanından taşıyor — ilk denemede tam olarak
     * bu oldu ve yazının başı ile sonu görünmedi. */
    lv_obj_t *probe = lv_label_create(scr);
    lv_label_set_text(probe, TR_PROBE);
    lv_label_set_long_mode(probe, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(probe, EKRAN_G - 2 * KENAR);
    lv_obj_set_style_text_align(probe, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(probe, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(probe, lv_color_hex(0x4C8DFF), LV_PART_MAIN);
    lv_obj_align(probe, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *state = lv_label_create(scr);
    lv_label_set_text(state, "3.2 tamam — ekran, font, yön");
    lv_label_set_long_mode(state, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(state, EKRAN_G - 2 * KENAR);
    lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(state, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(state, lv_color_hex(0x8B93A7), LV_PART_MAIN);
    lv_obj_align(state, LV_ALIGN_BOTTOM_MID, 0, -KENAR);

    display_unlock();
}

static lv_obj_t *s_bar;
static lv_obj_t *s_bilgi;

static void mic_ekrani_kur(void)
{
    if (!display_lock(-1)) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();

    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, EKRAN_G - 2 * KENAR, 24);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 40);

    s_bilgi = lv_label_create(scr);
    lv_label_set_text(s_bilgi, "Mikrofon: konusun");
    lv_obj_set_style_text_font(s_bilgi, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_bilgi, lv_color_hex(0x8B93A7), LV_PART_MAIN);
    lv_obj_align(s_bilgi, LV_ALIGN_CENTER, 0, 75);

    display_unlock();
}

/* 3.4 doğrulaması: 3 saniye kaydet, sonra aynısını hoparlörden çal.
 *
 * Loopback bilinçli seçildi. Tek başına ton çalmak hoparlörü doğrular ama
 * zincirin tamamını değil; loopback mikrofon -> PSRAM -> hoparlör yolunu
 * baştan sona kanıtlıyor ve ADR-02'nin bas-konuş döngüsünün aynısı.
 *
 * Ton testi yine de açılışta bir kez çalıyor: loopback'te ses gelmezse
 * suçlunun mikrofon mu hoparlör mü olduğunu ayırt etmek gerekiyor. */
#define LOOPBACK_SANIYE 3
#define LOOPBACK_BAYT (MIC_SAMPLE_RATE * 2 * LOOPBACK_SANIYE)

static void ses_gorevi(void *arg)
{
    (void)arg;

    while (true) {
        /* --- KAYIT --- */
        if (display_lock(50)) {
            lv_label_set_text(s_bilgi, "KAYIT - konusun");
            lv_obj_set_style_text_color(s_bilgi, lv_color_hex(0xE0483D), LV_PART_MAIN);
            display_unlock();
        }
        audio_in_start();
        int sayac = 0;
        while (audio_in_size() < LOOPBACK_BAYT) {
            audio_in_pump();
            /* Ekranı HER pump'ta güncellemek örnek kaybettiriyor: bir pump
             * 32 ms ses okuyor, LVGL + SPI aktarımı bundan uzun sürebiliyor
             * ve I2S DMA tamponu taşıyor. 16'da bir güncelleme hem yeterince
             * akıcı görünüyor hem de okumayı aksatmıyor. */
            if (++sayac >= 16) {
                sayac = 0;
                if (display_lock(5)) {
                    lv_bar_set_value(s_bar, audio_in_level(), LV_ANIM_OFF);
                    display_unlock();
                }
            }
        }
        audio_in_stop();

        /* --- ÇALMA --- */
        if (display_lock(50)) {
            lv_label_set_text(s_bilgi, "CALIYOR");
            lv_obj_set_style_text_color(s_bilgi, lv_color_hex(0x4C8DFF), LV_PART_MAIN);
            lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
            display_unlock();
        }
        ESP_LOGI(TAG, "loopback: %u bayt caliniyor", (unsigned)audio_in_size());
        audio_in_dokum(64);

        /* Kayıt 16 kHz; TTS 24 kHz gönderecek. Hız kaynağa göre ayarlanmazsa
         * ses tiz ya da pes çıkar. */
        audio_out_set_rate(MIC_SAMPLE_RATE);
        audio_out_hp_sifirla();
        audio_out_enable(true);
        audio_out_write(audio_in_data(), audio_in_size());
        audio_out_flush();

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static const char *durum_metni(net_durum_t d)
{
    switch (d) {
        case NET_BAGLANIYOR: return "Wi-Fi: baglaniyor...";
        case NET_BAGLI:      return "Wi-Fi: bagli";
        case NET_HATA:       return "Wi-Fi: hata";
        default:             return "Wi-Fi: ayar yok";
    }
}

static void ag_gorevi(void *arg)
{
    (void)arg;
    net_durum_t onceki = (net_durum_t)-1;

    while (true) {
        const net_durum_t d = net_durum();
        if (d != onceki) {
            onceki = d;
            if (display_lock(100)) {
                if (d == NET_BAGLI) {
                    lv_label_set_text_fmt(s_bilgi, "%s\n%s\nIP %s",
                                          durum_metni(d), net_ssid(), net_ip());
                    lv_obj_set_style_text_color(s_bilgi, lv_color_hex(0x4CAF50),
                                                LV_PART_MAIN);
                } else if (d == NET_KAPALI) {
                    /* ADR-09: ayar yoksa kurulum ekranı gerekiyor. Dokunmatik
                     * klavye 3.5b'de gelecek; şimdilik durumu bildiriyoruz. */
                    lv_label_set_text(s_bilgi,
                                      "Wi-Fi ayari yok\nKurulum ekrani gerekiyor");
                    lv_obj_set_style_text_color(s_bilgi, lv_color_hex(0xF0B849),
                                                LV_PART_MAIN);
                } else {
                    lv_label_set_text(s_bilgi, durum_metni(d));
                    lv_obj_set_style_text_color(s_bilgi, lv_color_hex(0x8B93A7),
                                                LV_PART_MAIN);
                }
                display_unlock();
            }
            ESP_LOGI(TAG, "%s", durum_metni(d));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    /* NVS, ADR-09 gereği Wi-Fi bilgileri ve dokunmatik kalibrasyonu için
     * kullanılacak; Wi-Fi yığını da açılışta NVS bekler. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(display_init());

    /* display_selftest() yön sorununu çözmek için kullanıldı; çeyrek deseni
     * doğru ayarı (mirror y=true) tek bakışta gösterdi. Ekranla ilgili yeni
     * bir şüphede tek satırla geri açılabilir. */

    /* Dokunmatik — ekran kurulduktan sonra (kendi SPI veri yolu, SPI3). */
    if (touch_init() == ESP_OK) {
        if (!touch_kalibre_mi()) {
            ESP_LOGW(TAG, "dokunmatik kalibre degil, kalibrasyon ekrani");
            /* Ölçüm reddedilebiliyor (gürültü, çok kısa dokunma); tek denemede
             * bırakmak cihazı dokunmatiksiz açılmaya mahkûm ederdi. */
            for (int deneme = 0; deneme < 3 && !touch_kalibre_mi(); deneme++) {
                if (touch_kalibre_et() != ESP_OK) {
                    ESP_LOGW(TAG, "kalibrasyon basarisiz (deneme %d/3)", deneme + 1);
                }
            }
        }
    }

    build_probe_screen();

    /* --- 3.3 mikrofon doğrulaması ---
     * Kaydın çalıştığı gözle görülmüyor, o yüzden seviye ekrana çubuk olarak
     * basılıyor: sessizlikte düşük, konuşunca yükselen bir çubuk mikrofonun
     * duyduğunun tek pratik kanıtı. */
    if (audio_in_init() == ESP_OK) {
        mic_ekrani_kur();
        /* Hoparlör henüz yok (3.4 bekliyor); loopback döngüsü kapalı.
         * Mikrofon seviyesi 3.3'te doğrulandı, tekrar test edilmiyor.
         *
         * NOT: bu çağrı BİR KEZ yapılmalı. Bir ara yanlışlıkla iki kez
         * çağrıldı; ikincisi I2S1 zaten alındığı için "no available channel"
         * döndürdü, ESP_ERROR_CHECK paniğe düştü ve kart açılış döngüsüne
         * girdi. Seri log'da art arda gelen aynı hata, tek bir hatanın
         * tekrarı değil, her yeniden başlatmanın izidir. */
        ESP_ERROR_CHECK(audio_out_init());

        /* Hoparlör geldi — 3.4 doğrulaması iki aşamalı:
         *   1. ton: hoparlör yolunu mikrofondan BAĞIMSIZ sınar
         *   2. loopback: mikrofon -> PSRAM -> hoparlör zincirinin tamamı
         * Ton ayrı tutuluyor ki ses gelmezse suçlunun hangi taraf olduğu
         * belli olsun. */
        /* 3.4 doğrulaması: önce ton (hoparlör yolu), sonra loopback
         * (mikrofon -> PSRAM -> hoparlör zincirinin tamamı). */
        /* Mikrofonsuz çalma testi: ezgi temizse sorun mikrofonda,
         * cızırtılıysa çalma yolunda. Sonuç alınana kadar loopback kapalı. */
        /* Ses zinciri 3.3/3.4'te doğrulandı; loopback testi kaldırıldı.
         * Artık gerçek tur döngüsü çalışıyor (3.6). */
    } else {
        ESP_LOGE(TAG, "Mikrofon kurulamadi");
    }

    /* --- 3.5 ağ --- */
    ESP_ERROR_CHECK(net_init());

    /* --- 3.6 tur --- Wi-Fi bağlanmadan WebSocket açmanın anlamı yok. */
    int bekleme = 0;
    while (net_durum() != NET_BAGLI && bekleme < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        bekleme++;
    }
    if (net_durum() == NET_BAGLI) {
        /* Profil seçimi tur BAŞLAMADAN önce: tur_baslat() bas-konuş görevini
         * açıyor ve o andan itibaren öğrenci soru sorabiliyor. Seçim sonraya
         * kalsaydı ilk soru varsayılan derse gider, öğrenci de nedenini
         * anlamazdı. */
        if (!secim_yukle()) {
            ESP_LOGI(TAG, "kayitli profil yok, katalog aliniyor");
            if (katalog_getir() == ESP_OK) {
                secim_goster();
                katalog_serbest();   /* seçim yapıldı, JSON ağacına gerek yok */
            } else {
                /* Katalog alınamazsa tur yine de başlıyor: tur.c'deki
                 * varsayılan profille en azından cihaz denenebilir durumda
                 * kalıyor ve sorun logda görünüyor. */
                ESP_LOGE(TAG, "katalog alinamadi, varsayilan profille devam");
            }
        }
        ESP_LOGI(TAG, "Wi-Fi hazir, tur baslatiliyor");
        tur_baslat();
    } else {
        /* WI-FI BAGLANAMADIYSA KURULUM EKRANI ACILIYOR.
         *
         * Aksi halde cihaz acilis ekraninda takili kaliyor ve kurulum
         * ekranina ULASILAMIYOR: o ekran profil seridinden aciliyor, profil
         * seridi ise tur ekraninda -- tur ise Wi-Fi olmadan hic baslamiyor.
         * Yani tek bir yanlis parola cihazi kullanilamaz hale getiriyordu ve
         * tek care yeniden derlemekti. Demo gununde bu kabul edilemez. */
        /* Zaman asimi PES ETMEK degil, ekrana bir sey basmak icin.
         * kurulum_baglanti_bekle() beklemeyi surduruyor ve ag ortaya
         * cikinca kendiliginden donuyor; parola yalnizca kullanici
         * isterse soruluyor. */
        ESP_LOGW(TAG, "Wi-Fi henuz yok, bekleme ekrani");
        kurulum_baglanti_bekle();
        ESP_LOGI(TAG, "Wi-Fi hazir, tur baslatiliyor");
        tur_baslat();
    }

    /* V-05 bellek baskısı ölçülerek takip ediliyor: ses tamponları PSRAM'e
     * alınacak, ama LVGL ve TLS de aynı havuzdan besleniyor. Buradaki taban
     * değer, 3.3'te I2S tamponu eklendiğinde kıyas noktası olacak. */
    ESP_LOGI(TAG, "Bos yigin  dahili: %u bayt",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Bos yigin  PSRAM : %u bayt",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
