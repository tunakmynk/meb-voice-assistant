@echo off
REM Firmware derleme. Proje kokunden:  firmware\derle.bat  [hedef]
REM
REM PATH temizligi sart: backend\.venv\Scripts PATH'in basinda duruyor ve
REM ESP-IDF'in Python'u onu secip "zaten sanal ortamdayim" diyerek duruyor.
REM Kurulum bu yuzden iki kez basarisiz oldu.

set VIRTUAL_ENV=
set PYTHONHOME=

REM Turkce Windows'ta varsayilan kod sayfasi cp1254. esptool karttan gelen
REM ham baytlari onunla cozmeye calisip "UnicodeDecodeError: charmap codec
REM can't decode byte 0x9e" ile duser. Python'u UTF-8'e zorluyoruz.
set PYTHONUTF8=1
set PYTHONIOENCODING=utf-8
set "PATH=C:\Users\tunak\AppData\Local\Python\pythoncore-3.14-64;%PATH:c:\Users\tunak\Desktop\Internship Project\backend\.venv\Scripts;=%"

call C:\Users\tunak\esp\esp-idf\export.bat || exit /b 1
cd /d "%~dp0" || exit /b 1

if "%1"=="" (
    idf.py build
) else (
    idf.py %*
)
