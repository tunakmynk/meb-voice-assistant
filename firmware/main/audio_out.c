/*
 * I2S hoparlör çıkışı — MAX98357A (plan 3.4).
 *
 * ============================================================
 * SD pini neden GPIO'ya bağlı (Açık Soru 10)
 * ============================================================
 * MAX98357A'da SD tek başına "kapat/aç" değil, aynı zamanda kanal seçicidir:
 *
 *     < 0.16 V   kapalı (shutdown)
 *     0.16-0.77  sağ kanal
 *     0.77-1.4   (sol+sağ)/2
 *     > 1.4 V    sol kanal
 *
 * GPIO'dan HIGH sürmek 3.3 V verir -> sol kanal, yani mono çıkışımız.
 * LOW sürmek amfiyi tamamen kapatır. Sabit VDD'ye bağlanmadı çünkü:
 *   - konuşma dışında Class-D'nin boştaki cızırtısı sessiz sınıfta duyulur
 *   - barge-in'de (ADR-04) hoparlörü anında susturmanın en kesin yolu bu;
 *     I2S tamponundaki sesin bitmesini beklemek gerekmiyor
 *
 * ============================================================
 * İki farklı örnekleme hızı
 * ============================================================
 * Mikrofon 16 kHz kaydediyor, backend TTS ise 24 kHz PCM gönderiyor
 * (settings.tts_sample_rate). Aynı I2S kanalı ikisini de çalacağı için hız
 * çalınacak kaynağa göre ayarlanıyor. Yanlış hız sesi tiz ya da pes yapar —
 * "ses geliyor ama tuhaf" tablosunda ilk bakılacak yer burasıdır.
 */

#include "audio_out.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pins.h"

static const char *TAG = "audio_out";

/* Yüksek geçiren süzgeç durumu (iki kademe, -12 dB/oktav). */
static float s_hp_x1[2], s_hp_y1[2];

static i2s_chan_handle_t s_tx;
static uint32_t s_rate;

esp_err_t audio_out_init(void)
{
    /* --- Amfi susturma pini --- */
    const gpio_config_t sd_cfg = {
        .pin_bit_mask = 1ULL << PIN_SPK_SD,
        /* INPUT_OUTPUT, sadece OUTPUT değil: OUTPUT modunda giriş tamponu
         * kapalı olduğu için gpio_get_level() pinin gerçek durumundan
         * bağımsız olarak 0 döndürür. Tanılama satırı bu yüzden yanıltıcı
         * çıktı — pinin sürüldüğünü doğrulayabilmek için geri okuma şart. */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sd_cfg));
    gpio_set_level(PIN_SPK_SD, 0);   /* açılışta kapalı */

    /* --- I2S TX kanalı ---
     * Doküman: kayıt ve çalma İKİ AYRI I2S kanalı. Hoparlör I2S_NUM_1;
     * mikrofon I2S_NUM_0'ı kullanıyor. Tek kanalı yön değiştirerek kullanmak
     * her bas-konuş geçişinde sürücüyü yeniden kurmayı gerektirirdi. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   /* tampon boşalınca sessizlik yaz, cızırtı olmasın */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    s_rate = SPK_SAMPLE_RATE;
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   /* MAX98357A MCLK istemez */
            .bclk = PIN_SPK_BCLK,
            .ws   = PIN_SPK_LRC,
            .dout = PIN_SPK_DIN,
            .din  = I2S_GPIO_UNUSED,   /* yalnızca çıkış */
            .invert_flags = { false, false, false },
        },
    };
    /* Kanal maskesi AÇIKÇA belirtiliyor. Mikrofonda bunu yapmıştım ama
     * çıkışta unutmuşum: MAX98357A, SD pini yüksekken SOL kanalı dinler.
     * ESP32 sağ kanala yazarsa amfi sessiz kalır ve HİÇBİR HATA ÜRETMEZ —
     * "kod çalışıyor, ses yok" tablosunun en sık sebebi bu. */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    ESP_LOGI(TAG, "Hoparlor hazir: %lu Hz, 16-bit mono, amfi kapali",
             (unsigned long)s_rate);
    return ESP_OK;
}

