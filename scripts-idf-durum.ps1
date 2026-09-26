# ESP-IDF kurulum durumu. PowerShell'de calistirin:
#   .\scripts-idf-durum.ps1
# Surekli izlemek icin:
#   while ($true) { Clear-Host; .\scripts-idf-durum.ps1; Start-Sleep 15 }

$log  = "C:\Users\tunak\AppData\Local\Temp\claude\c--Users-tunak-Desktop-Internship-Project\b4acd0b4-89fa-40d1-a8ba-4934a02846e5\scratchpad\idf_install.log"
$tools = "$env:USERPROFILE\.espressif"

Write-Host "--- Indirilen boyut ---" -ForegroundColor Cyan
if (Test-Path $tools) {
    $sum = (Get-ChildItem $tools -Recurse -File -ErrorAction SilentlyContinue |
            Measure-Object -Property Length -Sum).Sum
    Write-Host ("  .espressif : {0:N0} MB  (hedef ~1500-2000 MB)" -f ($sum/1MB))
} else {
    Write-Host "  .espressif henuz olusmadi"
}

Write-Host "--- Hala calisiyor mu ---" -ForegroundColor Cyan
# Surec adina bakmak yaniltici: kurulum birden cok alt surec dogurup bitiriyor.
# Log dosyasinin ne kadar once yazildigi cok daha guvenilir bir gosterge.
if (Test-Path $log) {
    $yas = (New-TimeSpan -Start (Get-Item $log).LastWriteTime -End (Get-Date)).TotalSeconds
    if ($yas -lt 30) {
        Write-Host ("  suruyor (log {0:N0} sn once yazildi)" -f $yas) -ForegroundColor Yellow
    } else {
        Write-Host ("  DURMUS olabilir - log {0:N0} sn'dir sessiz" -f $yas) -ForegroundColor Red
    }
} else { Write-Host "  log henuz yok" }

Write-Host "--- Log son satirlar ---" -ForegroundColor Cyan
if (Test-Path $log) { Get-Content $log -Tail 6 } else { Write-Host "  log henuz yok" }

Write-Host "--- Bitti mi ---" -ForegroundColor Cyan
if (Test-Path "$tools\tools\xtensa-esp-elf") {
    Write-Host "  [x] xtensa-esp-elf derleyici kuruldu" -ForegroundColor Green
} else { Write-Host "  [ ] derleyici henuz yok" }
if (Test-Path "$tools\python_env") {
    Write-Host "  [x] python ortami kuruldu" -ForegroundColor Green
} else { Write-Host "  [ ] python ortami henuz yok" }
