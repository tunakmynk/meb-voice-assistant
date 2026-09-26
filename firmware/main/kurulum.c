/*
 * Wi-Fi + backend kurulum ekranı (plan 3.5b, ADR-09).
 *
 * ============================================================
 * Neden SSID listeden seçiliyor
 * ============================================================
 * Dokunmatik klavyede SSID yazmak hem yavaş hem de hataya açık. Yanlış
 * yazılan bir SSID, demo günü teşhis edilmesi en zor hatalardan biri
 * olurdu: cihaz sessizce bağlanamaz, sebebi hiçbir yerde görünmez.
 * Taramadan gelen listeden seçmek bu hatayı tamamen ortadan kaldırıyor.
 *
 * Parola ve backend adresi elle giriliyor — ikisi de tahmin edilemez.
 *
 * ============================================================
 * Klavye fontu neden ayrı
 * ============================================================
 * lv_keyboard'un varsayılan tuş haritası LVGL sembollerini kullanıyor
 * (geri sil, enter, büyük harf). Bu semboller Montserrat'ın içinde;
 * gömdüğümüz DejaVu TTF'te yok. Klavye varsayılan fontla, metin kutusu
 * Türkçe fontla çiziliyor — böylece hem tuşlar hem de listeden gelen
 * Türkçe karakterli SSID doğru görünüyor.
 */

#include "kurulum.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "display.h"
#include "net.h"

static const char *TAG = "kurulum";

#define EKRAN_G 240
#define EKRAN_Y 320

#define GERI (-1)

#define PAROLA_AZAMI 65
#define BACKEND_AZAMI 128

static SemaphoreHandle_t s_bekle;
static volatile int s_secim;
static lv_obj_t *s_ta;

/* Ekranı, önceki sahibinin bıraktığı düzenden arındırır.
 *
 * lv_obj_clean() yalnızca çocukları siliyor; ekran nesnesinin kendi
 * biçemleri kalıyor. Tur ekranı flex düzeni bırakıyor ve bunun üstüne
 * kurulan her ekranın yerleşimi bozuluyor — siyah ekran hatası tam olarak
 * buydu (plan 3.6b). */
static lv_obj_t *ekran_sifirla(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_layout(scr, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr, 0, LV_PART_MAIN);
    lv_obj_scroll_to(scr, 0, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F1115), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    return scr;
}

static void satir_cb(lv_event_t *e)
{
    s_secim = (int)(intptr_t)lv_event_get_user_data(e);
    xSemaphoreGive(s_bekle);
}

/* --- 1. adım: ağ seçimi --- */

