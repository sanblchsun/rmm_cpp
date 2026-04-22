@echo off
REM Стартовый режим можно задать, но с сервера он всё равно переопределится.
agent.exe --server=192.168.2.222 --port=8000 --id=%COMPUTERNAME% ^
          --codec=mjpeg --fps=30 --quality=4 --bitrate=4M
