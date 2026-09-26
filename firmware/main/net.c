/*
 * Wi-Fi + NVS ayarları (plan 3.5, ADR-09).
 *
 * ============================================================
 * Neden ayarlar NVS'te, derleme zamanında değil
 * ============================================================
 * R-01: "Demo günü ağ: misafir Wi-Fi'de cihaz izolasyonu, port engeli, IP
 * değişimi -> cihaz backend'e hiç bağlanamaz, demo durur. Yedek yol: ayarların
 * ekrandan anında yeniden girilebilmesi (ADR-09)."
 *
 * Ayarlar ikiliye gömülü olsaydı, ağ değiştirmek için dizüstü + ESP-IDF +
 * yeniden derleme gerekirdi. Demo ortamında bu, demoyu kaybetmek demek.
 * NVS'te saklamak, telefonun hotspot'una 30 saniyede geçebilmek demek.
 *
 * ============================================================
 * Yeniden bağlanma
 * ============================================================
 * Wi-Fi kopması demo sırasında olağan (kalabalık 2.4 GHz bandı). Kesildiğinde
 * kendi kendine tekrar denemezse cihaz sessizce ölür; bu yüzden DISCONNECTED
 * olayında sınırsız yeniden deneme var. Deneme aralığı sabit 2 sn — üstel
 * geri çekilme daha zarif olurdu ama demo süresince hızlı toparlanmak,
 * yönlendiriciyi yormamaktan daha önemli.
 */

#include "net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "net";

#define NVS_AD "meb"
#define ANAHTAR_SSID "ssid"
#define ANAHTAR_PAROLA "pass"
#define ANAHTAR_BACKEND "backend"

/* Tarama modu: acikken otomatik yeniden baglanma askiya aliniyor.
 *
 * esp_wifi_scan_start(), surucu iliskilendirme denerken sonuc vermiyor.
 * Kopma isleyicisi 2 sn'de bir esp_wifi_connect() cagirdigi icin tarama
 * SIFIR ag donduruyor ve ekranda 'Ag bulunamadi' yaziyordu. Ilk kurulumda
 * sorun cikmamasinin sebebi de bu: o sirada cihaz zaten bagliydi ve
 * yeniden baglanma dongusu hic calismiyordu. Yani hata kurulum ekraninda
 * degil, ONCEKI basarisiz baglantinin biraktigi durumdaydi. */
static volatile bool s_tarama_modu;

static char s_ssid[33];
static char s_parola[65];
static char s_backend[96];
static char s_ip[16];
static net_durum_t s_durum = NET_KAPALI;
static bool s_baslatildi;

static esp_err_t nvs_oku(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_AD, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;   /* namespace hiç yazılmamış: ilk açılış */
    }

    size_t n = sizeof(s_ssid);
    if (nvs_get_str(h, ANAHTAR_SSID, s_ssid, &n) != ESP_OK) {
        s_ssid[0] = '\0';
    }
    n = sizeof(s_parola);
    if (nvs_get_str(h, ANAHTAR_PAROLA, s_parola, &n) != ESP_OK) {
        s_parola[0] = '\0';
    }
    n = sizeof(s_backend);
    if (nvs_get_str(h, ANAHTAR_BACKEND, s_backend, &n) != ESP_OK) {
        s_backend[0] = '\0';
    }
    nvs_close(h);
    return ESP_OK;
}

static void olay_isleyici(void *arg, esp_event_base_t base, int32_t id, void *veri)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Sınırsız yeniden deneme — yukarıdaki gerekçeye bakın. */
        s_durum = NET_BAGLANIYOR;
        s_ip[0] = '\0';
        /* SEBEP KODU sart. "Baglanti koptu" tek basina hicbir sey
         * soylemiyor; yanlis parola ile PMF uyusmazligi ayni sekilde
         * gorunuyor. Sik gorulenler:
         *   15  4WAY_HANDSHAKE_TIMEOUT -> parola yanlis (en yaygin)
         *  201  NO_AP_FOUND            -> ag kapali ya da 5 GHz
         *  202  AUTH_FAIL              -> kimlik dogrulama reddedildi
         *  205  CONNECTION_FAIL        -> genel baglanti hatasi */
        if (s_tarama_modu) {
            /* Kurulum ekrani acik: yeniden baglanmak taramayi bozar. */
            s_durum = NET_KAPALI;
            return;
        }
        const wifi_event_sta_disconnected_t *d =
            (const wifi_event_sta_disconnected_t *)veri;
        ESP_LOGW(TAG, "baglanti koptu (sebep %d), 2 sn sonra tekrar denenecek",
                 d ? d->reason : -1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        /* Bayrak GECIKMEDEN SONRA da kontrol ediliyor.
         *
         * Bu isleyici 2 saniye uyuyor; o sirada kurulum ekrani acilip
         * tarama moduna gecilmis olabilir. Yalnizca basta bakmak, 2 saniye
         * once verilmis bir karari simdi uygulamak demekti: tarama tam
         * baslarken uzerine bir baglanti denemesi biniyor ve surucu
         * ESP_ERR_WIFI_STATE donduruyordu. Alti yeniden deneme bile
         * yetmemisti, cunku sorun zamanlama degil bu bayat karardi. */
        if (s_tarama_modu) {
            s_durum = NET_KAPALI;
            return;
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)veri;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_durum = NET_BAGLI;
        ESP_LOGI(TAG, "baglandi: %s  IP %s", s_ssid, s_ip);
    }
}

