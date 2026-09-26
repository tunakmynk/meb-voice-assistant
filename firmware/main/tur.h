/*
 * Tur durum makinesi (plan 3.6).
 *
 * ADR-02 bas-konuş: ekrana/düğmeye basılı tutulduğunda kayıt başlar, el
 * çekilince biter. ADR-04: cevap çalınırken basılırsa araya girilir.
 */

#pragma once

#include "esp_err.h"

/* Ekranı kurar, backend'e bağlanır ve bas-konuş görevini başlatır.
 * net_init() sonrası, Wi-Fi bağlandıktan sonra çağrılmalı. */
esp_err_t tur_baslat(void);

/* Aktif sınıf/ders. Dizeler katalogdan geldiği gibi verilmeli —
 * plan 0.4: normalizasyon yapılmaz, "8.Sınıf" biçimi birebir eşleşir. */
void tur_profil_ayarla(const char *grade, const char *subject);
