/*
 * Backend WebSocket istemcisi (plan 3.6, doküman 3.2).
 *
 * ============================================================
 * Ses NEDEN ikili çerçevede gönderiliyor
 * ============================================================
 * Backend `ws.py` ikili çerçeveleri ham PCM olarak kabul ediyor, JSON zarfı
 * beklemiyor. Sebebi doküman 3.2'de: her ses parçasına JSON sarmak, saniyede
 * onlarca çerçevede hem cihazın CPU'sunu hem bant genişliğini boşa harcar.
 * Metin çerçeveleri kontrol olayları, ikili çerçeveler ses.
 *
 * ============================================================
 * Bellek
 * ============================================================
 * 3.5'te ölçüldü: Wi-Fi açıldıktan sonra dahili RAM'de ~62 KB kaldı. TLS
 * kullanılmıyor (ws://, wss:// değil) çünkü yerel ağda demo yapılıyor ve TLS
 * tamponları bu bütçede ciddi yer kaplardı. Üretimde wss'e geçilirse
 * buffer_size ve görev yığını yeniden ölçülmeli.
 */

#include "ws_client.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ws";

static esp_websocket_client_handle_t s_client;
static ws_geri_cagirma_t s_cb;
static ws_durum_t s_durum;
static tur_durum_t s_tur = TUR_BOSTA;

/* Gelen JSON'u tam ayrıştırmak yerine alan çekiyoruz.
 *
 * cJSON kullanmak daha temiz olurdu ama olay başına ayırma/serbest bırakma
 * yapıyor; ses akarken saniyede onlarca olay gelen bir cihazda bu bellek
 * parçalanması demek (dahili RAM'de zaten ~62 KB var). Olay sözleşmesi
 * sabit ve dar olduğu için dize araması yeterli ve öngörülebilir. */
static bool alan_cek(const char *json, const char *anahtar, char *hedef, size_t n)
{
    char desen[48];
    snprintf(desen, sizeof(desen), "\"%s\":\"", anahtar);
    const char *p = strstr(json, desen);
    if (p == NULL) {
        return false;
    }
    p += strlen(desen);

    size_t i = 0;
    while (*p != '\0' && *p != '"' && i < n - 1) {
        if (*p == '\\' && *(p + 1) != '\0') {
            p++;   /* kaçış: sonraki karakteri olduğu gibi al */
        }
        hedef[i++] = *p++;
    }
    hedef[i] = '\0';
    return true;
}

static bool tip_mi(const char *json, const char *tip)
{
    char desen[48];
    snprintf(desen, sizeof(desen), "\"type\":\"%s\"", tip);
    return strstr(json, desen) != NULL;
}

