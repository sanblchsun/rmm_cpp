## Запуск как systemd-сервис (production на Linux)

sudo useradd -r -s /usr/sbin/nologin relay
sudo mkdir -p /opt/relay
sudo cp -r server /opt/relay/
sudo chown -R relay:relay /opt/relay
sudo -u relay bash -c 'cd /opt/relay/server && ./install.sh'

sudo cp /opt/relay/server/relay.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now relay
sudo systemctl status relay
journalctl -u relay -f # живые логи

## Обновление:

sudo systemctl restart relay

## Docker на Linux (альтернатива systemd)

cd server
docker compose up -d --build
docker compose logs -f

## 8. Reverse proxy + HTTPS (когда сервер смотрит в интернет)
## nginx на том же Linux-хосте, сертификат через Let's Encrypt:

### nginx.conf
```
map $http_upgrade $connection_upgrade { default upgrade; '' close; }

server {
    listen 80;
    server_name stream.example.com;
    return 301 https://$host$request_uri;
}

server {
    listen 443 ssl http2;
    server_name stream.example.com;

    ssl_certificate     /etc/letsencrypt/live/stream.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/stream.example.com/privkey.pem;

    # КРИТИЧНО для MJPEG и chunked-ingest:
    proxy_buffering        off;
    proxy_request_buffering off;
    client_max_body_size   0;
    proxy_read_timeout     1h;
    proxy_send_timeout     1h;

    location / {
        proxy_pass         http://127.0.0.1:8000;
        proxy_http_version 1.1;
        proxy_set_header   Host              $host;
        proxy_set_header   X-Forwarded-For   $remote_addr;
        proxy_set_header   X-Forwarded-Proto $scheme;
        proxy_set_header   Upgrade           $http_upgrade;
        proxy_set_header   Connection        $connection_upgrade;
    }
}
```
### После этого рекомендую снова повесить uvicorn только на 127.0.0.1:
# в run.sh / relay.service
--host 127.0.0.1 --port 8000


и закрыть 8000 в файерволе — наружу торчит только 443.

Windows-агент ходит по HTTPS на 443 — но текущий agent.exe не поддерживает TLS. Варианты:

A (проще): агент → внутренний IP/VPN по HTTP, nginx наружу по HTTPS.
B: добавить TLS через системный Schannel в агенте (без внешних либ). Это отдельная доработка main.cpp на ~150 строк — готов добавить по запросу.
C: подложить в Windows stunnel как локальный TLS-прокси (агент → 127.0.0.1:HTTP → stunnel → HTTPS наружу). Без правки C++.
9. Рабочий процесс разработки «Linux-сервер ⇄ Windows-агент»
Удобная схема, если Linux-сервер — это, например, ВМ или домашний Linux, а разрабатываете с Windows:

Код сервера держите в git. На Linux:
bash

git clone ... && cd server && ./install.sh && ./run.sh
Правите main.py на Windows (VS Code + Remote-SSH в Linux) — uvicorn сам перезапускается, если добавить --reload в dev-режиме:
bash

uvicorn main:app --host 0.0.0.0 --port 8000 --reload
Агент собираете на Windows (build.bat) и запускаете там же (run.bat) — он ходит на Linux по IP/домену.
Открываете http://<linux-ip>:8000/ в браузере Windows — видите поток ~30 fps.
10. Чек-лист запуска «с нуля»
На Linux (сервер):

 python3 --version ≥ 3.10
 cd server && ./install.sh
 ./run.sh — в логах Uvicorn running on http://0.0.0.0:8000
 curl http://127.0.0.1:8000/healthz → {"ok":true,...}
 sudo ufw allow 8000/tcp (или аналог)
 С другой машины: curl http://<ip>:8000/healthz работает
На Windows (агент):

 ffmpeg.exe рядом с main.cpp
 build.bat → появился agent.exe
 run.bat правильный --server=<linux-ip>
 В консоли агента: throughput ~... kbit/s каждые 5 секунд
 В браузере на http://<linux-ip>:8000/ — карточка агента и поток