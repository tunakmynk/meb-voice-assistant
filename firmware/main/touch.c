/*
 * Dokunmatik giriş — XPT2046 (plan 3.6b, Açık Soru 9).
 *
 * ============================================================
 * Kendi SPI veri yolu (SPI3)
 * ============================================================
 * Ekranla paylaşmak yerine ayrı veri yolu kullanılıyor. Paylaşım, modülün
 * SCK/SDI pinlerine ikinci birer kablo takmayı gerektiriyordu; breadboard
 * olmadan bu fiziksel olarak mümkün değil (pins.h'deki gerekçe). SPI3 boşta
 * durduğu için ayırmanın maliyeti yok.
 *
 * Yan faydası: ekran 10 MHz'de kalabiliyor. XPT2046 ancak ~2 MHz'e kadar
 * güvenilir okur; paylaşılan bir yolda bu iki hızı aynı anda tutturmak
 * aygıt başına ayar gerektirirdi.
 *
 * ============================================================
 * Koordinat dönüşümü — neden sürücününkini kullanmıyoruz
 * ============================================================
 * Sürücünün CONFIG_XPT2046_CONVERT_ADC_TO_COORDS seçeneği ham ADC'yi
 * (ham / 4096) * genişlik diye eşliyor. Bu, panelin ADC aralığının TAMAMINI
 * kullandığını varsayar; gerçekte panel ~200-3900 arasında okur ve kayma
 * panelden panele değişir. İlk donanım ölçümü bunu doğruladı: sol üst köşeye
 * basınca (192, 271), sağ alt köşeye basınca (44, 49) geldi — hem ölçek
 * yanlıştı hem de iki eksen de tersti.
 *
 * Bu yüzden sürücü ham ADC döndürüyor (sdkconfig.defaults'ta seçenek kapalı)
 * ve dönüşümü process_coordinates içinde biz yapıyoruz. Çerçeve bu geri
 * çağrımı mirror/swap düzeltmesinden ÖNCE çağırıyor, yani doğru yer burası.
 *
 * YÖN BAYRAKLARI KULLANILMIYOR. İki karşıt köşeden kurulan doğrusal eşleme,
 * ham_x1 > ham_x2 olduğunda ters paneli işaretli aritmetikle kendiliğinden
 * düzeltiyor. mirror_x/mirror_y'yi tahmin etmeye çalışmak, kalibrasyonun
 * zaten ölçtüğü bilgiyi ikinci kez ve elle kodlamak olurdu.
 *
 * ============================================================
 * Neden kalibrasyon şart
 * ============================================================
 * Kalibrasyonsuz sapma ekranın ortasında küçük, KENARLARDA büyük olur.
 * Arayüzümüz liste tabanlı (sınıf ve ders satırları); kenardaki bir sapma
 * yanlış satırın seçilmesi demek ve kullanıcı bunu fark etmez — yanlış
 * dersten cevap alır.
 */

#include "touch.h"

#include <stdlib.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "display.h"
#include "pins.h"

static const char *TAG = "touch";

#define TOUCH_SPI_HOST SPI3_HOST
#define EKRAN_G 240
#define EKRAN_Y 320

#define NVS_AD "meb"
#define ANAHTAR_KAL "touch_kal"

/* Biçim sürümü. Artırmak eski kayıtları otomatik geçersiz kılıyor — ilk
 * donanım denemesinde NVS'e yazılan (ekran koordinatı cinsinden, dolayısıyla
 * anlamsız) kayıt böyle eleniyor. */
#define KAL_SURUM 2

/* Hedef noktaların kenardan uzaklığı. Tam köşe kullanılmıyor: panelin en uç
 * birkaç pikseli çoğu üründe ölü bölge ve orada alınan ölçüm eşlemeyi bozar. */
#define KENAR 24

/* İki köşe ham ADC olarak en az bu kadar ayrılmalı. Daha yakınsa ya aynı
 * yere iki kez basılmış ya da okumalar gürültü — böyle bir eşleme kabul
 * edilirse ekran kullanılamaz hale gelir, üstelik "kalibreyim" diyerek. */
#define EN_AZ_ACIKLIK 400

#define ORNEK_SAYISI 16
#define EN_AZ_ORNEK 5

/* --- Gürültü süzgeci ---
 * Sürücü okuma başına 5 örnek ORTALIYOR, ama ortalama aykırı değeri reddetmez;
 * beşliden biri sıçrarsa sonuç da kayar. Donanım denemesinde görülen
 * "dokunmadığım yere gitti" davranışı bu.
 *
 * Sıçrama eşiği ham ADC sayımı cinsinden. Ölçülen panel aralığı x'te 2404,
 * y'de 2832 sayım; ekran 240x320 olduğuna göre piksel başına ~10 ve ~8.9
 * sayım düşüyor, yani 300 sayım kabaca 30 piksel. */
