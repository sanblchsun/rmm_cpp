@echo off
REM Для HTTPS добавьте --https=true
REM Для HTTP (разработка) --https=false
agent.exe --server=192.168.88.127 --port=8000 --https=true --id=%COMPUTERNAME% ^
          --codec=mjpeg --fps=30 --quality=4 --bitrate=4M
