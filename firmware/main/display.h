/*
 * Ekran + LVGL kurulumu (plan 3.2).
 *
 * R-03 cihaz doğrulama sırası: ekran -> mikrofon -> hoparlör -> ağ.
 * Ekran ilk sırada çünkü sonraki her adımın hata ayıklaması buna dayanıyor;
 * mikrofon çalışmadığında bunu ekrandan görebilmek gerekiyor.
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

/* SPI veri yolunu, ILI9341 panelini, arka ışığı ve LVGL'i kurar.
 * Dönüşten sonra LVGL kullanılabilir durumdadır. */
esp_err_t display_init(void);

/* Donanım testi: arka ışık, SPI veri yolu ve renk sırasını tek tek gösterir.
 * display_init() sonrası, LVGL ekranı çizmeden önce çağrılmalı. */
void display_selftest(void);

/* Türkçe karakter içeren font. display_init sonrası geçerlidir.
 * Doküman: "Türkçe karakter içeren bir LVGL fontu gömülmelidir." */
const lv_font_t *display_font_tr(void);

/* Arka ışık (0-100). Demo'da ekranı karartmak için; NVS'e yazılmıyor. */
void display_set_backlight(uint8_t percent);

/* LVGL'e başka görevden dokunmadan önce kilit alınmalı — lv_* çağrıları
 * iş parçacığı güvenli değildir ve WebSocket görevinden ekran güncelleyeceğiz.
 * timeout_ms = -1 sonsuz bekler. */
bool display_lock(int timeout_ms);
void display_unlock(void);
