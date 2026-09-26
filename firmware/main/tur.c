/*
 * Tur durum makinesi ve ekran (plan 3.6).
 *
 * ADR-02 bas-konuş döngüsü:
 *
 *   BOSTA --(basıldı)--> KAYIT --(bırakıldı)--> DUSUNUYOR --> KONUSUYOR --> BOSTA
 *                                                    |             |
 *                                                    +--(basıldı)--+  iptal (ADR-04)
 *
 * Cihaz ince istemci: karar vermiyor, yalnızca ses gönderiyor ve gelen
 * olayları gösteriyor. Durum makinesi de bu yüzden ekranı ve mikrofonu
 * yönetmekten ibaret.
 */

#include "tur.h"

#include <string.h>

#include "audio_in.h"
#include "audio_out.h"
#include "display.h"
#include "katalog.h"
#include "kurulum.h"
#include "secim.h"
#include "yuz.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "pins.h"
#include "ws_client.h"

static const char *TAG = "tur";

/* Demo profili. 3.6b'de katalogdan seçilecek; şimdilik sabit.
 * Plan 0.4: dizeler payload'daki gibi BİREBİR olmalı, "8. Sınıf" değil. */
static char s_grade[24] = "8.Sınıf";
static char s_subject[64] = "Fen Bilimleri";

static lv_obj_t *s_durum_lbl;
static lv_obj_t *s_soru_lbl;
static lv_obj_t *s_cevap_lbl;
static lv_obj_t *s_kaynak_lbl;
static lv_obj_t *s_cevap_kap;
static lv_obj_t *s_profil_lbl;

/* Profil değiştirme isteği. Düğmenin geri çağrımı LVGL görevinde çalışıyor;
 * seçim ekranı ORADA açılamaz, çünkü secim_goster() seçim yapılana kadar
 * blokluyor ve seçimi veren de aynı LVGL görevi — kilitlenirdi. Bu yüzden
 * yalnızca bayrak konuyor, işi tur görevi yapıyor. */
static volatile bool s_profil_degistir;

/* Ekrandan bas-konuş (ADR-02). LVGL geri çağrımı yalnızca bayrağı çeviriyor;
 * kaydı başlatan iş tur görevinde, fiziksel düğmeyle aynı yerde. */
static volatile bool s_ekran_basili;

/* Eski turun geç gelen olaylarını yoksay.
 *
 * Araya girildiğinde (ADR-04) iptal backend'e ulaşana kadar ESKİ turun
 * mesajları gelmeye devam ediyor: ses paketleri, ardından cancelled ve done.
 * Kapı olmadan bunların HEPSİ arayüze yazıyordu — ses paketleri amfiyi
 * yeniden açıp durumu "Konuşuyor" yapıyor, cancelled "Kesildi", done
 * "Basili tutun" yazıyordu. Kullanıcı parmağını kaldırmamışken durum yazısı
 * bunların arasında gidip geliyordu.
 *
 * Yeni turun transcript'i geldiğinde kapı açılıyor — transcript ancak bizim
 * YENİ sesimiz yazıya çevrildikten sonra üretiliyor, dolayısıyla eski turun
 * paketleriyle karışması mümkün değil. */
static volatile bool s_eski_tur;

/* Kaynak satırı açık mı (dokununca tam metin görünüyor). */
static bool s_kaynak_acik;
static bool s_basili;

/* Çalmanın bu turda başlayıp başlamadığı.
 *
 * Dosya kapsamında ve turun SINIRLARINDA sıfırlanıyor. Önce cb_ses içinde
 * static olarak tutuluyordu ve "tur bitti mi" kontrolü de oradaydı; ama o
 * kontrol yalnızca ses geldiğinde çalışıyor. Tur bitince cb_ses bir daha
 * çağrılmadığı için bayrak true kalıyor, sonraki turda amfiyi açan satır
 * atlanıyor ve cihaz sessiz kalıyordu — ilk tur çalışıp sonrakiler sessizdi.
 * Bayrağın ömrü, onu değiştiren olayların yanında olmalı. */
static bool s_calma_basladi;
static int64_t s_kayit_basi;

#define EKRAN_G 240
#define KENAR   10

/* Boştaki durum yazısı aynı zamanda TALİMAT: cihazı ilk kez gören bir
 * öğrencinin ne yapacağını ekrandan anlaması gerekiyor. */
#define DURUM_BOSTA "Basili tutun"

static void cb_profil_dokun(lv_event_t *e)
{
    (void)e;
    s_profil_degistir = true;
}

