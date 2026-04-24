@echo off
REM Для HTTPS с самоподписанным сертификатом
agent.exe --server=dev.local --port=443 --id=%COMPUTERNAME% --codec=mjpeg --fps=30 --quality=4 --bitrate=4M --insecure