void audio_out_enable(bool on)
{
    gpio_set_level(PIN_SPK_SD, on ? 1 : 0);
}

esp_err_t audio_out_set_rate(uint32_t hz)
{
    if (hz == s_rate) {
        return ESP_OK;
    }
    ESP_ERROR_CHECK(i2s_channel_disable(s_tx));
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);
    const esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &clk);
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    if (err == ESP_OK) {
        s_rate = hz;
    }
    return err;
}

/* ~300 Hz yüksek geçiren süzgeç katsayısı.
 *
 * Ölçümle bulundu: frekans taramasında 200-800 Hz bozuk, 3200 Hz temiz
 * çıktı; hoparlörü kitap arasına koymak da düzeltti. İkisi de MEKANİK
 * bozulmaya işaret ediyor — kutusuz küçük hoparlörün baffle'ı yok, ön ve
 * arka dalgalar birbirini götürüyor ve koni bunu telafi etmek için aşırı
 * hareket ediyor. Üretemeyeceği frekansı ona göndermek bozulmadan başka
 * bir şey üretmiyor.
 *
 * 300 Hz altını atmak konuşma anlaşılırlığından hiçbir şey götürmez —
 * telefon bandı zaten 300 Hz-3.4 kHz'dir — ama koni hareketini belirgin
 * azaltır. a = RC/(RC+dt), fc=300 Hz, 16 kHz örnekleme. */
#define HP_A 0.894f

/* --- Limitleyici ---
 *
 * Sorun: genlik taramasında tepe 2000 temiz, 5000 bozuktu. Düz bir
 * zayıflatma (eski çözüm: hepsini 0.45 ile çarpmak) tepeyi güvene alıyor
 * ama sesi de o oranda kısıyordu.
 *
 * Anahtar ayrım: BOZULMAYI tepe değer üretir, DUYULAN SESİ ortalama enerji
 * belirler. Sinüs tonunda ikisi sabit oranlıdır, ama konuşmada tepeler
 * ortalamanın 4-5 katıdır. Yani konuşmayı tepe 2000'e sığdırınca ortalama
 * 400-500'de kalıyor — kulak bunu çok kısık duyuyor. Aradaki boşluk
 * kullanılabilir.
 *
 * Limitleyici sinyali ON KAZANÇ ile yükseltir, sonra yalnızca tavanı aşan
 * anlarda kazancı geri çeker. Sessiz kısımlar belirgin yükselir, tepeler
 * temiz bölgede kalır.
 *
 * Saldırı anında (tepeyi kaçırmamak için), bırakma yavaş (~100 ms) —
 * hızlı bırakma "pompalama" denen duyulur kazanç dalgalanması yapar. */
/* Ölçüm turlarıyla varılan değerler:
 *   5.0 / 3000 / 100ms  -> ses kisik kaldi, DALGALANMA duyuldu
 *   10.0 / 2200 / 300ms -> ses TEMIZ, dalgalanma gitti, ama hala kisik
 *   16.0 / 2200 / 300ms -> ayni temizlikte daha yuksek ortalama
 *
 * Ön kazancı artırmanın bedeli: sessiz anlarda limitleyicinin kazancı 1.0'a
 * döndüğü için ARKA PLAN GÜRÜLTÜSÜ de aynı oranda yükselir. TTS sesinde
 * gürültü tabanı düşük olduğu için sorun değil; mikrofon loopback'inde
 * kelimeler arasında hışırtı duyulabilir.
 *
 * ON_KAZANC ve TAVAN aynı anda ters yönde değişiyor ve bu bilinçli: daha
 * yüksek ön kazanç + daha düşük tavan = daha fazla sıkıştırma. Ortalama
 * enerji (duyulan ses) yükselirken tepeler ölçülmüş temiz bölgeye
 * (2000 civarı) iniyor. */
#define ON_KAZANC   16.0f
#define TAVAN       2200.0f

