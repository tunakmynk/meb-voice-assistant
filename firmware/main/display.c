/*
 * Ekran + LVGL + Türkçe font (plan 3.2).
 *
 * ============================================================
 * Türkçe font — neden lv_font_conv değil
 * ============================================================
 * LVGL'in standart yolu, lv_font_conv (Node.js aracı) ile TTF'den bir .c
 * dosyası üretip gömmektir. İki sorunu var:
 *
 *   1. Bu makinede node/npm yok, yani üretim adımı derleme hattının parçası
 *      olamaz. Üretilmiş .c dosyasını elle depoya koymak, fontu değiştirmek
 *      isteyen herkesi aynı araca mahkûm eder.
 *   2. Karakter aralığını önceden sabitlemek gerekir. Türkçe için ASCII +
 *      çğıİöşü yeter gibi görünür, ama kaynak breadcrumb'larında "T.C." gibi
 *      metinler ve MEB kitaplarından gelen tırnak/tire çeşitleri de var;
 *      aralık eksik kalırsa ekranda sessizce boş kutu çıkar.
 *
 * Onun yerine LVGL 9'un lv_tiny_ttf'i kullanılıyor: TTF ikiliye gömülüyor ve
 * glifler çalışma anında çiziliyor. Aralık sabitlemeye gerek kalmıyor, node
 * bağımlılığı yok. Bedeli CPU ve önbellek RAM'i; 320x240'lık bir arayüzde
 * metin miktarı az olduğu için kabul edilebilir, önbellek de PSRAM'de.
 */

#include "display.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"

#include "pins.h"

static const char *TAG = "display";

/* Panelin GERÇEK yönü: 2.4" ILI9341 modülü 240x320 DİKEY üretilir.
 *
 * Önce 320x240 yatay varsayılıp esp_lcd_panel_swap_xy(true) çağrılmıştı ve
 * ekran bozuk çıktı. Teşhisi zorlaştıran şey şuydu: DÜZ RENKLER KUSURSUZ
 * görünüyordu. Çünkü her piksel aynı değerdeyse satır uzunluğu hatası göze
 * batmaz — 320 piksellik satır 240'lık panele taşsa da sonuç yine tek renk.
 * Hata ancak metin gibi detaylı içerikte ortaya çıkıyor ve yatay bant
 * gürültüsü üretiyor.
 *
 * Bu yüzden panel yerel yönünde bırakıldı; döndürme gerekirse LVGL
 * tarafında (lv_display_set_rotation) yapılır, panel katmanında değil. */
#define LCD_H_RES 240
#define LCD_V_RES 320
#define LCD_SPI_HOST SPI2_HOST

/* SPI saat hızı — ekranda gürültü görülürse İLK oynanacak değer.
 *
 * 40 MHz ile başlandı ve breadboard + jumper kablo düzeneğinde ekran
 * tamamen bozuk çıktı: mavi zeminde yatay beyaz gürültü bantları. Panel
 * cevap veriyordu (arka ışık, CS/DC/RST doğru), yani kablolama değil sinyal
 * bütünlüğü sorunuydu. Dupont kabloları uzun, ekransız ve empedansı
 * kontrolsüz; 40 MHz'de kenarlar bozuluyor.
 *
 * 10 MHz muhafazakâr bir başlangıç. 320x240x2 bayt = 150 KB'lık tam çerçeve
 * 10 MHz'de ~120 ms sürer; bu arayüz için fazlasıyla yeterli, çünkü sadece
 * metin güncelleniyor, video değil. Lehimli bağlantıya geçilirse kademeli
 * artırılabilir (20 -> 40 -> 80). */
#define LCD_PIXEL_CLOCK_HZ (10 * 1000 * 1000)

#define LCD_BL_LEDC_TIMER LEDC_TIMER_0
#define LCD_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BL_LEDC_DUTY_RES LEDC_TIMER_10_BIT

/* Gömülü font. EMBED_FILES adlandırma kuralı: dosya adındaki '.' ve '/'
 * '_' olur, başına _binary_ ve sonuna _start/_end gelir. */
extern const uint8_t font_ttf_start[] asm("_binary_DejaVuSans_ttf_start");
extern const uint8_t font_ttf_end[] asm("_binary_DejaVuSans_ttf_end");

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static lv_font_t *s_font_tr;

static void backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_BL_LEDC_DUTY_RES,
        .timer_num = LCD_BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const ledc_channel_config_t channel = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

void display_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    const uint32_t max_duty = (1U << LCD_BL_LEDC_DUTY_RES) - 1;
    const uint32_t duty = (max_duty * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL);
}