static esp_err_t wifi_baslat(void)
{
    if (s_baslatildi) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &olay_isleyici, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &olay_isleyici, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* Ayarlar RAM'de tutuluyor: NVS'i biz kendimiz yönetiyoruz ve Wi-Fi
     * sürücüsünün ayrı bir kopya saklaması kafa karışıklığı yaratır. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    s_baslatildi = true;
    return ESP_OK;
}

static esp_err_t baglan(void)
{
    if (s_ssid[0] == '\0') {
        s_durum = NET_KAPALI;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_ERROR_CHECK(wifi_baslat());

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_parola, sizeof(wc.sta.password) - 1);
    /* Açık ağlar da desteklensin: parola boşsa eşik OPEN olmalı, yoksa
     * sürücü şifreli ağ arar ve açık hotspot'a hiç bağlanmaz. */
    wc.sta.threshold.authmode = (s_parola[0] == '\0') ? WIFI_AUTH_OPEN
                                                      : WIFI_AUTH_WPA2_PSK;

    /* PMF (korumali yonetim cerceveleri) YETENEKLI olarak bildiriliyor.
     *
     * wifi_config_t sifirla baslatildigi icin capable=false kaliyordu.
     * Modern telefon hotspotlari (WPA3 ya da WPA2/WPA3 karisik) PMF
     * istiyor; istemci yeteneksiz gorunurse iliskilendirme basarili olsa
     * bile dort yollu el sikismasi dusuyor. Logda "assoc -> run" sonrasi
     * birkac saniyede kopma olarak gorunuyor -- yanlis parolayla ayni
     * belirti, bu yuzden sebep kodu olmadan ayirt edilemiyordu.
     * required=false: WPA2-only eski aglar da calismaya devam ediyor. */
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    s_durum = NET_BAGLANIYOR;
    ESP_LOGI(TAG, "baglaniyor: %s", s_ssid);

    esp_err_t err = esp_wifi_start();
    if (err == ESP_ERR_WIFI_NOT_STOPPED) {
        /* Zaten çalışıyor (ayar değişikliği): yeniden bağlan yeter. */
        err = esp_wifi_connect();
    }
    return err;
}

