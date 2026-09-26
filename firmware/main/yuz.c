/*
 * Asistan yüzü — durum göstergesi (plan 3.6c).
 *
 * ============================================================
 * Neden LVGL nesneleri, resim değil
 * ============================================================
 * Her durum için hazır resim gömmek flash'ta yer kaplardı ve konuşma
 * animasyonu için onlarca kare gerekirdi. Yuvarlatılmış dikdörtgenlerden
 * kurulu bir yüz, boyutları animasyonla değiştirilerek her duruma
 * dönüşebiliyor — toplam maliyet birkaç yüz bayt.
 *
 * ============================================================
 * Animasyon ömrü
 * ============================================================
 * LVGL animasyonları nesneye bağlı ve nesne silinince kendileri de
 * siliniyor. Yine de yuz_birak() açıkça durduruyor: ekran seçim ekranı
 * için yıkıldığında animasyonun silinmiş belleğe dokunması, ortaya
 * çıkması en zor hata türü olurdu.
 */

#include "yuz.h"

#include "esp_log.h"

#include "display.h"

/* Yüz alanı. Ekran 240x320 ve altında soru/cevap metni var; yüz tanınır
 * kalmalı ama metne yer bırakmalı. */
#define YUZ_G 100
#define YUZ_Y 56

#define GOZ_G 14
#define GOZ_ACIK 18
#define GOZ_IRI 22
#define GOZ_KAPALI 3

#define AGIZ_G 30
#define AGIZ_KAPALI 4
#define AGIZ_ACIK 20

#define RENK_YUZ 0xE6E8EE
#define RENK_AGIZ 0x4C8DFF

static lv_obj_t *s_kap;
static lv_obj_t *s_sol_goz;
static lv_obj_t *s_sag_goz;
static lv_obj_t *s_agiz;
static lv_timer_t *s_kirpma_zaman;
static yuz_durum_t s_durum = YUZ_HAZIR;

/* --- Animasyon yardımcıları --- */

static void goz_boy_ayarla(void *nesne, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)nesne, v);
}

static void agiz_boy_ayarla(void *nesne, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)nesne, v);
}

static void kirp(void)
{
    /* İki göz aynı anda: ayrı animasyonlar zamanla kayardı ve yüz
     * göz kırpmak yerine şaşı bakardı. */
    lv_obj_t *gozler[2] = { s_sol_goz, s_sag_goz };
    const int32_t acik = (s_durum == YUZ_DINLIYOR) ? GOZ_IRI : GOZ_ACIK;

    for (int i = 0; i < 2; i++) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, gozler[i]);
        lv_anim_set_exec_cb(&a, goz_boy_ayarla);
        lv_anim_set_values(&a, acik, GOZ_KAPALI);
        lv_anim_set_duration(&a, 90);
        lv_anim_set_playback_duration(&a, 90);
        lv_anim_start(&a);
    }
}

static void kirpma_zamanlayici(lv_timer_t *t)
{
    (void)t;
    /* Konuşurken göz kırpmak ağız animasyonuyla yarışıp yüzü huzursuz
     * gösteriyor; yalnızca boşta ve dinlerken kırpılıyor. */
    if (s_durum == YUZ_HAZIR || s_durum == YUZ_DINLIYOR) {
        kirp();
    }
}

static void agiz_animasyonu_baslat(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_agiz);
    lv_anim_set_exec_cb(&a, agiz_boy_ayarla);
    lv_anim_set_values(&a, AGIZ_KAPALI, AGIZ_ACIK);
    lv_anim_set_duration(&a, 160);
    lv_anim_set_playback_duration(&a, 160);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

/* --- Kurulum --- */

