/*
 * Sınıf/ders seçim ekranı (plan 3.6b).
 *
 * ============================================================
 * Neden liste, neden iki adım
 * ============================================================
 * Katalog sınıf -> ders[] eşlemesi; her sınıfta farklı dersler indekslenmiş
 * olabiliyor. Tek ekranda sınıf ve ders yan yana gösterilseydi, karşılığı
 * olmayan bir bileşimin seçilmesi mümkün olurdu ve sonuç sessizce boş arama
 * olurdu. Önce sınıf, sonra O SINIFIN dersleri — geçersiz bileşim
 * kurulamıyor.
 *
 * Ekran 240x320 ve parmakla kullanılıyor; liste satırları LVGL'in kendi
 * kaydırmasıyla geliyor, ayrı bir sayfalama gerekmiyor.
 */

#include "secim.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "display.h"
#include "katalog.h"
#include "kurulum.h"
#include "tur.h"

static const char *TAG = "secim";

#define NVS_AD "meb"
#define ANAHTAR_SINIF "prf_sinif"
#define ANAHTAR_DERS "prf_ders"

/* tur.c'deki tamponlarla aynı sınırlar. */
#define SINIF_AZAMI 24
#define DERS_AZAMI 64

/* Liste satırları için, gerçek dizinlerle karışmayacak değerler. */
#define GERI (-1)
#define AYARLAR (-2)

static SemaphoreHandle_t s_bekle;
static volatile int s_secim;

/* ---------------- NVS ---------------- */