/* Bas-konuş: basılı tutuldukça kayıt sürüyor.
 *
 * PRESS_LOST da bırakma sayılıyor: parmak alanın dışına kayarsa LVGL
 * RELEASED üretmiyor ve yalnızca RELEASED dinlense kayıt sonsuza kadar
 * açık kalırdı. */
static void cb_ptt(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_ekran_basili = true;
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        s_ekran_basili = false;
        break;
    default:
        break;
    }
}

/* Kaynak satırı tek satıra kısaltılıyor, ama breadcrumb'ın tamamı bazen
 * gerekiyor (öğretmen cevabın dayanağını görmek isteyebilir). Dokunma
 * açıp kapatıyor; akış yerleşimi sayesinde açılınca cevap alanı kendiliğinden
 * daralıyor, hiçbir şey üst üste binmiyor. */
static void cb_kaynak_dokun(lv_event_t *e)
{
    (void)e;
    if (s_kaynak_lbl == NULL) {
        return;
    }
    s_kaynak_acik = !s_kaynak_acik;
    if (s_kaynak_acik) {
        lv_label_set_long_mode(s_kaynak_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(s_kaynak_lbl, LV_SIZE_CONTENT);
    } else {
        lv_label_set_long_mode(s_kaynak_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_height(s_kaynak_lbl, 22);
    }
}

static void ekran_kur(void)
{
    if (!display_lock(3000)) {
        ESP_LOGE(TAG, "LVGL kilidi alinamadi, ekran kurulamadi");
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    /* Seçim listesi ekranı kaydırmış bırakmış olabilir; sıfırlanmazsa yeni
     * içerik görünür alanın dışında kalır ve ekran boş görünür. */
    lv_obj_scroll_to(scr, 0, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F1115), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* DİKEY AKIŞ — mutlak konum değil.
     *
     * Önceki sürüm her etiketi sabit bir y'ye koyuyordu. Soru iki satıra
     * taştığı anda cevabın, cevap uzadığında da kaynak satırının üstüne
     * biniyordu. Metnin boyu çalışma anında belli olduğu için sabit konum
     * bu ekranda baştan yanlış: yerleşimi LVGL'in flex akışı yapmalı. */
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, KENAR, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr, 6, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Aktif profil en üstte ve DOKUNULABİLİR. Göstermek tek başına değerli:
     * yanlış derste sorulan soru KAYNAK_YOK üretiyor ve öğrenci nedenini
     * anlamıyor — hangi derste olduğunu görmek bu şaşkınlığı önlüyor. */
    lv_obj_t *profil_btn = lv_button_create(scr);
    lv_obj_set_size(profil_btn, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(profil_btn, lv_color_hex(0x232833), LV_PART_MAIN);
    lv_obj_set_style_radius(profil_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(profil_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(profil_btn, cb_profil_dokun, LV_EVENT_CLICKED, NULL);

    s_profil_lbl = lv_label_create(profil_btn);
    lv_obj_set_style_text_font(s_profil_lbl, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_profil_lbl, lv_color_hex(0xB9C0CF), LV_PART_MAIN);
    lv_obj_center(s_profil_lbl);
    lv_label_set_text_fmt(s_profil_lbl, "%s - %s", s_grade, s_subject);

    /* BAS-KONUŞ ALANI (ADR-02). Yüz ve durum yazısı birlikte tek bir basılı
     * tutma hedefi oluşturuyor. Ayrı bir düğme koymak dikeyde ~44 piksel daha
     * yerdi ve cevaba kalan yer iki satıra inerdi; bu panel zaten orada
     * duruyordu, üstelik hedef büyük — parmakla ve küçük sınıflarda önemli.
     *
     * Ekranın TAMAMINI hedef yapmak da düşünülebilirdi ama cevap alanı
     * kaydırılabilir: kaydırmak için yapılan her dokunuş kaydı başlatırdı. */
    lv_obj_t *ptt = lv_obj_create(scr);
    lv_obj_set_width(ptt, LV_PCT(100));
    lv_obj_set_height(ptt, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ptt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ptt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(ptt, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ptt, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ptt, lv_color_hex(0x171A21), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ptt, lv_color_hex(0x2A303D), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(ptt, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ptt, 8, LV_PART_MAIN);
    lv_obj_remove_flag(ptt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ptt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ptt, cb_ptt, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ptt, cb_ptt, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(ptt, cb_ptt, LV_EVENT_PRESS_LOST, NULL);

    yuz_olustur(ptt);

    s_durum_lbl = lv_label_create(ptt);
    lv_label_set_text(s_durum_lbl, DURUM_BOSTA);
    lv_obj_set_style_text_font(s_durum_lbl, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_durum_lbl, lv_color_hex(0x8B93A7), LV_PART_MAIN);

    /* Soru en fazla iki satır, taşan kısım "..." ile kısalıyor. Öğrenci
     * soruyu az önce kendisi sordu; burada amaç STT'nin ne duyduğunu
     * göstermek, tam metni okutmak değil. Sınırsız bırakmak cevaba
     * ayrılan yeri yerdi. */
    s_soru_lbl = lv_label_create(scr);
    lv_label_set_text(s_soru_lbl, "");
    lv_label_set_long_mode(s_soru_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_soru_lbl, LV_PCT(100));
    lv_obj_set_height(s_soru_lbl, 44);
    lv_obj_set_style_text_font(s_soru_lbl, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_soru_lbl, lv_color_hex(0xE6E8EE), LV_PART_MAIN);

    /* Cevap kalan yerin TAMAMINI alıyor ve kendi içinde kayıyor. Cevabın
     * uzunluğu önceden bilinmiyor; taşan metni kırpmak yerine kaydırmak,
     * uzun cevaplarda bilginin kaybolmamasını sağlıyor. */
    s_cevap_kap = lv_obj_create(scr);
    lv_obj_set_width(s_cevap_kap, LV_PCT(100));
    lv_obj_set_flex_grow(s_cevap_kap, 1);
    lv_obj_set_style_bg_opa(s_cevap_kap, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cevap_kap, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_cevap_kap, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_cevap_kap, LV_DIR_VER);

    s_cevap_lbl = lv_label_create(s_cevap_kap);
    lv_label_set_text(s_cevap_lbl, "");
    lv_label_set_long_mode(s_cevap_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_cevap_lbl, LV_PCT(100));
    lv_obj_set_style_text_font(s_cevap_lbl, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_cevap_lbl, lv_color_hex(0x4C8DFF), LV_PART_MAIN);

    /* Kaynak gösterimi doküman 3.2'nin şartı: cevap hangi ünitedendi.
     * Öğretmen/veli için cevabın dayanağını görünür kılıyor. Tek satır:
     * breadcrumb uzun olabiliyor ve cevabın yerini yemesi kabul edilemez. */
    s_kaynak_lbl = lv_label_create(scr);
    lv_label_set_text(s_kaynak_lbl, "");
    lv_label_set_long_mode(s_kaynak_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_kaynak_lbl, LV_PCT(100));
    lv_obj_set_height(s_kaynak_lbl, 22);
    lv_obj_set_style_text_font(s_kaynak_lbl, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_kaynak_lbl, lv_color_hex(0x6B7280), LV_PART_MAIN);
    /* Etiketler varsayılan olarak tıklanabilir değil; açıkça verilmesi
     * gerekiyor. */
    lv_obj_add_flag(s_kaynak_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_kaynak_lbl, cb_kaynak_dokun, LV_EVENT_CLICKED, NULL);
    s_kaynak_acik = false;

    display_unlock();
    yuz_durum_ayarla(YUZ_HAZIR);
}

/* Yüz durumu ayrı bir çağrıyla değil, yazıyla BİRLİKTE veriliyor. Ayrı
 * olsaydı ikisi zamanla birbirinden kopar ve ekran "Hazir" yazarken yüz
 * konuşuyor görünürdü — bu tür tutarsızlık, sebebi en geç fark edilen
 * hatalardan. */
static void durum_yaz(const char *metin, uint32_t renk, yuz_durum_t yuz)
{
    yuz_durum_ayarla(yuz);
    if (display_lock(50)) {
        /* Seçim ekranı açıkken tur ekranı yok edilmiş oluyor ve işaretçiler
         * NULL'a çekiliyor; o sırada gelen bir WebSocket olayı buraya
         * düşerse yazılacak etiket de yok. */
        if (s_durum_lbl == NULL) {
            display_unlock();
            return;
        }
        lv_label_set_text(s_durum_lbl, metin);
        lv_obj_set_style_text_color(s_durum_lbl, lv_color_hex(renk), LV_PART_MAIN);
        display_unlock();
    }
}

/* --- WebSocket geri çağırmaları --- */

static void cb_transcript(const char *metin)
{
    ESP_LOGI(TAG, "soru: %s", metin);
    /* Bu transcript yeni turun; buradan sonra gelen ses artık geçerli. */
    s_eski_tur = false;
    if (display_lock(100)) {
        if (s_soru_lbl == NULL) {
            display_unlock();
            return;
        }
        lv_label_set_text(s_soru_lbl, metin);
        lv_label_set_text(s_cevap_lbl, "");
        lv_label_set_text(s_kaynak_lbl, "");
        lv_obj_scroll_to_y(s_cevap_kap, 0, LV_ANIM_OFF);
        /* Önceki cevabın kaynağı açık bırakılmışsa kapansın; yeni cevaba
         * yer açıyor ve açık kalan satır artık başka bir kaynağı gösterirdi. */
        if (s_kaynak_acik) {
            s_kaynak_acik = false;
            lv_label_set_long_mode(s_kaynak_lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_height(s_kaynak_lbl, 22);
        }
        display_unlock();
    }
}

static void cb_dusunuyor(void)
{
    durum_yaz("Düşünüyor...", 0xF0B849, YUZ_DUSUNUYOR);
}

static void cb_cevap(const char *metin, const char *kaynak, bool kaynak_yok)
{
    if (display_lock(100)) {
        if (s_cevap_lbl == NULL) {
            display_unlock();
            return;
        }
        lv_label_set_text(s_cevap_lbl, metin);
        lv_obj_set_style_text_color(s_cevap_lbl,
                                    lv_color_hex(kaynak_yok ? 0xF0B849 : 0x4C8DFF),
                                    LV_PART_MAIN);
        /* Cevap parça parça akıyor; kap sabit kalsaydı öğrenci ilk satırlara
         * bakarken metnin gerisi görünmez olurdu. En alta kaydırmak, akan
         * metnin son halini her zaman görünür tutuyor. */
        lv_obj_update_layout(s_cevap_kap);
        lv_obj_scroll_by(s_cevap_kap, 0, -lv_obj_get_scroll_bottom(s_cevap_kap),
                         LV_ANIM_OFF);
        if (kaynak != NULL && kaynak[0] != '\0') {
            lv_label_set_text(s_kaynak_lbl, kaynak);
        }
        display_unlock();
    }
}

static void cb_ses(const uint8_t *veri, size_t bayt)
{
    if (s_eski_tur) {
        return;   /* iptal edilmiş turun artığı */
    }
    /* Backend 24 kHz ham PCM gönderiyor (settings.tts_sample_rate).
     * Kayıt 16 kHz olduğu için hız her çalmadan önce ayarlanmalı. */
    if (!s_calma_basladi) {
        s_calma_basladi = true;
        audio_out_set_rate(SPK_SAMPLE_RATE);
        audio_out_hp_sifirla();
        audio_out_enable(true);
        durum_yaz("Konuşuyor", 0x4CAF50, YUZ_KONUSUYOR);
    }
    audio_out_write(veri, bayt);
}

static void cb_bitti(void)
{
    s_calma_basladi = false;
    audio_out_flush();
    if (!s_eski_tur) {
        durum_yaz(DURUM_BOSTA, 0x8B93A7, YUZ_HAZIR);
    }
}

static void cb_iptal(void)
{
    s_calma_basladi = false;
    /* ADR-04: iptalden ÖNCE gönderilmiş baytlar hâlâ yolda olabilir.
     * Amfiyi anında kapatmak, tampondaki sesin bitmesini beklemeden
     * susturmanın en kesin yolu — SD pininin GPIO'ya bağlanma sebebi bu. */
    audio_out_enable(false);
    /* Susturma her koşulda yapılıyor; yazan yalnızca GÜNCEL tur.
     * Araya girme sırasında bu olay zaten iptal ettiğimiz turun cevabı ve
     * kullanıcı o an konuşuyor — ekrana "Kesildi" yazmak yanlış olurdu. */
    if (!s_eski_tur) {
        durum_yaz("Kesildi", 0x8B93A7, YUZ_HAZIR);
    }
}

static void cb_hata(const char *kod, const char *mesaj)
{
    s_calma_basladi = false;
    audio_out_enable(false);
    char buf[160];
    snprintf(buf, sizeof(buf), "Hata: %s", mesaj);
    durum_yaz(buf, 0xFF6B6B, YUZ_HAZIR);
    ESP_LOGE(TAG, "%s: %s", kod, mesaj);
}

/* Yakalama ve gönderme AYRI görevlerde.
 *
 * Önce ikisi tek döngüdeydi: audio_in_pump() ile okuyup hemen
 * ws_ses_gonder() ile yolluyorduk. Ağ gönderimi 2 sn'ye kadar bloklayabilir
 * ve o sırada I2S DMA tamponu taşarsa ÖRNEKLER DÜŞER. Loopback testinde bu
 * görünmüyordu çünkü ağ yoktu; 3.6'da STT'nin soruyu yanlış duymasının
 * muhtemel sebebi bu.
 *
 * Yakalama görevi daha yüksek öncelikte ve yalnızca I2S okuyor; gönderme
 * görevi tamponda biriken yeni baytları ağa veriyor. Ağ yavaşlarsa kayıt
 * değil gönderim gecikir — ses bütünlüğü korunur. */
static volatile size_t s_gonderilen;
static volatile bool s_yakala;

static void yakalama_gorevi(void *arg)
{
    (void)arg;
    while (true) {
        if (s_yakala) {
            if (audio_in_pump() == 0) {
                vTaskDelay(1);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/* --- Bas-konuş görevi --- */

static void tur_gorevi(void *arg)
{
    (void)arg;

    while (true) {
        /* Profil değiştirme isteği. Kayıt sürerken ele alınmıyor: ekranı
         * ortasında yıkmak kaydı yarıda keserdi. */
        if (s_profil_degistir && !s_basili) {
            s_profil_degistir = false;

            if (ws_tur_durum() == TUR_KONUSUYOR) {
                ws_iptal();
                audio_out_enable(false);
            }

            /* Etiketler seçim ekranı tarafından yok edilecek; işaretçileri
             * ÖNCEDEN NULL'a çekiyoruz. Aksi halde bu sırada gelen bir
             * WebSocket olayı silinmiş bir nesneye yazar. */
            ESP_LOGI(TAG, "profil degistir: basladi (dahili yigin %u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

            if (ws_tur_durum() == TUR_KONUSUYOR) {
                ws_iptal();
                audio_out_enable(false);
            }

            /* Etiketler menu/secim ekrani tarafindan yok edilecek;
             * isaretcileri ONCEDEN NULL'a cekiyoruz. Aksi halde bu sirada
             * gelen bir WebSocket olayi silinmis bir nesneye yazar. */
            if (display_lock(2000)) {
                yuz_birak();
                lv_obj_clean(lv_screen_active());
                s_profil_lbl = NULL;
                s_durum_lbl = NULL;
                s_soru_lbl = NULL;
                s_cevap_lbl = NULL;
                s_cevap_kap = NULL;
                s_kaynak_lbl = NULL;
                display_unlock();
                ESP_LOGI(TAG, "profil degistir: ekran yikildi");
            } else {
                ESP_LOGE(TAG, "profil degistir: LVGL kilidi alinamadi");
            }

            /* MENU ONCE, KATALOG SONRA.
             *
             * Katalogla baslamak, ayarlari calisan bir backend'e bagimli
             * kiliyordu: adres yanlissa katalog dusuyor ve kullanici ayarlar
             * girisini HIC goremiyordu -- tam da duzeltmesi gereken ani.
             * Menu agdan bagimsiz aciliyor; katalog yalnizca ders degistirme
             * secilirse isteniyor. */
            const secim_menu_t se = secim_menu();
            const char *hata = NULL;

            if (se == SECIM_MENU_AYARLAR) {
                kurulum_goster();   /* kaydedilirse cihaz yeniden baslar */
            } else if (se == SECIM_MENU_DERS) {
                const esp_err_t kr = katalog_getir();
                ESP_LOGI(TAG, "profil degistir: katalog %s (dahili yigin %u)",
                         esp_err_to_name(kr),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                if (kr == ESP_OK) {
                    const esp_err_t sr = secim_goster();
                    ESP_LOGI(TAG, "profil degistir: secim %s",
                             esp_err_to_name(sr));
                    katalog_serbest();
                } else {
                    /* Tur ekrani geri kurulduktan SONRA yaziliyor; simdi
                     * yazmak silinecek bir etikete yazmak olurdu. */
                    hata = "Katalog alinamadi";
                }
            }

            ekran_kur();
            if (hata != NULL) {
                durum_yaz(hata, 0xFF6B6B, YUZ_HAZIR);
            }
            ESP_LOGI(TAG, "profil degistir: ekran yeniden kuruldu");
            continue;
        }

        /* Ekran birincil giriş (ADR-02); kart üstündeki BOOT düğmesi yedek
         * olarak duruyor. Dokunmatik bir sorun çıkarırsa cihaz tamamen
         * kullanılamaz hale gelmiyor — R-03'ün adım adım doğrulama
         * yaklaşımının donanımdaki karşılığı. */
        const bool basili = s_ekran_basili || (gpio_get_level(PIN_PTT_BUTTON) == 0);

        if (basili && !s_basili) {
            s_basili = true;
            if (ws_durum() != WS_BAGLI) {
                durum_yaz("Backend'e baglanilamadi", 0xFF6B6B, YUZ_HAZIR);
            } else {
                /* Cevap çalınırken basıldıysa bu bir araya girmedir. */
                if (ws_tur_durum() == TUR_KONUSUYOR) {
                    ws_iptal();
                    audio_out_enable(false);
                }
                /* Bu andan sonra gelen her ses paketi ESKİ tura ait;
                 * yeni turun transcript'i gelene kadar hepsi atılıyor. */
                s_eski_tur = true;
                s_calma_basladi = false;
                s_gonderilen = 0;
                ws_konusma_basla(s_grade, s_subject);
                audio_in_start();
                s_yakala = true;
                s_kayit_basi = esp_timer_get_time();
                durum_yaz("Dinliyor...", 0xE0483D, YUZ_DINLIYOR);
            }
        } else if (!basili && s_basili) {
            s_basili = false;
            s_yakala = false;

            /* Kalan baytları gönder — bunlar sorunun sonu, atlanırsa cümle
             * yarım gider ve STT yanlış duyar. */
            const size_t simdi = audio_in_size();
            if (simdi > s_gonderilen) {
                ws_ses_gonder(audio_in_data() + s_gonderilen, simdi - s_gonderilen);
                s_gonderilen = simdi;
            }
            audio_in_stop();
            ws_konusma_bitir();

            /* GERÇEK yakalama hızı: 16 kHz 16-bit mono = 32000 bayt/sn
             * olmalı. Belirgin düşükse örnek DÜŞÜYOR demektir ve STT'nin
             * yanlış duymasının sebebi budur. */
            const int64_t sure_us = esp_timer_get_time() - s_kayit_basi;
            const int hiz = (int)((int64_t)simdi * 1000000 / (sure_us ? sure_us : 1));
            ESP_LOGW(TAG, "kayit %u bayt / %d ms -> %d bayt/sn (beklenen 32000)",
                     (unsigned)simdi, (int)(sure_us / 1000), hiz);

            durum_yaz("Düşünüyor...", 0xF0B849, YUZ_DUSUNUYOR);
        }

        /* Tamponda biriken yeni baytları gönder. Okuma ayrı görevde olduğu
         * için buradaki gecikme örnek kaybettirmiyor. */
        if (s_basili) {
            const size_t simdi = audio_in_size();
            if (simdi > s_gonderilen) {
                const size_t n = simdi - s_gonderilen;
                if (ws_ses_gonder(audio_in_data() + s_gonderilen, n) == ESP_OK) {
                    s_gonderilen = simdi;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

esp_err_t tur_baslat(void)
{
    const gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_PTT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    ekran_kur();

    static const ws_geri_cagirma_t cb = {
        .transcript = cb_transcript,
        .dusunuyor = cb_dusunuyor,
        .cevap_metni = cb_cevap,
        .ses = cb_ses,
        .bitti = cb_bitti,
        .hata = cb_hata,
        .iptal = cb_iptal,
    };
    const esp_err_t err = ws_baslat(net_backend(), &cb);
    if (err != ESP_OK) {
        durum_yaz("Backend adresi yok", 0xFF6B6B, YUZ_HAZIR);
        return err;
    }

    /* Yakalama önceliği daha YÜKSEK: ağ yavaşlasa da I2S okuması aksamamalı. */
    xTaskCreate(yakalama_gorevi, "yakala", 3072, NULL, 6, NULL);
    xTaskCreate(tur_gorevi, "tur", 4096, NULL, 5, NULL);
    return ESP_OK;
}

void tur_profil_ayarla(const char *grade, const char *subject)
{
    snprintf(s_grade, sizeof(s_grade), "%s", grade);
    snprintf(s_subject, sizeof(s_subject), "%s", subject);

    /* Ekran henüz kurulmamış olabilir (ilk açılışta tur_baslat'tan önce
     * çağrılıyor) ya da seçim ekranı yüzünden yok edilmiş olabilir. */
    if (display_lock(100)) {
        if (s_profil_lbl != NULL) {
            lv_label_set_text_fmt(s_profil_lbl, "%s - %s", s_grade, s_subject);
        }
        display_unlock();
    }
}