/* Bırakma süresi ~300 ms. 100 ms'de ses gözle görülür şekilde dalgalanıyordu
 * ("pompalama"): limitleyici her tepeden sonra kazancı hızla geri açıyor,
 * kulak bunu sesin sürekli açılıp kısılması olarak duyuyor. Yavaşlatmak
 * dalgalanmayı gideriyor; bedeli, güçlü bir tepeden sonra sesin normale
 * dönmesinin biraz uzun sürmesi — konuşmada fark edilmiyor. */
#define BIRAKMA     0.00014f  /* ~300 ms @ 24 kHz */

static float s_lim_kazanc = 1.0f;

static int16_t limitle(float v)
{
    const float buyukluk = v < 0.0f ? -v : v;
    const float hedef = (buyukluk > TAVAN) ? (TAVAN / buyukluk) : 1.0f;

    if (hedef < s_lim_kazanc) {
        s_lim_kazanc = hedef;                                  /* anında bas */
    } else {
        s_lim_kazanc += (hedef - s_lim_kazanc) * BIRAKMA;      /* yavaş bırak */
    }

    const float y = v * s_lim_kazanc;
    return (y > 32767.0f) ? (int16_t)32767
         : (y < -32768.0f) ? (int16_t)-32768
                           : (int16_t)y;
}

static int16_t isle(int16_t ornek)
{
    /* Sıra önemli: ÖNCE yüksek geçiren, SONRA limitleyici.
     * Tersi olsaydı limitleyici, hoparlörün zaten basamadığı alçak frekans
     * tepelerine tepki verip kazancı gereksiz yere kısardı. */
    float v = (float)ornek;
    for (int k = 0; k < 2; k++) {   /* iki kademe: -12 dB/oktav */
        const float y = HP_A * (s_hp_y1[k] + v - s_hp_x1[k]);
        s_hp_x1[k] = v;
        s_hp_y1[k] = y;
        v = y;
    }
    return limitle(v * ON_KAZANC);
}

void audio_out_hp_sifirla(void)
{
    s_hp_x1[0] = s_hp_y1[0] = s_hp_x1[1] = s_hp_y1[1] = 0.0f;
    s_lim_kazanc = 1.0f;
}

esp_err_t audio_out_write(const uint8_t *data, size_t bytes)
{
    /* Süzgeç BURADA uygulanıyor, çağıranlarda değil: hem loopback hem
     * ileride backend'den gelen TTS sesi aynı yoldan geçsin diye. */
    static int16_t tampon[512];
    const int16_t *giris = (const int16_t *)data;
    size_t kalan = bytes / sizeof(int16_t);

    while (kalan > 0) {
        const size_t n = kalan < 512 ? kalan : 512;
        for (size_t i = 0; i < n; i++) {
            tampon[i] = isle(giris[i]);
        }
        size_t yazilan = 0;
        const esp_err_t err = i2s_channel_write(s_tx, tampon,
                                                n * sizeof(int16_t),
                                                &yazilan, portMAX_DELAY);
        if (err != ESP_OK) {
            return err;
        }
        giris += n;
        kalan -= n;
    }
    return ESP_OK;
}

void audio_out_flush(void)
{
    /* Son parçanın DMA tamponundan çıkması için kısa bekleme; hemen
     * kapatılırsa cümlenin sonu kesilir. */
    vTaskDelay(pdMS_TO_TICKS(50));
    audio_out_enable(false);
}