static int ag_sec(char ssidler[][NET_SSID_AZAMI])
{
    /* Tarama sırasında ekranda bir şey olmalı: ~2 saniye boyunca donmuş
     * görünen bir cihaz, bozulduğu izlenimi verir. */
    if (display_lock(3000)) {
        lv_obj_t *scr = ekran_sifirla();
        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text(l, "Aglar taraniyor...");
        lv_obj_set_style_text_font(l, display_font_tr(), LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(0xE6E8EE), LV_PART_MAIN);
        lv_obj_center(l);
        display_unlock();
    }

    const int adet = net_tara(ssidler, NET_AZAMI_AG);
    ESP_LOGI(TAG, "%d ag listelenecek", adet);

    if (!display_lock(3000)) {
        return GERI;
    }
    lv_obj_t *scr = ekran_sifirla();

    lv_obj_t *liste = lv_list_create(scr);
    lv_obj_set_size(liste, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_text_font(liste, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(liste, lv_color_hex(0x171A21), LV_PART_MAIN);
    lv_obj_set_style_border_width(liste, 0, LV_PART_MAIN);

    lv_list_add_text(liste, adet > 0 ? "Wi-Fi agi secin" : "Ag bulunamadi");
    xSemaphoreTake(s_bekle, 0);   /* bekleyen sahte sinyali temizle */

    for (int i = 0; i < adet; i++) {
        lv_obj_t *b = lv_list_add_button(liste, NULL, ssidler[i]);
        lv_obj_add_event_cb(b, satir_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    /* Yeniden tarama şart: hotspot cihaz açıldıktan SONRA açılmış olabilir
     * ve o zaman ilk taramada görünmez. Yeniden başlatmak tek çare olsaydı
     * demo günü panik anı yaratırdı. */
    lv_obj_t *y = lv_list_add_button(liste, NULL, "Yeniden tara");
    lv_obj_add_event_cb(y, satir_cb, LV_EVENT_CLICKED, (void *)(intptr_t)GERI);
    display_unlock();

    xSemaphoreTake(s_bekle, portMAX_DELAY);
    return s_secim;
}

/* --- 2. adım: metin girişi --- */

static void kb_cb(lv_event_t *e)
{
    s_secim = (lv_event_get_code(e) == LV_EVENT_READY) ? 0 : GERI;
    xSemaphoreGive(s_bekle);
}

/* Metin kutusu + klavye. true dönerse `cikti` doldurulmuştur. */
static bool metin_al(const char *baslik, const char *mevcut, bool parola,
                     char *cikti, size_t n)
{
    if (!display_lock(3000)) {
        return false;
    }
    lv_obj_t *scr = ekran_sifirla();

    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, baslik);
    lv_obj_set_style_text_font(l, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_pos(l, 8, 6);

    s_ta = lv_textarea_create(scr);
    lv_obj_set_size(s_ta, EKRAN_G - 16, 52);
    lv_obj_set_pos(s_ta, 8, 28);
    lv_textarea_set_one_line(s_ta, true);
    /* Parola GIZLENMIYOR (parametre yok sayiliyor).
     *
     * Gizli alan, dokunmatik klavyede yapilan bir yazim hatasini
     * gorunmez kiliyor ve sonuc yalnizca "baglanamadi" olarak
     * cikiyor -- sebep kodu 15 (4WAY_HANDSHAKE_TIMEOUT) tam olarak
     * boyle bir hatada gelmisti. Cihazi sahibi kuruyor ve ekrana
     * bakan baska kimse yok; parolayi gormek, hatayi bulmaktan
     * cok daha degerli.
     *
     * Klavye kucuk harfle aciliyor: parolada BUYUK harf ya da rakam
     * varsa mod degistirmek gerekiyor ve bu, gozle dogrulanmadan
     * fark edilmesi imkansiz bir hata kaynagi. */
    (void)parola;
    lv_textarea_set_password_mode(s_ta, false);
    lv_textarea_set_text(s_ta, mevcut ? mevcut : "");
    /* Listeden gelen SSID'de Türkçe karakter olabilir; metin kutusu Türkçe
     * fontla çiziliyor. */
    lv_obj_set_style_text_font(s_ta, display_font_tr(), LV_PART_MAIN);

    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_obj_set_size(kb, EKRAN_G, 190);
    lv_obj_set_pos(kb, 0, EKRAN_Y - 190);
    lv_keyboard_set_textarea(kb, s_ta);
    /* Klavye VARSAYILAN fontta kalıyor — sembol tuşları (geri sil, enter)
     * yalnızca orada var. */
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_CANCEL, NULL);

    xSemaphoreTake(s_bekle, 0);
    display_unlock();

    xSemaphoreTake(s_bekle, portMAX_DELAY);

    bool tamam = false;
    if (display_lock(3000)) {
        if (s_secim == 0) {
            snprintf(cikti, n, "%s", lv_textarea_get_text(s_ta));
            tamam = true;
        }
        display_unlock();
    }
    s_ta = NULL;
    return tamam;
}

/* --- Akış --- */

esp_err_t kurulum_goster(void)
{
    if (s_bekle == NULL) {
        s_bekle = xSemaphoreCreateBinary();
        if (s_bekle == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* 20 x 33 bayt = 660 bayt; yığında tutmak yerine statik, çünkü bu
     * yordam kurulum sırasında bir kez çalışıyor ve görev yığını dar. */
    static char ssidler[NET_AZAMI_AG][NET_SSID_AZAMI];

    int secilen = GERI;
    while (secilen == GERI) {
        secilen = ag_sec(ssidler);   /* GERI = yeniden tara */
    }

    char parola[PAROLA_AZAMI] = "";
    if (!metin_al("Wi-Fi parolasi", "", true, parola, sizeof(parola))) {
        net_tarama_bitir();   /* vazgecildi: eski aga geri don */
        return ESP_ERR_NOT_FINISHED;
    }

    /* Backend adresi mevcut değerle dolu geliyor: demo günü genelde yalnızca
     * IP'nin son bölümü değişiyor ve tamamını yeniden yazdırmak gereksiz
     * bir hata kaynağı olurdu. */
    char backend[BACKEND_AZAMI] = "";
    if (!metin_al("Backend adresi", net_backend(), false,
                  backend, sizeof(backend))) {
        net_tarama_bitir();   /* vazgecildi: eski aga geri don */
        return ESP_ERR_NOT_FINISHED;
    }

    ESP_LOGW(TAG, "kaydediliyor: %s / %s", ssidler[secilen], backend);

    const esp_err_t sonuc = net_ayarla(ssidler[secilen], parola, backend);
    if (sonuc != ESP_OK) {
        ESP_LOGE(TAG, "ayarlar kaydedilemedi: %s", esp_err_to_name(sonuc));
        return sonuc;
    }

    /* AYARLAR NVS'E YAZILDI, ŞİMDİ YENİDEN BAŞLATILIYOR.
     *
     * Backend adresi ws_baslat()'a yalnızca AÇILIŞTA veriliyor; çalışan
     * WebSocket istemcisi eski adrese bağlanmaya devam ederdi. Adresi
     * çalışma anında değiştiren bir yol yazmak mümkün ama yeni ve az
     * denenmiş olurdu. Yeniden başlatmak, defalarca doğrulanmış tek bir
     * yolu (açılış) kullanıyor ve 3.5 saniye sürüyor — demo gününde doğru
     * takas bu. */
    if (display_lock(3000)) {
        lv_obj_t *scr = ekran_sifirla();
        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text(l, "Kaydedildi.\nYeniden baslatiliyor...");
        lv_obj_set_style_text_font(l, display_font_tr(), LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(0x35C46A), LV_PART_MAIN);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_center(l);
        display_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(1200));   /* yazı okunabilsin */
    esp_restart();
    return ESP_OK;   /* buraya gelinmiyor */
}

/* --- Baglanti bekleme --- */

static volatile bool s_ayar_istendi;

static void ayar_dugmesi_cb(lv_event_t *e)
{
    (void)e;
    s_ayar_istendi = true;
}

static void bekleme_ekrani(void)
{
    if (!display_lock(3000)) {
        return;
    }
    lv_obj_t *scr = ekran_sifirla();

    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text_fmt(l, "Wi-Fi araniyor...\n%s", net_ssid());
    lv_obj_set_style_text_font(l, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE6E8EE), LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, EKRAN_G - 32);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *ipucu = lv_label_create(scr);
    lv_label_set_text(ipucu, "Hotspot acik mi?");
    lv_obj_set_style_text_font(ipucu, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(ipucu, lv_color_hex(0x8B93A7), LV_PART_MAIN);
    lv_obj_align(ipucu, LV_ALIGN_CENTER, 0, 4);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, EKRAN_G - 60, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x232833), LV_PART_MAIN);
    lv_obj_add_event_cb(btn, ayar_dugmesi_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Ayarlari degistir");
    lv_obj_set_style_text_font(bl, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xB9C0CF), LV_PART_MAIN);
    lv_obj_center(bl);

    display_unlock();
}

void kurulum_baglanti_bekle(void)
{
    /* Hic ayar yoksa beklemenin anlami yok: dogrudan kuruluma. */
    if (!net_ayar_var()) {
        while (net_durum() != NET_BAGLI) {
            kurulum_goster();
        }
        return;
    }

    s_ayar_istendi = false;
    bekleme_ekrani();

    while (net_durum() != NET_BAGLI) {
        if (s_ayar_istendi) {
            s_ayar_istendi = false;
            /* Kaydedilirse cihaz yeniden baslar; vazgecilirse buraya doner
             * ve arka plandaki yeniden deneme kaldigi yerden surer. */
            kurulum_goster();
            bekleme_ekrani();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "baglandi, tur baslatilabilir");
}