lv_obj_t *yuz_olustur(lv_obj_t *ebeveyn)
{
    s_kap = lv_obj_create(ebeveyn);
    lv_obj_set_size(s_kap, YUZ_G, YUZ_Y);
    lv_obj_set_style_bg_opa(s_kap, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_kap, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_kap, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_kap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_kap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *gozler[2];
    for (int i = 0; i < 2; i++) {
        gozler[i] = lv_obj_create(s_kap);
        lv_obj_set_size(gozler[i], GOZ_G, GOZ_ACIK);
        lv_obj_set_style_radius(gozler[i], GOZ_G / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(gozler[i], lv_color_hex(RENK_YUZ), LV_PART_MAIN);
        lv_obj_set_style_border_width(gozler[i], 0, LV_PART_MAIN);
        lv_obj_remove_flag(gozler[i], LV_OBJ_FLAG_SCROLLABLE);
        /* Yüz bas-konuş alanının İÇİNDE duruyor; tıklanabilir kalırsa
         * gözlere denk gelen dokunuşu yutar ve kayıt başlamaz. */
        lv_obj_remove_flag(gozler[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(gozler[i], LV_ALIGN_TOP_MID, (i == 0) ? -18 : 18, 6);
    }
    s_sol_goz = gozler[0];
    s_sag_goz = gozler[1];

    s_agiz = lv_obj_create(s_kap);
    lv_obj_set_size(s_agiz, AGIZ_G, AGIZ_KAPALI);
    lv_obj_set_style_radius(s_agiz, AGIZ_KAPALI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_agiz, lv_color_hex(RENK_AGIZ), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_agiz, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_agiz, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_agiz, LV_OBJ_FLAG_CLICKABLE);
    /* Ağız merkeze hizalı: boyu animasyonla değişince iki yöne birden
     * açılıyor, çenesi aşağı kayan bir ağız gibi görünmüyor. */
    lv_obj_align(s_agiz, LV_ALIGN_CENTER, 0, 16);

    if (s_kirpma_zaman == NULL) {
        s_kirpma_zaman = lv_timer_create(kirpma_zamanlayici, 3200, NULL);
    }

    s_durum = YUZ_HAZIR;
    return s_kap;
}

void yuz_durum_ayarla(yuz_durum_t d)
{
    if (!display_lock(100)) {
        return;
    }
    if (s_kap == NULL) {
        display_unlock();
        return;
    }
    s_durum = d;

    /* Devam eden animasyonlar durduruluyor: yeni durumun boyutları eski
     * animasyonun ara karesiyle çakışırsa yüz titrer. */
    lv_anim_delete(s_agiz, agiz_boy_ayarla);
    lv_anim_delete(s_sol_goz, goz_boy_ayarla);
    lv_anim_delete(s_sag_goz, goz_boy_ayarla);

    switch (d) {
    case YUZ_DINLIYOR:
        lv_obj_set_height(s_sol_goz, GOZ_IRI);
        lv_obj_set_height(s_sag_goz, GOZ_IRI);
        lv_obj_set_height(s_agiz, AGIZ_KAPALI);
        lv_obj_set_width(s_agiz, AGIZ_KAPALI * 3);
        lv_obj_align(s_sol_goz, LV_ALIGN_TOP_MID, -18, 4);
        lv_obj_align(s_sag_goz, LV_ALIGN_TOP_MID, 18, 4);
        break;

    case YUZ_DUSUNUYOR:
        /* Gözler yukarı: "aklından geçiriyor" jesti, tek karede anlaşılıyor. */
        lv_obj_set_height(s_sol_goz, GOZ_KAPALI * 3);
        lv_obj_set_height(s_sag_goz, GOZ_KAPALI * 3);
        lv_obj_set_width(s_agiz, AGIZ_G);
        lv_obj_set_height(s_agiz, AGIZ_KAPALI);
        lv_obj_align(s_sol_goz, LV_ALIGN_TOP_MID, -18, 4);
        lv_obj_align(s_sag_goz, LV_ALIGN_TOP_MID, 18, 4);
        break;

    case YUZ_KONUSUYOR:
        lv_obj_set_height(s_sol_goz, GOZ_ACIK);
        lv_obj_set_height(s_sag_goz, GOZ_ACIK);
        lv_obj_set_width(s_agiz, AGIZ_G);
        lv_obj_align(s_sol_goz, LV_ALIGN_TOP_MID, -18, 6);
        lv_obj_align(s_sag_goz, LV_ALIGN_TOP_MID, 18, 6);
        agiz_animasyonu_baslat();
        break;

    case YUZ_HAZIR:
    default:
        lv_obj_set_height(s_sol_goz, GOZ_ACIK);
        lv_obj_set_height(s_sag_goz, GOZ_ACIK);
        lv_obj_set_width(s_agiz, AGIZ_G);
        lv_obj_set_height(s_agiz, AGIZ_KAPALI);
        lv_obj_align(s_sol_goz, LV_ALIGN_TOP_MID, -18, 6);
        lv_obj_align(s_sag_goz, LV_ALIGN_TOP_MID, 18, 6);
        break;
    }
    display_unlock();
}

void yuz_birak(void)
{
    if (s_kirpma_zaman != NULL) {
        lv_timer_delete(s_kirpma_zaman);
        s_kirpma_zaman = NULL;
    }
    if (s_agiz != NULL) {
        lv_anim_delete(s_agiz, agiz_boy_ayarla);
    }
    if (s_sol_goz != NULL) {
        lv_anim_delete(s_sol_goz, goz_boy_ayarla);
        lv_anim_delete(s_sag_goz, goz_boy_ayarla);
    }
    /* Nesneler ekranla birlikte siliniyor; burada yalnızca işaretçiler
     * bırakılıyor ki durum değişimi silinmiş nesneye dokunmasın. */
    s_kap = NULL;
    s_sol_goz = NULL;
    s_sag_goz = NULL;
    s_agiz = NULL;
}