void audio_out_ezgi_genlik(int16_t tepe)
{
    /* Mikrofonu tamamen devre dışı bırakan çalma testi.
     *
     * Loopback cızırtılı geliyor ama suçlunun mikrofon mu çalma yolu mu
     * olduğu belirsiz. Cihazın KENDİ ürettiği temiz bir sinyali çalmak bunu
     * ayırır: ezgi temiz duyuluyorsa çalma yolu sağlam, sorun mikrofonda.
     *
     * Tek ton yerine ezgi seçildi — sabit bir sinüs hafif bozulmayı gizler,
     * değişen notalar ise nota geçişlerinde ve farklı frekanslarda bozulmayı
     * duyulur kılar.
     *
     * Loopback ile AYNI örnekleme hızında (16 kHz) çalıyor ki aynı yolu
     * sınayalım; 24 kHz'de çalmak farklı bir yapılandırmayı test etmek olurdu.
     */
    static const uint16_t notalar[] = {
        262, 294, 330, 349, 392, 440, 494, 523,   /* çıkan diziler */
        494, 440, 392, 349, 330, 294, 262,        /* inen */
    };
    const size_t nota_adet = sizeof(notalar) / sizeof(notalar[0]);
    const uint32_t nota_ms = 280;

    audio_out_set_rate(MIC_SAMPLE_RATE);

    const size_t parca = 512;
    int16_t *buf = heap_caps_malloc(parca * sizeof(int16_t), MALLOC_CAP_DMA);
    if (buf == NULL) {
        ESP_LOGE(TAG, "ezgi tamponu ayrilamadi");
        return;
    }

    ESP_LOGW(TAG, "EZGI genlik=%d (tam olcegin %%%d'i)",
             (int)tepe, (int)((tepe * 100) / 32767));
    audio_out_enable(true);

    for (size_t n = 0; n < nota_adet; n++) {
        const size_t toplam = (MIC_SAMPLE_RATE * nota_ms) / 1000;
        /* Nota başında/sonunda 5 ms yumuşatma: sert kesim tıklama üretir ve
         * bunu bozulmayla karıştırırdık. */
        const size_t fade = MIC_SAMPLE_RATE / 200;
        size_t yazilan = 0;
        uint32_t faz = 0;

        while (yazilan < toplam) {
            const size_t adet = (toplam - yazilan) < parca ? (toplam - yazilan) : parca;
            for (size_t i = 0; i < adet; i++) {
                const size_t k = yazilan + i;
                float zarf = 1.0f;
                if (k < fade) {
                    zarf = (float)k / (float)fade;
                } else if (k > toplam - fade) {
                    zarf = (float)(toplam - k) / (float)fade;
                }
                const float a = (2.0f * (float)M_PI * (float)notalar[n] * (float)faz)
                                / (float)MIC_SAMPLE_RATE;
                buf[i] = (int16_t)(sinf(a) * (float)tepe * zarf);
                faz++;
            }
            audio_out_write((const uint8_t *)buf, adet * sizeof(int16_t));
            yazilan += adet;
        }
    }

    heap_caps_free(buf);
    audio_out_flush();
}

void audio_out_frekans_taramasi(void)
{
    /* FREKANS taraması — bozulma mekanik mi elektriksel mi?
     *
     * Elenenler: aşırı sürme değil (5000 genlikte güç ~0.04 W, hoparlör
     * 1.5 W), bit hizalaması değil (dört çerçeve biçimi de bozuk), mikrofon
     * değil (ezgi cihazın kendi ürettiği sinyal).
     *
     * Ayırt edici: MEKANİK bozulma frekansa bağlıdır — hoparlörün kendi
     * rezonansları, konisinin esnemesi, çerçevesinin zemine vurması belirli
     * frekanslarda ortaya çıkar. ELEKTRİKSEL bozulma (kırpılma, besleme
     * çökmesi) tüm frekanslarda benzer görünür.
     *
     * Hepsi aynı genlikte (8000), tek değişken frekans. */
    static const uint16_t frekanslar[] = { 200, 400, 800, 1600, 3200 };

    audio_out_set_rate(MIC_SAMPLE_RATE);
    for (size_t i = 0; i < sizeof(frekanslar) / sizeof(frekanslar[0]); i++) {
        ESP_LOGW(TAG, "FREKANS %u/5: %u Hz", (unsigned)(i + 1),
                 (unsigned)frekanslar[i]);
        audio_out_tone_amp(frekanslar[i], 2000, 8000);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    ESP_LOGW(TAG, "FREKANS taramasi bitti — hangileri temizdi?");
}

void audio_out_cerceve_taramasi(void)
{
    /* ÇERÇEVE BİÇİMİ taraması.
     *
     * Genlik taraması şunu gösterdi: 2000 (%6) temiz, 5000 (%15) ve üstü
     * bozuk. Güç hesabı bunun aşırı sürme OLMADIĞINI söylüyor — 5000
     * genlikte hoparlöre giden güç ~0.04 W, hoparlör 1.5 W.
     *
     * Bozulmanın genlikle orantılı olması ve çok düşük seviyede başlaması
     * BİT HİZALAMASINA işaret ediyor: veri bir bit kaymışsa küçük
     * genliklerde üst bitler işaret uzantısıdır ve fark etmez; genlik
     * büyüdükçe üst bitler gerçek veri taşır ve bozulma patlar.
     *
     * Dört çerçeve biçimi, hepsi aynı genlikte (12000). */
    struct { const char *ad; i2s_std_slot_config_t slot; } secenekler[] = {
        { "Philips 16bit/16slot MONO",
          I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                              I2S_SLOT_MODE_MONO) },
        { "Philips 16bit/32slot MONO",
          I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                              I2S_SLOT_MODE_MONO) },
        { "MSB-justified 16bit MONO",
          I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                          I2S_SLOT_MODE_MONO) },
        { "Philips 16bit STEREO",
          I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                              I2S_SLOT_MODE_STEREO) },
    };
    /* 2. seçenekte yuva genişliği elle 32'ye çekiliyor: bazı amfiler 16 bit
     * veriyi 32 bitlik yuvada bekler ve aradaki fark tam olarak bit kayması
     * gibi davranır. */
    secenekler[1].slot.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    for (size_t i = 0; i < 4; i++) {
        ESP_ERROR_CHECK(i2s_channel_disable(s_tx));
        ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(s_tx, &secenekler[i].slot));
        ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
        ESP_LOGW(TAG, "CERCEVE %u/4: %s", (unsigned)(i + 1), secenekler[i].ad);
        audio_out_ezgi_genlik(12000);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
    ESP_LOGW(TAG, "CERCEVE taramasi bitti — kacinci temizdi?");
}

