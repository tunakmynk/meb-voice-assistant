# Türkçe font

`display.c` bu dizindeki **`DejaVuSans.ttf`** dosyasını ikiliye gömer ve
LVGL'in `lv_tiny_ttf` motoruyla çalışma anında çizer.

**Durum: `DejaVuSans.ttf` bu dizinde mevcut** (347 KB).

Kaynak: https://dejavu-fonts.github.io/ -> `dejavu-fonts-ttf-*.zip` -> `ttf/DejaVuSans.ttf`

Dikkat: arşivin tamamı (21 font + belgeler, 8.2 MB) açılırsa ESP-IDF bileşen
dizinini kirletir. Yalnızca `DejaVuSans.ttf` gerekiyor ve **bu dizinde**
olmalı; `main/` içine değil. Dosya yoksa derleme `EMBED_FILES` adımında hata
verir.

## Neden DejaVu

- **Lisans:** serbest, gömülü üründe kullanılabilir. Arial ya da Segoe UI
  Windows'ta hazır duruyor ama ikisi de lisanslı; bir cihaza gömmek telif
  sorunu yaratır.
- **Kapsam:** Türkçe'nin tamamını taşıyor — `ç ğ ı İ ö ş ü` ve büyük
  karşılıkları. Özellikle **noktasız `ı` ve noktalı `İ`** çoğu dar kapsamlı
  fontta eksiktir; bizde bunlar ekranın her yerinde geçiyor.

Boyut ~750 KB. 8 MB flash'ta sorun değil. Küçültmek isterseniz `pyftsubset`
(fonttools) ile Latin + Türkçe aralığına indirebilirsiniz:

```
pyftsubset DejaVuSans.ttf --unicodes=U+0020-007E,U+00C0-00FF,U+011E-011F,U+0130-0131,U+015E-015F --output-file=DejaVuSans.ttf
```

## Neden lv_font_conv kullanılmadı

LVGL'in standart yolu `lv_font_conv` (Node.js) ile TTF'den `.c` üretmek.
İki sebeple seçilmedi:

1. Bu makinede node/npm yok; üretim adımı derleme hattının parçası olamazdı.
2. Karakter aralığını önceden sabitlemek gerekir. Türkçe için "ASCII + çğıİöşü"
   yeterli görünür, ama MEB kitaplarından gelen breadcrumb'larda değişik
   tırnak ve tire biçimleri de var; aralık eksik kalırsa ekranda **sessizce
   boş kutu** çıkar — fark edilmesi zor bir hata.

`lv_tiny_ttf` aralık sabitlemeyi ortadan kaldırıyor. Bedeli CPU ve önbellek
RAM'i; 320x240'lık bir arayüzde metin az olduğu için kabul edilebilir.
