/*
 * Sınıf/ders seçim ekranı (plan 3.6b).
 *
 * ADR-02'ye göre dokunmatik birincil giriş. Seçim NVS'te saklanıyor:
 * öğrenci her açılışta aynı dersi yeniden seçmek zorunda kalmamalı, ama
 * ders değiştirebilmeli de.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* NVS'te kayıtlı seçim varsa yükler ve tur_profil_ayarla() ile uygular.
 * Kayıt yoksa false döner — o zaman secim_goster() çağrılmalı. */
bool secim_yukle(void);

/* Seçim ekranını gösterir ve kullanıcı seçene kadar BLOKLAR.
 * Seçimi NVS'e yazar ve tur_profil_ayarla() ile uygular.
 * katalog_getir() başarılı olduktan sonra çağrılmalı. */
esp_err_t secim_goster(void);

/* Profil şeridine dokununca açılan menü. KATALOG GEREKTİRMEZ.
 *
 * Ayarlar girişi önce sınıf listesinin içindeydi; o liste katalogla
 * doluyor, katalog da çalışan bir backend istiyor. Yani backend adresi
 * yanlışken — tam da ayarlara ihtiyaç duyulan an — ayarlara ULAŞILAMIYORDU.
 * Menü bu bağımlılığı kırıyor: ağ olmadan da açılıyor. */
typedef enum {
    SECIM_MENU_GERI = -1,
    SECIM_MENU_DERS = 0,
    SECIM_MENU_AYARLAR = 1,
} secim_menu_t;

secim_menu_t secim_menu(void);
