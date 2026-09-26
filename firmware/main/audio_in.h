/*
 * I2S mikrofon girişi — INMP441 (plan 3.3).
 *
 * R-03 doğrulama sırası: ekran -> MİKROFON -> hoparlör -> ağ.
 *
 * Bu adımın doğrulaması diğerlerinden zor: kaydın çalıştığı gözle
 * görülmüyor. Bu yüzden modül ham bayt döndürmenin yanında bir de SEVİYE
 * ölçüsü veriyor; ekrana çubuk olarak basılınca mikrofonun çalıştığı
 * konuşarak anlaşılabiliyor.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* I2S RX kanalını kurar ve PSRAM'de kayıt tamponunu ayırır. */
esp_err_t audio_in_init(void);

/* Kaydı başlatır (bas-konuş: ekrana basıldığında). Tamponu sıfırlar. */
esp_err_t audio_in_start(void);

/* Kaydı durdurur (el çekildiğinde). */
esp_err_t audio_in_stop(void);

/* Mikrofondan okunanı tampona ekler. Kayıt sırasında düzenli çağrılmalı.
 * Dönüş: bu çağrıda eklenen bayt sayısı (16-bit PCM). */
size_t audio_in_pump(void);

/* Şimdiye kadar kaydedilen ses. audio_in_start ile sıfırlanır. */
const uint8_t *audio_in_data(void);
size_t audio_in_size(void);

/* Son okunan parçanın seviyesi, 0-100.
 *
 * Mikrofonun çalıştığını doğrulamanın tek pratik yolu bu: sessizlikte düşük,
 * konuşurken yükselen bir sayı. Ham baytlara bakmak bir şey söylemez. */
uint8_t audio_in_level(void);

/* Ham örnekleri seri porta basar — dalga biçimini gözle doğrulamak için. */
void audio_in_dokum(size_t adet);

/* Tampon doldu mu — sessizce üzerine yazmak yerine bilinmesi gerekiyor. */
bool audio_in_is_full(void);
