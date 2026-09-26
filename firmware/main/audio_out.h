/*
 * I2S hoparlör çıkışı — MAX98357A (plan 3.4).
 *
 * R-03 doğrulama sırası: ekran -> mikrofon -> HOPARLÖR -> ağ.
 *
 * Cihazda MP3 çözücü YOK. Backend akışlı ham PCM gönderiyor (plan 2.1'de
 * ölçüldü: akışlı PCM'de ilk bayt 683 ms, akışlı mp3'te 1282 ms), dolayısıyla
 * gelen baytlar doğrudan I2S'e yazılıyor.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* I2S TX kanalını kurar ve amfiyi kapalı bırakır. */
esp_err_t audio_out_init(void);

/* Amfiyi açar/kapatır (MAX98357A SD pini).
 *
 * ADR-02 gereği kayıt ve çalma aynı anda olmuyor, bu yüzden konuşma dışında
 * amfi kapatılabiliyor: Class-D'nin boştaki cızırtısı sessiz bir sınıfta
 * duyulur. ADR-04 barge-in'de de hoparlörü anında susturmanın en kesin yolu
 * budur — tampondaki sesin çalınmasını beklemek gerekmez. */
void audio_out_enable(bool on);

/* Örnekleme hızını değiştirir. Mikrofon 16 kHz kaydediyor, backend TTS ise
 * 24 kHz gönderiyor; aynı kanal ikisini de çalacağı için hız çalınacak
 * kaynağa göre ayarlanmalı. Yanlış hız sesi tiz ya da pes yapar. */
esp_err_t audio_out_set_rate(uint32_t hz);

/* Ham PCM (16-bit, mono) yazar. Tamponu boşalana kadar bloklar. */
esp_err_t audio_out_write(const uint8_t *data, size_t bytes);

/* Yüksek geçiren süzgecin durumunu sıfırlar (yeni çalma başlarken). */
void audio_out_hp_sifirla(void);

/* Kalan sesi çalıp bitirir, sonra amfiyi kapatır. */
void audio_out_flush(void);

/* Cihazın kendi ürettiği ezgi — mikrofonu devre dışı bırakıp yalnızca
 * çalma yolunu sınar. Temiz duyulursa sorun mikrofonda. */
void audio_out_ezgi(void);

/* I2S çerçeve biçimi taraması — bit hizalaması şüphesinde. */
void audio_out_cerceve_taramasi(void);

/* Frekans taraması — bozulmanın mekanik mi elektriksel mi olduğunu ayırır. */
void audio_out_frekans_taramasi(void);

/* Belirli genlikte ezgi (0-32767). */
void audio_out_ezgi_genlik(int16_t tepe);

/* I2S'siz test: SD pinini açıp kapatarak besleme+hoparlör bağlantısını sınar. */
void audio_out_tik_testi(void);

/* Kanal seçeneklerini sırayla deneyen tanılama — ses hiç gelmediğinde. */
void audio_out_diag(void);

/* Genliği belirtilen sinüs tonu (0-32767). */
void audio_out_tone_amp(uint32_t hz, uint32_t ms, int16_t genlik);

/* Test amaçlı sinüs tonu — hoparlör yolunu mikrofondan bağımsız doğrular. */
void audio_out_test_tone(uint32_t hz, uint32_t ms);
