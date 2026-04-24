@echo off
REM Для HTTPS добавьте --https=true
REM Для HTTP (разработка) --https=false
REM Подключаемся через nginx (порт 443), а не напрямую к FastAPI (8000)
agent.exe --server=dev.local --port=443 --https=true --id=%COMPUTERNAME% ^
          --codec=mjpeg --fps=30 --quality=4 --bitrate=4M