int net_tara(char ssidler[][NET_SSID_AZAMI], int azami)
{
    if (wifi_baslat() != ESP_OK) {
        return 0;
    }
    /* Tarama sürücü çalışırken yapılabiliyor ama sürücünün başlatılmış
     * olması şart. İlk kurulumda henüz hiçbir ağa bağlanmadığımız için
     * esp_wifi_start() burada da çağrılıyor; zaten çalışıyorsa zararsız. */
    const esp_err_t bs = esp_wifi_start();
    if (bs != ESP_OK && bs != ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_LOGE(TAG, "wifi baslatilamadi: %s", esp_err_to_name(bs));
        return 0;
    }

    /* Yeniden baglanma dongusu durduruluyor ve varsa mevcut baglanti
     * kesiliyor: tarama ancak surucu bosken sonuc veriyor. */
    s_tarama_modu = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Bloklayan tarama: true parametresi bitene kadar dönmüyor. */
    /* Tarama birkac kez deneniyor: esp_wifi_disconnect() istegi ANINDA
     * islemiyor ve acilistan kalan bir baglanti denemesi hala yoldaysa
     * surucu ESP_ERR_WIFI_STATE donduruyor. Tek denemede birakmak, kurulum
     * ekrani ilk acildiginda bos liste gostermek demekti; kullanicinin
     * 'Yeniden tara'ya basmasi gerekiyordu ve bunu bilmesinin bir yolu
     * yoktu. */
    esp_err_t tr = ESP_FAIL;
    for (int deneme = 0; deneme < 6; deneme++) {
        tr = esp_wifi_scan_start(NULL, true);
        if (tr == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "tarama hazir degil (%s), yeniden deneniyor",
                 esp_err_to_name(tr));
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    if (tr != ESP_OK) {
        ESP_LOGE(TAG, "tarama basarisiz: %s", esp_err_to_name(tr));
        return 0;
    }

    uint16_t adet = (uint16_t)azami;
    wifi_ap_record_t *kayitlar = calloc(adet, sizeof(wifi_ap_record_t));
    if (kayitlar == NULL) {
        esp_wifi_scan_stop();
        return 0;
    }
    if (esp_wifi_scan_get_ap_records(&adet, kayitlar) != ESP_OK) {
        free(kayitlar);
        return 0;
    }

    /* esp_wifi zaten sinyal gücüne göre sıralı veriyor. Tekrarlar eleniyor:
     * çok bantlı bir yönlendirici aynı SSID'yi birden çok kez bildiriyor ve
     * listede üç kez aynı adı görmek kullanıcıyı tereddüde düşürürdü. */
    int n = 0;
    for (int i = 0; i < adet && n < azami; i++) {
        const char *ad = (const char *)kayitlar[i].ssid;
        if (ad[0] == '\0') {
            continue;   /* gizli ağ: listeden seçilemez */
        }
        bool zaten = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(ssidler[j], ad) == 0) {
                zaten = true;
                break;
            }
        }
        if (!zaten) {
            snprintf(ssidler[n], NET_SSID_AZAMI, "%s", ad);
            n++;
        }
    }
    free(kayitlar);
    ESP_LOGI(TAG, "tarama: %d ag bulundu", n);
    return n;
}

void net_tarama_bitir(void)
{
    if (!s_tarama_modu) {
        return;
    }
    s_tarama_modu = false;
    if (s_ssid[0] != '\0') {
        s_durum = NET_BAGLANIYOR;
        esp_wifi_connect();
    }
}

esp_err_t net_init(void)
{
    nvs_oku();

    /* NVS boşsa menuconfig'teki değerlerle ilk kurulumu yap.
     *
     * Bu GEÇİCİ bir köprü: dokunmatik kurulum ekranı (3.5b) gelene kadar
     * cihazın bir ağa bağlanabilmesi gerekiyor. Değerler sdkconfig'te durur,
     * sdkconfig .gitignore'da olduğu için parola depoya girmez. Ekran
     * geldiğinde oradan girilen değer NVS'te bunun üzerine yazar ve
     * menuconfig bir daha devreye girmez. */
    if (s_ssid[0] == '\0' && CONFIG_MEB_WIFI_SSID[0] != '\0') {
        ESP_LOGW(TAG, "NVS bos — menuconfig degerleriyle ilk kurulum");
        return net_ayarla(CONFIG_MEB_WIFI_SSID, CONFIG_MEB_WIFI_PASSWORD,
                          CONFIG_MEB_BACKEND_URL);
    }

    if (s_ssid[0] == '\0') {
        ESP_LOGW(TAG, "NVS'te Wi-Fi ayari yok — kurulum ekrani gerekiyor");
        s_durum = NET_KAPALI;
        return ESP_OK;
    }
    return baglan();
}

esp_err_t net_ayarla(const char *ssid, const char *parola, const char *backend)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_parola, sizeof(s_parola), "%s", parola ? parola : "");
    snprintf(s_backend, sizeof(s_backend), "%s", backend ? backend : "");

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_AD, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, ANAHTAR_SSID, s_ssid);
    nvs_set_str(h, ANAHTAR_PAROLA, s_parola);
    nvs_set_str(h, ANAHTAR_BACKEND, s_backend);
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    s_tarama_modu = false;   /* kurulum bitti, yeniden baglanma serbest */
    ESP_LOGI(TAG, "ayarlar kaydedildi: ssid=%s backend=%s", s_ssid, s_backend);
    return baglan();
}

const char *net_ssid(void) { return s_ssid; }
const char *net_backend(void) { return s_backend; }
const char *net_ip(void) { return s_ip; }
net_durum_t net_durum(void) { return s_durum; }
bool net_ayar_var(void) { return s_ssid[0] != '\0'; }