static bool nvs_oku(char *sinif, size_t sn, char *ders, size_t dn)
{
    nvs_handle_t h;
    if (nvs_open(NVS_AD, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t a = sn, b = dn;
    const bool tamam = nvs_get_str(h, ANAHTAR_SINIF, sinif, &a) == ESP_OK
                    && nvs_get_str(h, ANAHTAR_DERS, ders, &b) == ESP_OK;
    nvs_close(h);
    return tamam;
}

static void nvs_yaz(const char *sinif, const char *ders)
{
    nvs_handle_t h;
    if (nvs_open(NVS_AD, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS acilamadi, secim kaydedilmedi");
        return;
    }
    nvs_set_str(h, ANAHTAR_SINIF, sinif);
    nvs_set_str(h, ANAHTAR_DERS, ders);
    nvs_commit(h);
    nvs_close(h);
}

bool secim_yukle(void)
{
    char sinif[SINIF_AZAMI], ders[DERS_AZAMI];
    if (!nvs_oku(sinif, sizeof(sinif), ders, sizeof(ders))) {
        return false;
    }
    ESP_LOGI(TAG, "kayitli profil: %s / %s", sinif, ders);
    tur_profil_ayarla(sinif, ders);
    return true;
}

/* ---------------- Liste ekranı ---------------- */

static void satir_cb(lv_event_t *e)
{
    s_secim = (int)(intptr_t)lv_event_get_user_data(e);
    xSemaphoreGive(s_bekle);
}

/* Başlık + satırlardan liste kurar ve seçim yapılana kadar bekler.
 * geri_var: ilk satıra "Geri" konur (yanlış sınıf seçildiğinde tek çıkış
 * yolunun yeniden başlatmak olması kabul edilemezdi). */
/* ek_satir: listenin başına konan fazladan satır (NULL ise yok).
 * Sınıf listesinde "Ayarlar", ders listesinde "< Geri" oluyor — ikisi de
 * listeden çıkışın tek yolu, yoksa kullanıcı yeniden başlatmak zorunda
 * kalırdı. */
static int liste_sor(const char *baslik, int adet,
                     const char *(*metin)(int),
                     const char *ek_satir, int ek_deger)
{
    /* Süresiz bekleme yok. LVGL görevi bir kez takıldığında (bellek biterse
     * çizim döngüsü ilerleyemiyor) burada sonsuza kadar beklemek, ekranı
     * siyah bırakıp hiçbir iz üretmiyordu. */
    if (!display_lock(3000)) {
        ESP_LOGE(TAG, "LVGL kilidi alinamadi, liste gosterilemiyor");
        return GERI;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    /* EKRANIN KENDİ BİÇEMİ SIFIRLANIYOR.
     *
     * lv_obj_clean() yalnızca ÇOCUKLARI siliyor; ekran nesnesinin üzerindeki
     * yerel biçemler kalıyor. Tur ekranı (ekran_kur) ekrana flex düzeni ve
     * dolgu bırakıyor; seçim listesi bunların üstüne kurulunca yerleşimi
     * bozuluyor. İlk açılışta sorun çıkmamasının sebebi de bu: o sırada
     * ekran_kur henüz çalışmamış, ekran temizdi. Yani hata seçim ekranında
     * değil, ekranın ÖNCEKİ sahibinden devraldığı durumdaydı. */
    lv_obj_set_layout(scr, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr, 0, LV_PART_MAIN);
    lv_obj_scroll_to(scr, 0, 0, LV_ANIM_OFF);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F1115), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *liste = lv_list_create(scr);
    lv_obj_set_size(liste, LV_PCT(100), LV_PCT(100));
    lv_obj_center(liste);
    /* Yazı biçimi kalıtımla iniyor; her satıra ayrı ayrı vermek gerekmiyor.
     * Türkçe font şart — varsayılan LVGL fontunda ş/ğ/ı yok. */
    lv_obj_set_style_text_font(liste, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(liste, lv_color_hex(0x171A21), LV_PART_MAIN);
    lv_obj_set_style_border_width(liste, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(liste, 2, LV_PART_MAIN);

    lv_list_add_text(liste, baslik);

    /* Önceki listede çift dokunma olduysa semafor sinyalli kalmış olabilir;
     * temizlenmezse bu liste hiç gösterilmeden, eski bir dizinle geçilir. */
    xSemaphoreTake(s_bekle, 0);

    if (ek_satir != NULL) {
        lv_obj_t *b = lv_list_add_button(liste, NULL, ek_satir);
        lv_obj_add_event_cb(b, satir_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)ek_deger);
    }
    for (int i = 0; i < adet; i++) {
        const char *m = metin(i);
        if (m == NULL) {
            continue;
        }
        lv_obj_t *b = lv_list_add_button(liste, NULL, m);
        lv_obj_add_event_cb(b, satir_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    display_unlock();
    ESP_LOGI(TAG, "liste kuruldu: %s, %d satir", baslik, adet);

    /* Kilit BIRAKILDIKTAN sonra bekleniyor: geri çağrım LVGL görevinde
     * çalışıyor ve kilidi burada tutmak kilitlenmeye yol açardı. */
    xSemaphoreTake(s_bekle, portMAX_DELAY);
    ESP_LOGI(TAG, "secilen satir: %d", s_secim);
    return s_secim;
}

/* liste_sor'a verilen metin sağlayıcılar. Ders listesi hangi sınıfa ait
 * olduğunu bilmek zorunda; geri çağrımın imzasında yer olmadığı için
 * seçilen sınıf burada tutuluyor. */
static int s_sinif;

static const char *sinif_metni(int i)
{
    return katalog_sinif(i);
}

static const char *ders_metni(int i)
{
    return katalog_ders(s_sinif, i);
}

esp_err_t secim_goster(void)
{
    if (katalog_sinif_sayisi() == 0) {
        ESP_LOGE(TAG, "katalog bos, secim gosterilemiyor");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_bekle == NULL) {
        s_bekle = xSemaphoreCreateBinary();
        if (s_bekle == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    const char *sinif = NULL;
    const char *ders = NULL;

    while (ders == NULL) {
        /* Ayarlar girişi BURADA: profil şeridine dokunmak zaten bu listeyi
         * açıyor, ayrı bir düğme ekranda yer yemeden aynı yere ulaşılıyor.
         * Demo günü ağ değiştirmek gerekirse tek dokunuş uzakta. */
        const int a = liste_sor("Sinif secin", katalog_sinif_sayisi(),
                                sinif_metni, NULL, 0);
        if (a < 0) {
            continue;
        }
        s_sinif = a;
        sinif = katalog_sinif(s_sinif);

        const int b = liste_sor("Ders secin", katalog_ders_sayisi(s_sinif),
                                ders_metni, "< Geri", GERI);
        if (b >= 0) {
            ders = katalog_ders(s_sinif, b);
        }
        /* b < 0 ise "Geri": sınıf listesine dönülüyor. */
    }

    ESP_LOGI(TAG, "secildi: %s / %s", sinif, ders);
    nvs_yaz(sinif, ders);
    tur_profil_ayarla(sinif, ders);
    return ESP_OK;
}


/* ---------------- Menü (katalogdan bağımsız) ---------------- */

static const char *MENU_SATIRLARI[] = {
    "Ders degistir",
    "Wi-Fi / Backend ayarlari",
};

static const char *menu_metni(int i)
{
    return MENU_SATIRLARI[i];
}

secim_menu_t secim_menu(void)
{
    if (s_bekle == NULL) {
        s_bekle = xSemaphoreCreateBinary();
        if (s_bekle == NULL) {
            return SECIM_MENU_GERI;
        }
    }
    const int n = (int)(sizeof(MENU_SATIRLARI) / sizeof(MENU_SATIRLARI[0]));
    const int secim = liste_sor("Ayarlar", n, menu_metni, "< Geri", GERI);
    if (secim < 0 || secim >= n) {
        return SECIM_MENU_GERI;
    }
    return (secim_menu_t)secim;
}
