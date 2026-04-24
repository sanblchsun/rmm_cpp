@echo off
REM Для HTTP (напрямую к FastAPI на порту 8000)
agent.exe --server=dev.local --port=8000 --https=false --id=%COMPUTERNAME% --codec=mjpeg --fps=30 --quality=4 --bitrate=4M
