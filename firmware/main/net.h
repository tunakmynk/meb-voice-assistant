/*
 * Wi-Fi + NVS ayarları (plan 3.5, ADR-09).
 *
 * ADR-09: "Dokunmatik klavye yavaştır. Wi-Fi şifresi ve backend adresi bu
 * ekrandan girileceği için değerler NVS'te kalıcı saklanır." Yani cihaz bir
 * kez kurulur, sonraki açılışlarda kendi bağlanır.
 *
 * R-01 (demo günü ağ sorunu) doğrudan buraya bağlı: ayarların ekrandan
 * ANINDA değiştirilebilmesi, misafir Wi-Fi'si tutmazsa telefon hotspot'una
 * geçebilmek demek. Bu yüzden değerler derleme zamanına gömülmüyor.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    NET_KAPALI = 0,
    NET_BAGLANIYOR,
    NET_BAGLI,
    NET_HATA,
} net_durum_t;

/* NVS'i okur, kayıtlı ayar varsa Wi-Fi'ye bağlanmayı dener.
 * Kayıt yoksa NET_KAPALI'da bekler — kurulum ekranı devreye girer. */
esp_err_t net_init(void);

/* Ayarları NVS'e yazar ve yeniden bağlanır. Kurulum ekranı bunu çağırır. */
esp_err_t net_ayarla(const char *ssid, const char *parola, const char *backend);

/* Kayıtlı ayarlar. Yoksa boş dize döner. */
const char *net_ssid(void);
const char *net_backend(void);

net_durum_t net_durum(void);

/* Bağlanınca alınan IP — ekranda göstermek, doğru ağa girdiğini anlamak
 * ve backend'e ulaşılabilirliği kabaca doğrulamak için. */
const char *net_ip(void);

/* --- Ağ tarama (3.5b kurulum ekranı için) --- */

/* Bir taramada döndürülecek en fazla ağ. Bir sınıfta görünen ağ sayısı
 * genelde 10-15; 20 hem yeterli hem de listeyi kaydırılabilir tutuyor. */
#define NET_AZAMI_AG 20

#define NET_SSID_AZAMI 33

/* Çevredeki ağları tarar, SSID'leri sinyal gücüne göre sıralı yazar.
 * BLOKLAR (~2 sn): tarama eşzamansız yapılıp arayüzü ikinci bir duruma
 * sokmaktansa, kullanıcı zaten "Ağları tara" düğmesine basmışken beklemesi
 * daha anlaşılır.
 *
 * Dönen değer bulunan ağ sayısı; hata durumunda 0.
 * SSID'yi elle yazdırmak yerine listeden seçtirmek şart: dokunmatik klavyede
 * yanlış yazılan bir SSID, demo günü teşhis edilmesi en zor hatalardan biri
 * olurdu — cihaz sessizce bağlanamaz ve sebebi görünmez. */
int net_tara(char ssidler[][NET_SSID_AZAMI], int azami);

/* Tarama modunu bitirir ve mevcut ayarlarla yeniden baglanmayi surdurur.
 * net_tara() otomatik yeniden baglanmayi DURDURUYOR (ikisi ayni anda
 * olamiyor); kurulumdan vazgecilirse bu cagrilmali, yoksa cihaz
 * kayitli agina bir daha hic baglanmaz. */
void net_tarama_bitir(void);

/* Kayıtlı ayar var mı — yoksa cihaz kurulum ekranıyla açılır. */
bool net_ayar_var(void);