void audio_out_ezgi(void)
{
    /* Genlik taraması. Ezgi çalıyor ama NOTALARIN ÜSTÜNDE bozulma var —
     * fon gürültüsü değil. Bu, aşırı sürmenin/kırpılmanın imzası.
     *
     * Hesap: MAX98357A 5V beslemede 8 ohm'a ~1.8 W verebiliyor, hoparlör ise
     * 1.5 W. Yani amfi hoparlörün taşıyabileceğinden fazlasını sürebiliyor.
     * Küçük kağıt konili hoparlörler ayrıca kendi sınırlarında da bozar.
     *
     * Dört genlikte sırayla çalıp hangisinin temiz olduğunu buluyoruz;
     * tek tek denemek yerine tarama, ekran yönünde ve I2S kanalında işe
     * yarayan yöntemin aynısı. */
    static const int16_t genlikler[] = { 2000, 5000, 12000, 24000 };
    for (size_t i = 0; i < sizeof(genlikler) / sizeof(genlikler[0]); i++) {
        ESP_LOGW(TAG, "--- TARAMA %u/4 ---", (unsigned)(i + 1));
        audio_out_ezgi_genlik(genlikler[i]);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
    ESP_LOGW(TAG, "TARAMA bitti — kacinci temizdi?");
}

void audio_out_tik_testi(void)
{
    /* I2S'i TAMAMEN devre dışı bırakan test.
     *
     * Ton da loopback de sessiz kaldı; SD pininin sürüldüğü doğrulandı ama
     * amfinin BESLENDİĞİ ve hoparlörün ona BAĞLI olduğu hiç sınanmadı.
     * Bu üçünü I2S'ten ayırmanın yolu: SD pinini hızlıca açıp kapatmak.
     *
     * Class-D amfi açılıp kapanırken çıkışında ani bir seviye değişimi olur;
     * beslenen bir amfiye bağlı hoparlör bunu "tık" olarak duyurur. Hiçbir
     * ses verisi göndermeden.
     *
     *   tık duyuluyorsa  -> besleme, GND, SD ve hoparlör bağlantısı SAĞLAM,
     *                       sorun yalnızca I2S veri hattında (BCLK/LRC/DIN)
     *   tık duyulmuyorsa -> amfi beslenmiyor, GND ortak değil ya da hoparlör
     *                       amfiye gerçekten bağlı değil
     */
    ESP_LOGW(TAG, "TIK TESTI: 8 kez ac/kapa — hoparlorden tik sesi gelmeli");
    for (int i = 0; i < 8; i++) {
        audio_out_enable(true);
        vTaskDelay(pdMS_TO_TICKS(250));
        audio_out_enable(false);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    ESP_LOGW(TAG, "TIK TESTI bitti — tik duydunuz mu?");
}

void audio_out_diag(void)
{
    /* Hoparlörden hiç ses gelmedi. Ton da gelmediği için mikrofon elendi,
     * sorun çıkış tarafında. Tek tek denemek yerine üç kanal seçeneğini
     * sırayla çalıyoruz — ekran yönünde işe yarayan yöntemin aynısı.
     *
     * Ayrıca SD pininin gerçekten sürüldüğü log'lanıyor: pin boşta ya da
     * 0.16 V altındaysa amfi tamamen kapalı durur, sessizce. */
    const struct { const char *ad; i2s_std_slot_mask_t maske; } secenekler[] = {
        {"SOL",  I2S_STD_SLOT_LEFT},
        {"SAG",  I2S_STD_SLOT_RIGHT},
        {"IKISI", I2S_STD_SLOT_BOTH},
    };

    audio_out_enable(true);
    ESP_LOGW(TAG, "SD pini (GPIO %d) seviyesi: %d  (1 olmali)",
             PIN_SPK_SD, gpio_get_level(PIN_SPK_SD));

    for (size_t i = 0; i < sizeof(secenekler) / sizeof(secenekler[0]); i++) {
        ESP_ERROR_CHECK(i2s_channel_disable(s_tx));
        i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
        slot.slot_mask = secenekler[i].maske;
        ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(s_tx, &slot));
        ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

        ESP_LOGW(TAG, "TANI %u/3: kanal=%s — simdi ses gelmeli",
                 (unsigned)(i + 1), secenekler[i].ad);
        /* Genlik tam ölçeğe yakın (28000/32767). Tanılama sırasında kırpılma
         * sorun değil; amaç duyulup duyulmadığını anlamak. */
        audio_out_tone_amp(1000, 1200, 28000);
        vTaskDelay(pdMS_TO_TICKS(600));
    }

    /* Varsayılana dön */
    ESP_ERROR_CHECK(i2s_channel_disable(s_tx));
    i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    slot.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(s_tx, &slot));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_LOGW(TAG, "TANI bitti — hangi numarada ses geldi?");
}

