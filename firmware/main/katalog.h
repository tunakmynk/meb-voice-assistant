/*
 * Sınıf/ders kataloğu (plan 3.6b).
 *
 * Backend'in GET /catalog ucu sınıf -> ders[] eşlemesi döndürüyor ve bu
 * eşleme Qdrant'tan okunuyor, elle yazılmıyor. Yani katalog, cihazda
 * gömülü bir listeyle DEĞİL, gerçekten indekslenmiş kitaplarla birebir
 * aynı — kullanıcıya karşılığı olmayan bir ders gösterilemiyor.
 *
 * Dizeler katalogdan geldiği gibi taşınıyor. Plan 0.4: normalizasyon
 * yapılmaz, "8.Sınıf" biçimi backend'de birebir eşleşiyor. Cihazda
 * güzelleştirmek (boşluk eklemek gibi) aramayı sessizce boşa düşürürdü.
 */

#pragma once

#include "esp_err.h"

/* Backend'den kataloğu indirir ve belleğe alır. Wi-Fi bağlandıktan sonra
 * çağrılmalı. Adres CONFIG_MEB_BACKEND_URL'den türetiliyor; ayrı bir ayar
 * olsaydı demo günü ikisi birbirini tutmayabilirdi. */
esp_err_t katalog_getir(void);

int katalog_sinif_sayisi(void);
const char *katalog_sinif(int i);

int katalog_ders_sayisi(int sinif_index);
const char *katalog_ders(int sinif_index, int ders_index);

/* Seçim yapıldıktan sonra belleği geri verir. */
void katalog_serbest(void);