#define SICRAMA 300

/* Bu süreden uzun sessizlikten sonraki okuma YENİ bir basış sayılıyor.
 * LVGL ~30 ms'de bir yokluyor; 150 ms, basış içindeki tek bir kayıp okumayı
 * yeni basış sanmayacak kadar geniş, iki ayrı dokunuşu ayırt edecek kadar dar. */
#define YENI_BASIS_US 150000

typedef struct {
    uint16_t surum;
    int16_t ham_x1, ham_y1;   /* sol üst hedefte okunan ham ADC */
    int16_t ham_x2, ham_y2;   /* sağ alt hedefte okunan ham ADC */
    int16_t px1, py1;         /* o hedeflerin piksel konumu */
    int16_t px2, py2;
} kalibrasyon_t;

static esp_lcd_touch_handle_t s_tp;
static lv_indev_t *s_indev;

static kalibrasyon_t s_kal;
static bool s_kal_var;

/* process_coordinates'in yakaladığı son ham değer. Kalibrasyon yordamı ham
 * ADC'ye ihtiyaç duyuyor ama sürücü onu doğrudan sunmuyor; bu geri çağrım
 * ham veriyi gören tek nokta. */
static volatile int16_t s_ham_x, s_ham_y;
static volatile bool s_ham_taze;

/* Süzgeç durumu */
static int64_t s_son_us;
static int16_t s_ref_x, s_ref_y;     /* son KABUL EDİLEN nokta */
static int16_t s_aday_x, s_aday_y;   /* doğrulanmayı bekleyen sıçrama */
static bool s_aday_var;

/* ---------------- NVS ---------------- */

static void kal_oku(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_AD, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    kalibrasyon_t k;
    size_t n = sizeof(k);
    const esp_err_t r = nvs_get_blob(h, ANAHTAR_KAL, &k, &n);
    nvs_close(h);

    if (r != ESP_OK || n != sizeof(k) || k.surum != KAL_SURUM) {
        ESP_LOGI(TAG, "kayitli kalibrasyon yok (ya da eski surum)");
        return;
    }
    s_kal = k;
    s_kal_var = true;
    ESP_LOGI(TAG, "kalibrasyon NVS'ten: ham x %d->%d  y %d->%d",
             k.ham_x1, k.ham_x2, k.ham_y1, k.ham_y2);
}

static void kal_yaz(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_AD, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS acilamadi, kalibrasyon kaydedilmedi");
        return;
    }
    nvs_set_blob(h, ANAHTAR_KAL, &s_kal, sizeof(s_kal));
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- Ham ADC -> piksel ---------------- */

/* İki noktadan doğrusal eşleme. h2 < h1 olabilir (ters eksen); int32 ile
 * işaretli hesaplandığı için ayrıca bir şey yapmak gerekmiyor. */
static uint16_t esle(int16_t ham, int16_t h1, int16_t p1, int16_t h2, int16_t p2,
                     int16_t ust)
{
    const int32_t bolen = (int32_t)h2 - (int32_t)h1;
    if (bolen == 0) {
        return (uint16_t)p1;
    }
    int32_t p = (int32_t)p1
              + ((int32_t)ham - (int32_t)h1) * ((int32_t)p2 - (int32_t)p1) / bolen;
    if (p < 0) {
        p = 0;
    }
    if (p > ust) {
        p = ust;
    }
    return (uint16_t)p;
}

static bool yakin(int16_t ax, int16_t ay, int16_t bx, int16_t by)
{
    return abs((int)ax - (int)bx) <= SICRAMA && abs((int)ay - (int)by) <= SICRAMA;
}