static void metin_olayi(const char *json, int uzunluk)
{
    (void)uzunluk;
    /* Backend max_tokens=400 üretiyor; Türkçe UTF-8'de bu ~1.2-1.6 KB.
     * 512 bayt, cevap uzunluk sınırı (eski kural 8) promptdan kaldırılınca
     * yetmez oldu ve cevap sessizce kesiliyordu — ekranda "metnin gerisi
     * gelmiyor" diye görünüyordu, oysa kaydırma değil veri eksikti. */
    static char metin[2048];
    static char kaynak[256];

    if (tip_mi(json, "transcript")) {
        if (alan_cek(json, "text", metin, sizeof(metin)) && s_cb.transcript) {
            s_cb.transcript(metin);
        }
    } else if (tip_mi(json, "thinking")) {
        s_tur = TUR_DUSUNUYOR;
        if (s_cb.dusunuyor) {
            s_cb.dusunuyor();
        }
    } else if (tip_mi(json, "answer_text")) {
        /* partial=true olanlar cevap büyürken geliyor; ekran her seferinde
         * üzerine yazdığı için hepsini işlemek doğru davranış. */
        const bool kaynak_yok = strstr(json, "\"no_source\":true") != NULL;
        kaynak[0] = '\0';
        alan_cek(json, "breadcrumb", kaynak, sizeof(kaynak));
        if (alan_cek(json, "text", metin, sizeof(metin)) && s_cb.cevap_metni) {
            s_cb.cevap_metni(metin, kaynak, kaynak_yok);
        }
    } else if (tip_mi(json, "done")) {
        s_tur = TUR_BOSTA;
        if (s_cb.bitti) {
            s_cb.bitti();
        }
    } else if (tip_mi(json, "cancelled")) {
        /* Backend turu kesti; tamponda kalan sesi ATMAK cihazın işi.
         * İptalden ÖNCE gönderilmiş baytlar hâlâ yolda olabilir. */
        s_tur = TUR_BOSTA;
        if (s_cb.iptal) {
            s_cb.iptal();
        }
    } else if (tip_mi(json, "error")) {
        static char kod[64];
        kod[0] = '\0';
        alan_cek(json, "code", kod, sizeof(kod));
        alan_cek(json, "message", metin, sizeof(metin));
        s_tur = TUR_HATA;
        ESP_LOGE(TAG, "backend hatasi: %s — %s", kod, metin);
        if (s_cb.hata) {
            s_cb.hata(kod, metin);
        }
    }
}

/* Parçalı çerçeve birleştirme durumu. Olay JSON'u en büyük hâlinde
 * cevap metni (2 KB) + breadcrumb + zarf kadar; 4 KB rahat yetiyor. */
static char s_json[4096];
static size_t s_json_uz;
static uint8_t s_cerceve_op;

static void olay_isleyici(void *arg, esp_event_base_t base, int32_t id, void *veri)
{
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *e = (const esp_websocket_event_data_t *)veri;

    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_durum = WS_BAGLI;
            ESP_LOGI(TAG, "backend'e baglandi");
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            s_durum = WS_BAGLANIYOR;   /* istemci kendi yeniden deniyor */
            s_tur = TUR_BOSTA;
            ESP_LOGW(TAG, "baglanti koptu");
            break;

        case WEBSOCKET_EVENT_DATA: {
            /* Yük buffer_size'ı aşarsa istemci onu PARÇA PARÇA veriyor ve
             * devam parçalarında op_code sıfırlanıyor. Türü yalnızca ilk
             * parça taşıyor; devamında hatırlanması gerekiyor, yoksa uzun
             * bir cevabın devamı hiçbir dala girmeden düşer. */
            const uint8_t op = (e->payload_offset == 0)
                             ? (uint8_t)e->op_code : s_cerceve_op;
            s_cerceve_op = op;

            if (op == 0x02) {                   /* ikili: ses */
                s_tur = TUR_KONUSUYOR;
                if (s_cb.ses && e->data_len > 0) {
                    /* Ses baytları sıraya yazıldığı için biriktirmeye gerek
                     * yok; parça geldiği gibi çalınabilir. */
                    s_cb.ses((const uint8_t *)e->data_ptr, (size_t)e->data_len);
                }
            } else if (op == 0x01) {            /* metin: olay */
                /* JSON'un yarısında dize aramak sessizce başarısız olur —
                 * alan bulunamaz, ekran güncellenmez ve hiçbir hata görünmez.
                 * Bu yüzden ayrıştırmadan önce tamamı birleştiriliyor. */
                if (e->payload_offset == 0) {
                    s_json_uz = 0;
                }
                const size_t yer = sizeof(s_json) - 1 - s_json_uz;
                const size_t alinacak = ((size_t)e->data_len < yer)
                                      ? (size_t)e->data_len : yer;
                if (alinacak < (size_t)e->data_len) {
                    ESP_LOGW(TAG, "olay tamponu doldu, %d bayt atildi",
                             (int)((size_t)e->data_len - alinacak));
                }
                memcpy(s_json + s_json_uz, e->data_ptr, alinacak);
                s_json_uz += alinacak;
                s_json[s_json_uz] = '\0';

                if (e->payload_offset + e->data_len >= e->payload_len) {
                    metin_olayi(s_json, (int)s_json_uz);
                    s_json_uz = 0;
                }
            }
            break;
        }

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "websocket hatasi");
            break;

        default:
            break;
    }
}

