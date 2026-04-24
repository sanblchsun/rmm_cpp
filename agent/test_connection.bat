@echo off
echo === Диагностика подключения к серверу ===
echo.

echo 1. Проверяем разрешение DNS для dev.local:
nslookup dev.local
echo.

echo 2. Проверяем доступность порта 443:
telnet dev.local 443
echo.

echo 3. Проверяем доступность через браузер:
echo Откройте в браузере: https://dev.local
echo.

echo 4. Проверяем сертификат:
powershell -Command "$cert = [System.Net.ServicePointManager]::ServerCertificateValidationCallback = {$true}; try { $req = [System.Net.WebRequest]::Create('https://dev.local'); $req.GetResponse() | Out-Null; Write-Host 'SSL соединение успешно' } catch { Write-Host 'Ошибка SSL: ' $_.Exception.Message }"
echo.

echo 5. Запуск агента с --insecure флагом:
agent.exe --server=dev.local --port=443 --id=%COMPUTERNAME% --codec=mjpeg --fps=30 --quality=4 --bitrate=4M --insecure
pause