esp_err_t display_init(void)
{
    backlight_init();

    /* --- SPI veri yolu ---
     * Dokunmatik (XPT2046) aynı veri yolunu paylaşacak, ayrı CS ile; bu yüzden
     * veri yolu burada bir kez kuruluyor ve 3.x'te dokunmatik aynı host'a
     * ekleniyor. max_transfer_sz tek seferde gönderilecek en büyük parçayı
     * belirler: LVGL tampon boyutundan küçük olursa aktarım bölünür ve
     * yenileme gözle görülür şekilde yavaşlar. */
    const spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,   /* panelden okuma yok; SDO kablosu bağlanmıyor */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));
    s_panel = panel;

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    /* Yön ayarı BURADA YAPILMIYOR — bkz. lvgl_port_add_disp sonrası.
     * Buraya konan esp_lcd_panel_mirror çağrıları sessizce etkisiz kalıyor. */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    /* --- LVGL --- */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        /* Tampon ekranın 1/8'i. Tam çerçeve tamponu 320*240*2 = 150 KB eder ve
         * V-05'in uyardığı bellek baskısını doğrudan üretir; kısmi tampon +
         * DMA bu arayüz için fazlasıyla yeterli. */
        /* 30 satırdan 20'ye: dahili RAM kritik seviyeye indi (3.6 ölçümü).
         * Çift tamponla 30 satır 28.8 KB, 20 satır 19.2 KB — 9.6 KB kazanç.
         * Metin arayüzünde yenileme farkı gözle görülmüyor. */
        .buffer_size = LCD_H_RES * 20,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,  /* CONFIG_LV_COLOR_16_SWAP ile eşleşir */
        },
    };
    /* Tamponun NEREDE tahsis edildiğini ölç. Düz renkler doğru çizilirken
     * metnin bozulması, PSRAM önbellek/DMA tutarsızlığının klasik imzası:
     * dolgu sıralı yazdığı için önbellek doğal boşalır, metin ise dağınık
     * küçük yazmalar yapar ve DMA eski PSRAM içeriğini okur. */
    const size_t ic0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    s_disp = lvgl_port_add_disp(&disp_cfg);

    /* Panel yönü — LVGL kurulduktan SONRA uygulanmak ZORUNDA.
     *
     * lvgl_port_add_disp() panel yönünü kendi yapılandırmasına göre yeniden
     * kuruyor ve bu çağrıdan önce yapılan mirror ayarlarını eziyor. İki tur
     * bunu anlamadan kaybedildi: display_init içinde mirror(true,false) ve
     * mirror(true,true) denendi, ikisi de ekranda HİÇBİR değişiklik
     * yaratmadı — çağrı hata da vermiyor, sessizce yok sayılıyor.
     *
     * Doğru değer dört kombinasyon sırayla denenerek ölçüldü (çeyrek deseni,
     * kırmızı sol üstte olmalı): bu modülde yalnızca Y ekseni ters. */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, true));

    const size_t ic1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t ps1 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGW(TAG, "LVGL tamponu -> dahili: %d bayt | PSRAM: %d bayt",
             (int)(ic0 - ic1), (int)(ps0 - ps1));
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "LVGL ekran eklenemedi");
        return ESP_FAIL;
    }

    /* --- Türkçe font --- */
    const size_t font_size = (size_t)(font_ttf_end - font_ttf_start);
    s_font_tr = lv_tiny_ttf_create_data(font_ttf_start, font_size, 18);
    if (s_font_tr == NULL) {
        ESP_LOGE(TAG, "Turkce font yuklenemedi (%u bayt)", (unsigned)font_size);
        return ESP_FAIL;
    }

    display_set_backlight(100);
    ESP_LOGI(TAG, "Ekran hazir: %dx%d, font %u bayt",
             LCD_H_RES, LCD_V_RES, (unsigned)font_size);
    return ESP_OK;
}

static void ceyrek_ciz(uint16_t *line)
{
    const uint16_t KIRMIZI = __builtin_bswap16(0xF800);
    const uint16_t YESIL   = __builtin_bswap16(0x07E0);
    const uint16_t MAVI    = __builtin_bswap16(0x001F);
    const uint16_t BEYAZ   = __builtin_bswap16(0xFFFF);

    for (int y = 0; y < LCD_V_RES; y++) {
        const bool ust = (y < LCD_V_RES / 2);
        for (int x = 0; x < LCD_H_RES; x++) {
            const bool sol = (x < LCD_H_RES / 2);
            line[x] = ust ? (sol ? KIRMIZI : YESIL) : (sol ? MAVI : BEYAZ);
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + 1, line);
    }
}

void display_selftest(void)
{
    /* DÖRT AYNA KOMBİNASYONUNU sırayla dener.
     *
     * Neden tek tek değil: mirror(true,false) ve mirror(true,true) denendi,
     * ikisi de görüntüyü HİÇ değiştirmedi. Bu, çağrının etkisiz kaldığına
     * işaret ediyor — büyük ihtimalle lvgl_port_add_disp panel yönünü kendi
     * yapılandırmasıyla yeniden kuruyor ve benim ayarımı eziyor. Bu yüzden
     * ayna burada, LVGL kurulduktan SONRA uygulanıyor.
     *
     * Her kombinasyon 5 saniye ekranda kalır. Doğru olan, kartı konnektör
     * yukarıda tutarken KIRMIZIYI SOL ÜSTTE gösterendir.
     *
     * Dördü de aynı görünüyorsa mirror çağrısı bu sürücüde hiç işlemiyor
     * demektir; o zaman çözüm LVGL tarafında (lv_display_set_rotation). */
    uint16_t *line = heap_caps_malloc(LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (line == NULL) {
        ESP_LOGE(TAG, "SELFTEST: satir tamponu ayrilamadi");
        return;
    }

    const struct { bool x, y; } kombinasyonlar[] = {
        {false, false}, {true, false}, {false, true}, {true, true},
    };

    for (size_t i = 0; i < sizeof(kombinasyonlar) / sizeof(kombinasyonlar[0]); i++) {
        const bool mx = kombinasyonlar[i].x;
        const bool my = kombinasyonlar[i].y;
        ESP_LOGW(TAG, "SELFTEST %u/4: mirror(x=%s, y=%s)",
                 (unsigned)(i + 1), mx ? "true" : "false", my ? "true" : "false");
        esp_lcd_panel_mirror(s_panel, mx, my);
        ceyrek_ciz(line);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    heap_caps_free(line);
    ESP_LOGW(TAG, "SELFTEST bitti — hangi numarada KIRMIZI SOL USTTEYDI?");
}


const lv_font_t *display_font_tr(void)
{
    return s_font_tr;
}

bool display_lock(int timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void display_unlock(void)
{
    lvgl_port_unlock();
}