esp_err_t ws_baslat(const char *url, const ws_geri_cagirma_t *cb)
{
    if (url == NULL || url[0] == '\0') {
        ESP_LOGE(TAG, "backend adresi bos — NVS'te kayitli degil");
        return ESP_ERR_INVALID_ARG;
    }
    s_cb = *cb;

    const esp_websocket_client_config_t cfg = {
        .uri = url,
        /* Kopmada kendi kendine yeniden bağlanır. Demo sırasında ağ
         * dalgalanması olağan; elle müdahale gerekmemeli (R-01). */
        .reconnect_timeout_ms = 2000,
        .network_timeout_ms = 10000,
        /* 12 KB ile başlandı ("TTS parçası 8 KB, tampon büyük olsun") ama
         * ÖLÇÜM bunu eledi: dahili RAM 62 KB'dan 12.8 KB'a düştü. Büyük
         * tampon gereksiz — esp_websocket_client büyük çerçeveyi birden çok
         * DATA olayına bölerek teslim ediyor, biz de her parçayı geldiği gibi
         * I2S'e yazıyoruz. Sesin bütünlüğü tampon boyutuna bağlı değil. */
        .buffer_size = 4096,
        .task_stack = 4096,
    };

    s_client = esp_websocket_client_init(&cfg);
    if (s_client == NULL) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                  olay_isleyici, NULL));
    s_durum = WS_BAGLANIYOR;
    ESP_LOGI(TAG, "baglaniyor: %s", url);
    return esp_websocket_client_start(s_client);
}

static esp_err_t json_gonder(const char *json)
{
    if (s_client == NULL || !esp_websocket_client_is_connected(s_client)) {
        return ESP_ERR_INVALID_STATE;
    }
    const int n = esp_websocket_client_send_text(s_client, json, strlen(json),
                                                 pdMS_TO_TICKS(2000));
    return (n < 0) ? ESP_FAIL : ESP_OK;
}

esp_err_t ws_konusma_basla(const char *grade, const char *subject)
{
    char json[256];
    /* Profil dizeleri katalogdan geldiği gibi gönderiliyor — plan 0.4:
     * "12.Sınıf" biçimi birebir eşleşmeli, normalizasyon YAPILMAZ. */
    snprintf(json, sizeof(json),
             "{\"type\":\"speech_start\",\"grade\":\"%s\",\"subject\":\"%s\"}",
             grade ? grade : "", subject ? subject : "");
    const esp_err_t err = json_gonder(json);
    if (err == ESP_OK) {
        s_tur = TUR_KAYIT;
    }
    return err;
}

esp_err_t ws_ses_gonder(const uint8_t *pcm, size_t bayt)
{
    if (s_client == NULL || !esp_websocket_client_is_connected(s_client)) {
        return ESP_ERR_INVALID_STATE;
    }
    const int n = esp_websocket_client_send_bin(s_client, (const char *)pcm, bayt,
                                                pdMS_TO_TICKS(2000));
    return (n < 0) ? ESP_FAIL : ESP_OK;
}

esp_err_t ws_konusma_bitir(void)
{
    const esp_err_t err = json_gonder("{\"type\":\"speech_end\"}");
    if (err == ESP_OK) {
        s_tur = TUR_DUSUNUYOR;
    }
    return err;
}

esp_err_t ws_iptal(void)
{
    return json_gonder("{\"type\":\"cancel\"}");
}

ws_durum_t ws_durum(void) { return s_durum; }
tur_durum_t ws_tur_durum(void) { return s_tur; }
