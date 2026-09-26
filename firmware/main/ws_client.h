/*
 * Backend WebSocket istemcisi (plan 3.6, doküman 3.2 olay sözleşmesi).
 *
 * Cihaz -> backend:  speech_start {grade, subject} · ikili PCM · speech_end · cancel
 * Backend -> cihaz:  transcript · thinking · answer_text · ikili ses · done · error
 *
 * Cihaz ince istemci: mantık yok, yalnızca ses gönderiyor ve gelen olayları
 * ekrana/hoparlöre veriyor. Bu, tüm mantığın backend'de toplanması kararının
 * (doküman 1. bölüm, modüler monolit) cihaz tarafındaki karşılığı.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WS_KOPUK = 0,
    WS_BAGLANIYOR,
    WS_BAGLI,
} ws_durum_t;

/* Cihazın tur içindeki durumu — doküman 3.2'deki olay akışının karşılığı.
 * Ekran hangi bilgiyi göstereceğine buna bakarak karar veriyor. */
typedef enum {
    TUR_BOSTA = 0,      /* bekliyor, düğmeye basılabilir */
    TUR_KAYIT,          /* mikrofon açık, ses gönderiliyor */
    TUR_DUSUNUYOR,      /* speech_end gönderildi, cevap bekleniyor */
    TUR_KONUSUYOR,      /* ses geliyor ve çalınıyor */
    TUR_HATA,
} tur_durum_t;

/* Olaylar ana görevden değil WebSocket görevinden gelir; ekranı güncelleyecek
 * kod LVGL kilidini kendisi almalı. */
typedef struct {
    void (*transcript)(const char *metin);
    void (*dusunuyor)(void);
    void (*cevap_metni)(const char *metin, const char *kaynak, bool kaynak_yok);
    void (*ses)(const uint8_t *veri, size_t bayt);
    void (*bitti)(void);
    void (*hata)(const char *kod, const char *mesaj);
    void (*iptal)(void);
} ws_geri_cagirma_t;

esp_err_t ws_baslat(const char *url, const ws_geri_cagirma_t *cb);

/* Bas-konuş. speech_start profili taşır — 0.4 bulgusu gereği dizeler
 * katalogdan geldiği gibi, DEĞİŞTİRİLMEDEN gönderilmeli. */
esp_err_t ws_konusma_basla(const char *grade, const char *subject);
esp_err_t ws_ses_gonder(const uint8_t *pcm, size_t bayt);
esp_err_t ws_konusma_bitir(void);

/* ADR-04 barge-in: cevap çalınırken araya girildiğinde. */
esp_err_t ws_iptal(void);

ws_durum_t ws_durum(void);
tur_durum_t ws_tur_durum(void);
