/*
 * Asistan yüzü — durum göstergesi (plan 3.6c).
 *
 * Neden yüz: cihazın ne yaptığı sesten her zaman anlaşılmıyor. Sessiz bir
 * sınıfta "dinliyor mu, düşünüyor mu, bağlantı mı koptu" sorusu metin
 * okumadan da cevaplanabilmeli — özellikle küçük sınıflarda. Yüz, durum
 * yazısının yerine geçmiyor, ona GÖRSEL bir karşılık veriyor.
 *
 * Animasyonlar LVGL'in kendi zamanlayıcısında dönüyor; ayrı görev yok.
 */

#pragma once

#include "lvgl.h"

typedef enum {
    YUZ_HAZIR,        /* boşta: yavaş göz kırpma */
    YUZ_DINLIYOR,     /* kayıt sürüyor: gözler iri, ağız küçük */
    YUZ_DUSUNUYOR,    /* backend'i bekliyor: gözler yukarı, ağız düz */
    YUZ_KONUSUYOR,    /* TTS çalıyor: ağız açılıp kapanıyor */
} yuz_durum_t;

/* Yüzü verilen kabın içinde kurar. LVGL kilidi ÇAĞIRANDA olmalı. */
lv_obj_t *yuz_olustur(lv_obj_t *ebeveyn);

/* Durum değiştirir. Her yerden çağrılabilir; kilidi kendi alır. */
void yuz_durum_ayarla(yuz_durum_t d);

/* Ekran yıkılmadan önce çağrılmalı: animasyonlar silinmiş nesnelere
 * dokunmamalı. LVGL kilidi ÇAĞIRANDA olmalı. */
void yuz_birak(void);