void audio_out_tone_amp(uint32_t hz, uint32_t ms, int16_t genlik)
{
    /* Hoparlör yolunu mikrofondan BAĞIMSIZ doğrular. Loopback testi ikisini
     * birden kullandığı için, ses gelmediğinde hangisinin suçlu olduğunu
     * söyleyemez; bu ton onu ayırır. */
    const size_t ornek = (s_rate * ms) / 1000;
    const size_t parca = 512;
    int16_t *buf = heap_caps_malloc(parca * sizeof(int16_t), MALLOC_CAP_DMA);
    if (buf == NULL) {
        ESP_LOGE(TAG, "ton tamponu ayrilamadi");
        return;
    }

    ESP_LOGI(TAG, "ton: %lu Hz, %lu ms, genlik %d",
             (unsigned long)hz, (unsigned long)ms, (int)genlik);
    audio_out_enable(true);

    size_t kalan = ornek;
    uint32_t faz = 0;
    while (kalan > 0) {
        const size_t n = kalan < parca ? kalan : parca;
        for (size_t i = 0; i < n; i++) {
            const float a = (2.0f * (float)M_PI * (float)hz * (float)faz) / (float)s_rate;
            /* Genlik tam ölçeğin ~%30'u: kırpılma olmadan net duyulur. */
            buf[i] = (int16_t)(sinf(a) * (float)genlik);
            faz++;
        }
        audio_out_write((const uint8_t *)buf, n * sizeof(int16_t));
        kalan -= n;
    }

    heap_caps_free(buf);
    audio_out_flush();
}


void audio_out_test_tone(uint32_t hz, uint32_t ms)
{
    audio_out_tone_amp(hz, ms, 9800);
}
