/*
 * I2S mikrofon girişi — INMP441 (plan 3.3).
 *
 * ============================================================
 * INMP441'in iki tuzağı
 * ============================================================
 *
 * 1. **24 bit veriyi 32 bitlik yuvada gönderir.** 16 bitlik yuva
 *    yapılandırılırsa mikrofon susmaz, ama örneklerin üst bitleri kesilir ve
 *    sonuç gürültüye benzer. Bu yüzden yuva 32 bit okunuyor, sonra kaydırma
 *    ile 16 bite indiriliyor (backend PCM16 bekliyor, bkz. config.py).
 *
 * 2. **L/R pini kanalı seçer.** GND'ye bağlıysa mikrofon SOL kanaldan yayın
 *    yapar. Sürücü sağ kanalı dinliyorsa okunan her örnek SIFIR olur —
 *    yani "kod çalışıyor ama ses yok" tablosu. Sessiz kayıtta ilk bakılacak
 *    yer burasıdır (pins.h'de de not düşüldü).
 *
 * ============================================================
 * Neden PSRAM
 * ============================================================
 * V-05: "Dahili RAM ses tamponu, LVGL çerçeve tamponu ve TLS/WebSocket
 * tamponlarını aynı anda taşımakta zorlanır; ses tamponları PSRAM'e alınır."
 * 15 saniyelik kayıt = 16000 * 2 * 15 = 480 KB. Dahili RAM'de boş yer zaten
 * ~242 KB (3.2'de ölçüldü), yani seçenek değil — PSRAM şart.
 *
 * DMA tamponu ise dahili RAM'de kalmak ZORUNDA; I2S DMA PSRAM'e erişemez.
 * Bu yüzden iki katman var: küçük DMA tamponu (sürücüde, dahili) -> okunan
 * veri -> büyük kayıt tamponu (PSRAM).
 */

#include "audio_in.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "pins.h"

static const char *TAG = "audio_in";

/* Bas-konuş için üst sınır. ADR-02 gereği öğrenci düğmeyi basılı tutuyor;
 * 15 saniye bir soru için fazlasıyla yeterli, kazara basılı kalırsa da
 * belleği tüketmiyor. */
#define KAYIT_SANIYE 15
#define KAYIT_BAYT (MIC_SAMPLE_RATE * 2 * KAYIT_SANIYE)

/* Tek okumada işlenecek örnek sayısı. 32 bitlik yuvada 512 örnek = 2 KB
 * dahili tampon; küçük tutuluyor çünkü bu bellek PSRAM'e alınamıyor. */
#define OKUMA_ORNEK 512

static i2s_chan_handle_t s_rx;
static uint8_t *s_kayit;      /* PSRAM */
static size_t s_kayit_boyut;
static int32_t *s_ham;        /* dahili RAM, I2S okuma tamponu */
static float s_dc_x1, s_dc_y1;   /* DC engelleyici süzgeç durumu */
static uint8_t s_seviye;
static bool s_kayitta;

esp_err_t audio_in_init(void)
{
    /* --- I2S RX kanalı ---
     * Doküman: kayıt ve çalma İKİ AYRI I2S kanalı. Mikrofon I2S_NUM_0. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    /* 8x512 denendi (cızırtı şüphesiyle) ama cızırtıyı düzeltmedi — sebep
     * DC kayması ve kutusuz hoparlördü. 32 bitlik yuvada 8x512 = 16 KB
     * dahili RAM demek ve 3.6'da dahili RAM kritik seviyeye indi. 4x512
     * (~128 ms pay) yeterli; kazanılan 8 KB WebSocket'e gidiyor. */
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 512;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        /* 32 bitlik yuva, MONO SOL. INMP441'in L/R pini GND'de olduğu için
         * sol kanaldan yayın yapıyor; buradaki SLOT_MODE_MONO + LEFT ikilisi
         * onunla eşleşmek zorunda. */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   /* INMP441 MCLK istemez */
            .bclk = PIN_MIC_SCK,
            .ws   = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,   /* yalnızca giriş */
            .din  = PIN_MIC_SD,
            .invert_flags = { false, false, false },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));

    /* --- Tamponlar --- */
    s_kayit = heap_caps_malloc(KAYIT_BAYT, MALLOC_CAP_SPIRAM);
    if (s_kayit == NULL) {
        ESP_LOGE(TAG, "PSRAM'de %d baytlik kayit tamponu ayrilamadi", KAYIT_BAYT);
        return ESP_ERR_NO_MEM;
    }
    /* Bu tampon DMA ile doldurulmuyor, i2s_channel_read ona kopyalıyor; yine
     * de dahili RAM'de olmalı çünkü sürücü DMA'dan buraya yazıyor. */
    s_ham = heap_caps_malloc(OKUMA_ORNEK * sizeof(int32_t), MALLOC_CAP_INTERNAL);
    if (s_ham == NULL) {
        ESP_LOGE(TAG, "Dahili okuma tamponu ayrilamadi");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Mikrofon hazir: %d Hz, kayit tamponu %d bayt (PSRAM, %d sn)",
             MIC_SAMPLE_RATE, KAYIT_BAYT, KAYIT_SANIYE);
    return ESP_OK;
}

esp_err_t audio_in_start(void)
{
    s_kayit_boyut = 0;
    s_dc_x1 = 0.0f;
    s_dc_y1 = 0.0f;
    s_seviye = 0;
    s_kayitta = true;
    return i2s_channel_enable(s_rx);
}

esp_err_t audio_in_stop(void)
{
    s_kayitta = false;
    return i2s_channel_disable(s_rx);
}

