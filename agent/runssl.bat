@echo off
REM Для HTTPS (через nginx на порту 443)
agent.exe --server=dev.local --port=443 --https=true --id=%COMPUTERNAME% --codec=mjpeg --fps=30 --quality=4 --bitrate=4M