/* Okumayı kabul mü etmeli. false dönerse çağıran, dokunma yokmuş gibi davranır. */
static bool ayikla(int16_t hx, int16_t hy)
{
    const int64_t simdi = esp_timer_get_time();
    const bool yeni_basis = (simdi - s_son_us) > YENI_BASIS_US;
    s_son_us = simdi;

    if (yeni_basis) {
        /* Basışın İLK okuması her zaman yutuluyor: parmak tam oturmadan
         * basınç düşük olur ve XPT2046 en gürültülü değerleri orada üretir.
         * Bir yoklama gecikmesi (~30 ms) kullanıcıya hissettirmiyor. */
        s_ref_x = hx;
        s_ref_y = hy;
        s_aday_var = false;
        return false;
    }

    if (yakin(hx, hy, s_ref_x, s_ref_y)) {
        s_ref_x = hx;
        s_ref_y = hy;
        s_aday_var = false;
        return true;
    }

    /* Referanstan uzak. Bu tek seferlik gürültü de olabilir, hızlı kaydırma
     * da; ikisini tek okumadan ayırt etmek mümkün değil. Bir sonraki okumayı
     * bekliyoruz: gürültüyse gerçek konuma geri döner, gerçek hareketse aynı
     * yeni bölgede devam eder. Böylece gürültü tamamen eleniyor, hızlı
     * kaydırma yalnızca tek okuma gecikiyor. */
    if (s_aday_var && yakin(hx, hy, s_aday_x, s_aday_y)) {
        s_ref_x = hx;
        s_ref_y = hy;
        s_aday_var = false;
        return true;
    }
    s_aday_x = hx;
    s_aday_y = hy;
    s_aday_var = true;
    return false;
}

static void koordinat_isle(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                           uint16_t *guc, uint8_t *adet, uint8_t azami)
{
    (void)tp; (void)guc; (void)azami;
    if (adet == NULL || *adet == 0) {
        return;
    }

    /* Kalibrasyon yordamı için ham değeri sakla — dönüşümden önce. */
    s_ham_x = (int16_t)x[0];
    s_ham_y = (int16_t)y[0];
    s_ham_taze = true;

    if (!s_kal_var) {
        return;   /* kalibre değilken ham değer geçsin; süzgeç de devre dışı */
    }

    if (!ayikla((int16_t)x[0], (int16_t)y[0])) {
        *adet = 0;   /* çağırana dokunma yokmuş gibi görünür */
        return;
    }

    for (uint8_t i = 0; i < *adet; i++) {
        x[i] = esle((int16_t)x[i], s_kal.ham_x1, s_kal.px1,
                    s_kal.ham_x2, s_kal.px2, EKRAN_G - 1);
        y[i] = esle((int16_t)y[i], s_kal.ham_y1, s_kal.py1,
                    s_kal.ham_y2, s_kal.py2, EKRAN_Y - 1);
    }
}

/* ---------------- Kurulum ---------------- */

esp_err_t touch_init(void)
{
    kal_oku();

    /* XPT2046 komutları 3 baytlık; max_transfer_sz'yi küçük tutmak DMA
     * tanımlayıcılarının boşuna dahili RAM yemesini önlüyor (dahili RAM bu
     * projede dar). */
    const spi_bus_config_t bus = {
        .sclk_io_num = PIN_TOUCH_SCK,
        .mosi_io_num = PIN_TOUCH_MOSI,
        .miso_io_num = PIN_TOUCH_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TOUCH_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
    /* XPT2046 ~2 MHz üstünde güvenilir okumuyor. */
    io_cfg.pclk_hz = 2 * 1000 * 1000;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)TOUCH_SPI_HOST, &io_cfg, &tp_io));

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = EKRAN_G,
        .y_max = EKRAN_Y,
        .rst_gpio_num = GPIO_NUM_NC,
        /* T_IRQ bağlı ama sürücüye VERİLMİYOR — bilerek. esp_lvgl_port,
         * int_gpio_num dolu olduğunda LVGL'i LV_INDEV_MODE_EVENT'e alıyor:
         * artık hiç yoklamıyor, yalnızca PENIRQ kesmesinde okuyor. XPT2046 ise
         * PENIRQ'yu ancak CONFIG_XPT2046_INTERRUPT_MODE açıkken sürüyor.
         * İkisi birleşince LVGL hiç okumuyordu; dokunma çalışıyor ama ekranda
         * hiçbir tepki yok (ilk donanım denemesinde tam olarak bu oldu).
         *
         * Kesme kipini açmak yerine yoklama seçildi: kesme yalnızca dokunma
         * anında tetiklenirse parmağın kalktığı fark edilmez ve arayüz
         * "basılı" kalır. Yoklamanın bedeli 30 ms'de bir 3 baytlık SPI
         * okuması — bu cihazda ölçülemeyecek kadar küçük. */
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        /* Yön bayrakları kapalı — dosya başındaki gerekçe: eksen yönünü
         * kalibrasyon eşlemesi zaten taşıyor. */
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        .process_coordinates = koordinat_isle,
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &s_tp));

    const lvgl_port_touch_cfg_t port_cfg = {
        .disp = lv_display_get_default(),
        .handle = s_tp,
    };
    s_indev = lvgl_port_add_touch(&port_cfg);
    if (s_indev == NULL) {
        ESP_LOGE(TAG, "LVGL giris aygiti eklenemedi");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Dokunmatik hazir%s", s_kal_var ? "" : " (KALIBRE DEGIL)");
    return ESP_OK;
}