size_t audio_in_pump(void)
{
    if (!s_kayitta || s_kayit_boyut >= KAYIT_BAYT) {
        return 0;
    }

    size_t okunan = 0;
    const esp_err_t err = i2s_channel_read(s_rx, s_ham,
                                           OKUMA_ORNEK * sizeof(int32_t),
                                           &okunan, 100);
    if (err != ESP_OK || okunan == 0) {
        return 0;
    }

    const size_t ornek = okunan / sizeof(int32_t);
    int16_t *hedef = (int16_t *)(s_kayit + s_kayit_boyut);
    const size_t yer = (KAYIT_BAYT - s_kayit_boyut) / sizeof(int16_t);
    const size_t yazilacak = ornek < yer ? ornek : yer;

    uint32_t tepe = 0;
    for (size_t i = 0; i < yazilacak; i++) {
        /* INMP441 24 biti 32 bitlik yuvanın ÜST tarafına hizalar. 16 bite
         * indirmek için 16 bit kaydırmak yeterli; 8 kaydırıp alt baytı almak
         * gürültülü sonuç verir. */
        /* --- DC engelleyici ---
         * INMP441 sıfır etrafında değil, sabit bir kaymayla yayın yapıyor.
         * Ham dökümde ölçüldü: sessizken örnekler -4000 ya da -2170
         * civarında salınıyordu, konuşurken -10000 ile -6000 arası — hepsi
         * negatif. Dalga biçimi düzgündü (ardışık fark ort. 281), yani
         * mikrofon okuması doğruydu; sorun yalnızca eksenin kaymasıydı.
         *
         * Bu kayma hoparlörde koniyi sürekli bir uca itiyor, gerçek ses de
         * onun üstüne biniyor: bozulma ve cızırtı. Kazancı düşürmek çözmedi,
         * çünkü kayma da onunla birlikte küçülüyor, ORAN değişmiyor.
         *
         * y[n] = x[n] - x[n-1] + R*y[n-1], tek kutuplu yüksek geçiren.
         * R=0.995 -> ~12 Hz kesim: konuşma 80 Hz'in üstünde başladığı için
         * dokunmuyor, ama DC'yi ve yavaş sürüklenmeyi siliyor. */
        const float x = (float)(s_ham[i] >> 16);
        const float y = x - s_dc_x1 + 0.995f * s_dc_y1;
        s_dc_x1 = x;
        s_dc_y1 = y;

        const int16_t s = (y > 32767.0f)    ? (int16_t)32767
                        : (y < -32768.0f)   ? (int16_t)-32768
                                            : (int16_t)y;
        hedef[i] = s;

        const uint32_t mutlak = (uint32_t)(s < 0 ? -(int32_t)s : (int32_t)s);
        if (mutlak > tepe) {
            tepe = mutlak;
        }
    }

    s_kayit_boyut += yazilacak * sizeof(int16_t);
    /* Seviye 0-100. Tepe değeri kullanılıyor (RMS değil): amaç ses kalitesini
     * ölçmek değil, "mikrofon duyuyor mu" sorusunu gözle cevaplamak. */
    s_seviye = (uint8_t)((tepe * 100) / 32768);
    return yazilacak * sizeof(int16_t);
}

void audio_in_dokum(size_t adet)
{
    /* Ham örnekleri seri porta basar.
     *
     * Neden gerekli: 3.3'te mikrofonu doğrularken yalnızca SEVİYENİN
     * konuşmayla değiştiğini gördük. Bu "enerji var" demek, dalga biçiminin
     * DOĞRU olduğunu kanıtlamıyor — bit hizalaması yanlış olsa da seviye
     * konuşmayı takip ederdi. Cızırtının sebebi mikrofon okuması mı yoksa
     * çalma tarafı mı, ancak örneklere bakarak ayrılır.
     *
     * Konuşma 16 kHz'de görece YUMUŞAK değişir: ardışık örnekler arasındaki
     * fark küçüktür. Çöp veri ise tam ölçek aralığında rastgele zıplar. */
    const int16_t *p = (const int16_t *)s_kayit;
    const size_t n = s_kayit_boyut / sizeof(int16_t);
    if (n < adet) {
        adet = n;
    }
    /* Ortadan al: baştaki ilk örnekler mikrofon oturmadan önce olabilir. */
    const size_t basla = (n > adet * 2) ? (n / 2) : 0;

    int64_t fark_toplam = 0;
    int32_t en_buyuk_fark = 0;
    for (size_t i = basla + 1; i < basla + adet; i++) {
        const int32_t d = (int32_t)p[i] - (int32_t)p[i - 1];
        const int32_t ad = d < 0 ? -d : d;
        fark_toplam += ad;
        if (ad > en_buyuk_fark) {
            en_buyuk_fark = ad;
        }
    }
    ESP_LOGW(TAG, "DOKUM ortalama|fark|=%d  en buyuk|fark|=%d",
             (int)(fark_toplam / (int64_t)(adet - 1)), (int)en_buyuk_fark);

    char satir[220];
    for (size_t blok = 0; blok < adet; blok += 16) {
        int off = 0;
        for (size_t i = blok; i < blok + 16 && i < adet; i++) {
            off += snprintf(satir + off, sizeof(satir) - off, "%6d ", p[basla + i]);
        }
        ESP_LOGW(TAG, "%s", satir);
    }
}

const uint8_t *audio_in_data(void) { return s_kayit; }
size_t audio_in_size(void) { return s_kayit_boyut; }
uint8_t audio_in_level(void) { return s_seviye; }
bool audio_in_is_full(void) { return s_kayit_boyut >= KAYIT_BAYT; }
