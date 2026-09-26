/*
 * Sınıf/ders kataloğu — GET /catalog (plan 3.6b).
 *
 * ============================================================
 * Neden burada cJSON kullanılıyor
 * ============================================================
 * ws_client.c bilerek cJSON kullanmıyor: her tur içinde onlarca olay
 * mesajı ayrıştırılıyor ve tekrarlanan ayırma/serbest bırakma öbeği
 * parçalıyor. Katalog ise açılışta BİR KEZ, iç içe nesne+dizi yapısında
 * geliyor; onu dize arayarak ayrıştırmak kırılgan olurdu ve parçalanma
 * gerekçesi tek seferlik bir çağrı için geçerli değil.
 */

#include "katalog.h"

#include "net.h"

#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "katalog";

/* 9 ders x birkaç sınıf birkaç yüz bayt tutuyor; 8 KB fazlasıyla yeterli
 * ve PSRAM'den alındığı için dar olan dahili RAM'e dokunmuyor. */
#define TAMPON 8192

static cJSON *s_kok;

/* ws://192.168.1.89:8000/ws  ->  http://192.168.1.89:8000/catalog
 *
 * Şema ve yol değişiyor, ana makine ve kapı aynı kalıyor. Kullanıcının iki
 * ayrı adresi elle tutarlı tutması gerekmiyor. */
static bool katalog_url(char *cikti, size_t n)
{
    /* Adres NVS'ten (net_backend), derleme zamani sabitinden DEGIL.
     *
     * CONFIG_MEB_BACKEND_URL kullanmak, ayni adresin iki kaynagi olmasi
     * demekti ve ekrandan yeni adres girilince ikisi ayrisiyordu:
     * WebSocket net_backend() aldigi icin BAGLANIYOR, katalog ise eski
     * derleme degerine gidip zaman asimina ugruyordu. Cihaz calisiyor
     * gorunurken ders degistirmek imkansiz hale geliyordu.
     *
     * NVS bossa (hic kurulum yapilmamis) derleme degerine dusuluyor. */
    const char *ws = net_backend();
    if (ws == NULL || ws[0] == 0) {
        ws = CONFIG_MEB_BACKEND_URL;
    }
    const char *makine = strstr(ws, "://");
    if (makine == NULL) {
        ESP_LOGE(TAG, "backend adresi anlasilamadi: %s", ws);
        return false;
    }
    makine += 3;

    const char *yol = strchr(makine, '/');
    const int uzunluk = (yol != NULL) ? (int)(yol - makine) : (int)strlen(makine);

    const int yazilan = snprintf(cikti, n, "http://%.*s/catalog", uzunluk, makine);
    return yazilan > 0 && (size_t)yazilan < n;
}

esp_err_t katalog_getir(void)
{
    char url[128];
    if (!katalog_url(url, sizeof(url))) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "katalog aliniyor: %s", url);

    char *tampon = heap_caps_malloc(TAMPON, MALLOC_CAP_SPIRAM);
    if (tampon == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t istemci = esp_http_client_init(&cfg);
    if (istemci == NULL) {
        free(tampon);
        return ESP_FAIL;
    }

    esp_err_t sonuc = esp_http_client_open(istemci, 0);
    if (sonuc != ESP_OK) {
        ESP_LOGE(TAG, "baglanti kurulamadi: %s", esp_err_to_name(sonuc));
        goto bitir;
    }

    esp_http_client_fetch_headers(istemci);
    const int durum = esp_http_client_get_status_code(istemci);
    if (durum != 200) {
        ESP_LOGE(TAG, "beklenmeyen HTTP durumu: %d", durum);
        sonuc = ESP_FAIL;
        goto bitir;
    }

    const int okunan = esp_http_client_read_response(istemci, tampon, TAMPON - 1);
    if (okunan <= 0) {
        ESP_LOGE(TAG, "govde okunamadi");
        sonuc = ESP_FAIL;
        goto bitir;
    }
    tampon[okunan] = '\0';

    cJSON *kok = cJSON_Parse(tampon);
    /* Cevap {"grades": {sinif: [ders...]}} biçiminde; sınıf eşlemesi kökün
     * KENDİSİ değil, "grades" altında. Doğrudan kökü kullanmak sessizce boş
     * bir katalog verirdi — ekran açılır, hiçbir sınıf görünmezdi. */
    cJSON *sinif_map = cJSON_DetachItemFromObject(kok, "grades");
    cJSON_Delete(kok);
    if (sinif_map == NULL || !cJSON_IsObject(sinif_map)) {
        ESP_LOGE(TAG, "JSON ayristirilamadi ya da 'grades' alani yok");
        cJSON_Delete(sinif_map);
        sonuc = ESP_ERR_INVALID_RESPONSE;
        goto bitir;
    }

    katalog_serbest();
    s_kok = sinif_map;
    ESP_LOGI(TAG, "katalog hazir: %d sinif", katalog_sinif_sayisi());
    sonuc = ESP_OK;

bitir:
    esp_http_client_close(istemci);
    esp_http_client_cleanup(istemci);
    free(tampon);
    return sonuc;
}

int katalog_sinif_sayisi(void)
{
    return s_kok != NULL ? cJSON_GetArraySize(s_kok) : 0;
}

const char *katalog_sinif(int i)
{
    const cJSON *e = cJSON_GetArrayItem(s_kok, i);
    return (e != NULL) ? e->string : NULL;
}

int katalog_ders_sayisi(int sinif_index)
{
    const cJSON *e = cJSON_GetArrayItem(s_kok, sinif_index);
    return cJSON_IsArray(e) ? cJSON_GetArraySize(e) : 0;
}

const char *katalog_ders(int sinif_index, int ders_index)
{
    const cJSON *e = cJSON_GetArrayItem(s_kok, sinif_index);
    const cJSON *d = cJSON_GetArrayItem(e, ders_index);
    return cJSON_IsString(d) ? d->valuestring : NULL;
}

void katalog_serbest(void)
{
    cJSON_Delete(s_kok);
    s_kok = NULL;
}