bool touch_kalibre_mi(void)
{
    return s_kal_var;
}

/* ---------------- Kalibrasyon ekranı ---------------- */

static void hedef_ciz(int hedef_x, int hedef_y, const char *yazi)
{
    if (!display_lock(-1)) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F1115), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *nokta = lv_obj_create(scr);
    lv_obj_set_size(nokta, 22, 22);
    lv_obj_set_style_radius(nokta, 11, LV_PART_MAIN);
    lv_obj_set_style_bg_color(nokta, lv_color_hex(0xE0483D), LV_PART_MAIN);
    lv_obj_set_style_border_width(nokta, 0, LV_PART_MAIN);
    lv_obj_set_pos(nokta, hedef_x - 11, hedef_y - 11);

    lv_obj_t *bilgi = lv_label_create(scr);
    lv_label_set_text(bilgi, yazi);
    lv_obj_set_style_text_font(bilgi, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(bilgi, lv_color_hex(0xE6E8EE), LV_PART_MAIN);
    lv_label_set_long_mode(bilgi, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(bilgi, EKRAN_G - 40);
    lv_obj_set_style_text_align(bilgi, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(bilgi, LV_ALIGN_CENTER, 0, 0);
    display_unlock();
}

/* Tek bir okuma dener; dokunma varsa ham değeri yazar. */
static bool ham_oku(int16_t *hx, int16_t *hy)
{
    uint16_t x[1], y[1];
    uint8_t adet = 0;
    s_ham_taze = false;
    esp_lcd_touch_read_data(s_tp);
    if (!esp_lcd_touch_get_coordinates(s_tp, x, y, NULL, &adet, 1) || adet == 0) {
        return false;
    }
    if (!s_ham_taze) {
        return false;
    }
    *hx = s_ham_x;
    *hy = s_ham_y;
    return true;
}

static int16_t ortanca(int16_t *v, int n)
{
    /* Küçük dizi; yerinde ekleme sıralaması yeterli. */
    for (int i = 1; i < n; i++) {
        const int16_t k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > k) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = k;
    }
    return v[n / 2];
}

/* Bir köşeyi ölçer. Tek örnek yerine ORTANCA alınıyor: parmağın ilk değdiği
 * an basınç düşük olduğu için XPT2046 orada en gürültülü okumayı yapıyor ve
 * ortanca bu tür uç değerleri ortalamadan daha iyi eliyor. */
static bool kose_olc(int hedef_x, int hedef_y, const char *yazi,
                     int16_t *ham_x, int16_t *ham_y)
{
    hedef_ciz(hedef_x, hedef_y, yazi);

    int16_t ox[ORNEK_SAYISI], oy[ORNEK_SAYISI];
    int n = 0;
    int16_t hx, hy;

    /* 1) Dokunmayı bekle (en fazla ~40 sn). */
    int i = 0;
    while (!ham_oku(&hx, &hy)) {
        if (++i > 2000) {
            ESP_LOGE(TAG, "kalibrasyon: dokunma gelmedi");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* 2) Basılı tutulduğu sürece örnekle. */
    ox[n] = hx; oy[n] = hy; n++;
    for (int k = 0; k < ORNEK_SAYISI * 3 && n < ORNEK_SAYISI; k++) {
        if (ham_oku(&hx, &hy)) {
            ox[n] = hx; oy[n] = hy; n++;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    if (n < EN_AZ_ORNEK) {
        ESP_LOGW(TAG, "kalibrasyon: yalnizca %d ornek, cok kisa dokunuldu", n);
        return false;
    }

    *ham_x = ortanca(ox, n);
    *ham_y = ortanca(oy, n);
    ESP_LOGI(TAG, "kose (%d,%d): ham (%d,%d), %d ornek",
             hedef_x, hedef_y, *ham_x, *ham_y, n);

    /* 3) Parmak kalksın — yoksa aynı basış ikinci köşe sanılır. */
    int bos = 0;
    for (int k = 0; k < 300 && bos < 5; k++) {
        bos = ham_oku(&hx, &hy) ? 0 : bos + 1;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

esp_err_t touch_kalibre_et(void)
{
    ESP_LOGW(TAG, "kalibrasyon basliyor");

    /* Ölçüm sırasında ham değer geçsin. */
    const bool onceki = s_kal_var;
    s_kal_var = false;

    /* LVGL'in giriş görevi de aynı paneli yokluyor. Kapatılmazsa bizim
     * okumamızla arasında yarış oluşur ve hangi örneğin yakalandığı belirsiz
     * hale gelir. */
    if (display_lock(-1)) {
        lv_indev_enable(s_indev, false);
        display_unlock();
    }

    kalibrasyon_t k = {
        .surum = KAL_SURUM,
        .px1 = KENAR,           .py1 = KENAR,
        .px2 = EKRAN_G - KENAR, .py2 = EKRAN_Y - KENAR,
    };

    esp_err_t sonuc = ESP_OK;
    if (!kose_olc(k.px1, k.py1, "Kirmizi noktaya basili tutun", &k.ham_x1, &k.ham_y1) ||
        !kose_olc(k.px2, k.py2, "Simdi bu noktaya basili tutun", &k.ham_x2, &k.ham_y2)) {
        sonuc = ESP_ERR_TIMEOUT;
    } else {
        const int ax = abs((int)k.ham_x2 - (int)k.ham_x1);
        const int ay = abs((int)k.ham_y2 - (int)k.ham_y1);
        if (ax < EN_AZ_ACIKLIK || ay < EN_AZ_ACIKLIK) {
            ESP_LOGE(TAG, "kalibrasyon reddedildi: aciklik x=%d y=%d (en az %d)",
                     ax, ay, EN_AZ_ACIKLIK);
            sonuc = ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (display_lock(-1)) {
        lv_indev_enable(s_indev, true);
        display_unlock();
    }

    if (sonuc != ESP_OK) {
        s_kal_var = onceki;
        return sonuc;
    }

    s_kal = k;
    s_kal_var = true;
    kal_yaz();

    ESP_LOGW(TAG, "kalibrasyon kaydedildi: ham x %d->%d  y %d->%d",
             k.ham_x1, k.ham_x2, k.ham_y1, k.ham_y2);
    return ESP_OK;
}

/* ---------------- Gözle doğrulama ---------------- */

static lv_obj_t *s_iz;
static lv_obj_t *s_koord;

/* Dokunmayı LVGL'in kendi olayından alıyoruz; böylece doğrulama, seçim
 * arayüzünün kullanacağı yolun TAMAMINI sınıyor — sürücü, dönüşüm ve LVGL
 * giriş katmanı dahil. Paneli ayrıca yoklamak bu katmanları atlardı. */
static void dogrula_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_obj_set_pos(s_iz, p.x - 9, p.y - 9);
    lv_label_set_text_fmt(s_koord, "x %d   y %d", (int)p.x, (int)p.y);
}

void touch_dogrula(int saniye)
{
    if (!display_lock(-1)) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F1115), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Kenar çerçevesi: ölçek hatası en çok kenarlarda görünür, referans
     * olmadan "biraz kaymış" ile "doğru" ayırt edilemiyor. */
    lv_obj_t *cerceve = lv_obj_create(scr);
    lv_obj_set_size(cerceve, EKRAN_G - 2 * KENAR, EKRAN_Y - 2 * KENAR);
    lv_obj_set_pos(cerceve, KENAR, KENAR);
    lv_obj_set_style_bg_opa(cerceve, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(cerceve, lv_color_hex(0x3A4050), LV_PART_MAIN);
    lv_obj_set_style_border_width(cerceve, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(cerceve, 0, LV_PART_MAIN);
    lv_obj_remove_flag(cerceve, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *bilgi = lv_label_create(scr);
    lv_label_set_text(bilgi, "Ekrana dokunun: nokta parmagi izlemeli");
    lv_obj_set_style_text_font(bilgi, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(bilgi, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_label_set_long_mode(bilgi, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(bilgi, EKRAN_G - 2 * KENAR - 8);
    lv_obj_set_style_text_align(bilgi, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(bilgi, LV_ALIGN_TOP_MID, 0, 4);

    s_koord = lv_label_create(scr);
    lv_label_set_text(s_koord, "-");
    lv_obj_set_style_text_font(s_koord, display_font_tr(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_koord, lv_color_hex(0xE6E8EE), LV_PART_MAIN);
    lv_obj_align(s_koord, LV_ALIGN_BOTTOM_MID, 0, -4);

    s_iz = lv_obj_create(scr);
    lv_obj_set_size(s_iz, 18, 18);
    lv_obj_set_style_radius(s_iz, 9, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_iz, lv_color_hex(0x35C46A), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_iz, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_iz, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_iz, -40, -40);

    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, dogrula_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr, dogrula_cb, LV_EVENT_PRESSED, NULL);
    display_unlock();

    vTaskDelay(pdMS_TO_TICKS(saniye * 1000));

    if (display_lock(-1)) {
        lv_obj_remove_event_cb(scr, dogrula_cb);
        display_unlock();
    }
    s_iz = NULL;
    s_koord = NULL;
}
