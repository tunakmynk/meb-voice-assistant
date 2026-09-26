/*
 * Dokunmatik giriş — XPT2046 (plan 3.6b, Açık Soru 9).
 *
 * Ekrandan AYRI bir SPI veri yolu (SPI3) kullanır. Paylaşım, modülün SCK/SDI
 * pinlerine ikinci birer kablo takmayı gerektiriyordu ve breadboard olmadan
 * kurulamıyordu; SPI3 zaten boştaydı (pins.h'deki karar).
 *
 * Kalibrasyon şart: direnç tabanlı panelde ham ADC değerleri panelden panele
 * kayar. Kalibrasyonsuz dokunma, ekranın kenarlarında birkaç piksele kadar
 * sapar — sınıf/ders listesinde YANLIŞ SATIR seçilmesi demektir. Değerler
 * NVS'te saklanır (ADR-09 zaten NVS kullanıyor), böylece kalibrasyon bir kez
 * yapılır.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Dokunmatiği kurar ve LVGL'e giriş aygıtı olarak ekler.
 * display_init() sonrası çağrılmalı — SPI veri yolu orada kuruluyor. */
esp_err_t touch_init(void);

/* NVS'te kayıtlı kalibrasyon var mı. Yoksa kalibrasyon ekranı gösterilmeli. */
bool touch_kalibre_mi(void);

/* İki karşıt köşeye dokundurarak kalibre eder ve NVS'e yazar.
 * Bloklar: kullanıcı iki köşeye de dokunana kadar döner.
 * Ölçüm gürültülüyse ya da iki köşe birbirine çok yakınsa kaydetmeden
 * hata döndürür — saçma bir eşlemeyi "kalibre" diye kaydetmek, ekranı
 * kullanıcının nedenini anlayamayacağı şekilde bozardı. */
esp_err_t touch_kalibre_et(void);

/* Gözle doğrulama: parmağı izleyen bir nokta ve koordinat yazısı gösterir,
 * verilen süre dolunca döner. Ölçek ya da yön hatası burada tek bakışta
 * görülüyor — seçim arayüzü yazıldıktan sonra fark etmek çok daha pahalı. */
void touch_dogrula(int saniye);
