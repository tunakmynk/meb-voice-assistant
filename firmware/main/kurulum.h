/*
 * Wi-Fi + backend kurulum ekranı (plan 3.5b, ADR-09).
 *
 * R-01'in doğrudan karşılığı: demo günü misafir ağı tutmazsa telefon
 * hotspot'una geçebilmek. Menuconfig köprüsü bunu yapamıyor — firmware
 * yeniden derlemek gerekiyor ve demo ortamında bu gerçekçi değil.
 *
 * BACKEND ADRESİ EN AZ WI-FI KADAR ÖNEMLİ. Ağ değiştiğinde backend'i
 * çalıştıran dizüstünün IP'si de değişiyor; yalnızca SSID/parola
 * değiştirilebilseydi cihaz ağa girer ama backend'e ulaşamazdı ve ekranda
 * yalnızca "Backend'e baglanilamadi" görünürdü.
 */

#pragma once

#include "esp_err.h"

/* Kurulum ekranını gösterir ve kullanıcı kaydedene kadar BLOKLAR.
 * Kaydedince net_ayarla() çağrılır; ayarlar NVS'e yazılıp bağlantı
 * yeniden kurulur.
 *
 * İptal edilirse ESP_ERR_NOT_FINISHED döner ve mevcut ayarlar korunur. */
esp_err_t kurulum_goster(void);

/* Baglanti bekleme ekrani. Baglanana kadar BLOKLAR ve bu sirada net.c
 * arka planda yeniden denemeyi surdurur; ag ortaya cikinca kendiliginden
 * baglanip doner.
 *
 * Neden kurulum ekranini ZORLAMIYOR: telefon hotspot'lari istemci yokken
 * kapaniyor ya da uykuya geciyor. Cihaz sifirlandiginda ag henuz ortada
 * olmayabiliyor; sabit bir zaman asiminda pes edip parola sormak, kayitli
 * ayarlar dogruyken kullaniciya her seferinde parola yazdirmak demekti.
 * Ekrandaki dugme ayarlara gitmeyi ISTEGE BAGLI birakiyor. */
void kurulum_baglanti_bekle(void);
