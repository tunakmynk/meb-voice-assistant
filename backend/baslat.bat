@echo off
REM Backend baslatma. Proje kokunden:
REM
REM     backend\baslat.bat          -> normal (demo icin)
REM     backend\baslat.bat izle     -> --reload (prompt uzerinde calisirken)
REM
REM NEDEN YENIDEN BASLATMAK GEREKIYOR
REM Uvicorn --reload olmadan modulu acilista BIR KEZ ice aktarip bellekte
REM tutuyor. llm.py'deki promptu degistirmek calisan sunucuyu etkilemiyor;
REM cihaz eski promptla konusmaya devam ediyor ve bu hicbir yerde hata
REM vermiyor. Bir kez 44 dakika boyle gecti -- bu betik onu onlemek icin var.
REM
REM "izle" modu dosya degisince kendini yeniden baslatiyor. Acilis olculdu:
REM ~2 saniye (embed modeli diskte onbellekli). Prompt denerken bu bedel
REM onemsiz; demo gunu ise ongorulebilirlik icin normal mod tercih edilmeli.

REM Ayni portu dinleyen eski surec varsa kapat. Olmadan uvicorn
REM "address already in use" ile duser ve eski surum calismaya devam eder.
for /f "tokens=5" %%p in ('netstat -ano ^| findstr ":8000 " ^| findstr "LISTENING"') do (
    echo Eski surec kapatiliyor: PID %%p
    taskkill /F /PID %%p >nul 2>&1
)

cd /d "%~dp0" || exit /b 1

if /i "%1"=="izle" (
    echo Izleme modu: dosya degisince yeniden baslar.
    .venv\Scripts\uvicorn.exe app.main:app --host 0.0.0.0 --port 8000 --reload
) else (
    .venv\Scripts\uvicorn.exe app.main:app --host 0.0.0.0 --port 8000
)
