// Windows-агент: MJPEG или H.264 (libx264/h264_amf/h264_qsv/h264_nvenc).
// Кодек/энкодер переключаются удалённо через HTTP-поллинг /agents/{id}/config.
// Ввод (мышь) принимается по WebSocket /ws/control/agent/{id}.
// agent/main.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cctype>
#include <random>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

// ===== CONFIG / RUNTIME =====
struct Config {
    std::string server_host = "127.0.0.1";
    int         server_port = 8000;
    std::string agent_id    = "agent1";

    std::string codec       = "mjpeg";   // "mjpeg" | "h264"
    std::string encoder     = "cpu";     // "cpu" | "amf" | "qsv" | "nvenc"
    std::string bitrate     = "4M";
    int         framerate   = 30;
    int         mjpeg_q     = 4;

    std::string ffmpeg_path = "ffmpeg.exe";
    std::string input_fmt   = "gdigrab";
    std::string input       = "desktop";
    std::string video_size  = "";
    int         config_poll_ms = 2000;
};

struct Runtime {
    std::mutex  m;
    std::string codec, encoder, bitrate;
    int         framerate = 30;
    int         mjpeg_q   = 4;
    std::atomic<bool> restart{false};
    std::atomic<bool> stop{false};

    // Открытый control-WS сокет, чтобы watcher разрешения мог досылать hello.
    std::mutex  ctrl_sock_m;
    SOCKET      ctrl_sock = INVALID_SOCKET;
};

static std::atomic<int> g_screen_w{1920};
static std::atomic<int> g_screen_h{1080};
static std::atomic<int> g_screen_origin_x{0};
static std::atomic<int> g_screen_origin_y{0};

static void log(const std::string& s) { std::cerr << "[agent] " << s << std::endl; }

// ===== sockets / http helpers =====
static bool send_all(SOCKET s, const char* p, int n) {
    while (n > 0) { int k = send(s, p, n, 0); if (k <= 0) return false; p += k; n -= k; }
    return true;
}
static bool send_chunk(SOCKET s, const char* p, int n) {
    char h[32]; int hl = std::snprintf(h, sizeof h, "%X\r\n", n);
    if (!send_all(s, h, hl)) return false;
    if (n > 0 && !send_all(s, p, n)) return false;
    return send_all(s, "\r\n", 2);
}
static SOCKET tcp_connect(const std::string& host, int port) {
    addrinfo hints{}, *res = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    std::string p = std::to_string(port);
    if (getaddrinfo(host.c_str(), p.c_str(), &hints, &res) != 0) return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (auto* a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        closesocket(s); s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET) { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof one); }
    return s;
}
static int recv_n(SOCKET s, char* p, int n) {
    int got = 0; while (got < n) { int k = recv(s, p + got, n - got, 0); if (k <= 0) return got; got += k; }
    return got;
}
static std::string http_get(const std::string& host, int port, const std::string& path) {
    SOCKET s = tcp_connect(host, port);
    if (s == INVALID_SOCKET) return {};
    std::ostringstream r;
    r << "GET " << path << " HTTP/1.1\r\nHost: " << host << ":" << port
      << "\r\nConnection: close\r\nAccept: text/plain\r\n\r\n";
    std::string req = r.str();
    if (!send_all(s, req.data(), (int)req.size())) { closesocket(s); return {}; }
    std::string all; char buf[4096];
    for (;;) { int n = recv(s, buf, sizeof buf, 0); if (n <= 0) break; all.append(buf, n); }
    closesocket(s);
    auto p = all.find("\r\n\r\n");
    return p == std::string::npos ? std::string{} : all.substr(p + 4);
}

// ===== minimal JSON (плоские объекты) =====
static bool json_str(const std::string& j, const std::string& k, std::string& out) {
    std::string key = "\"" + k + "\"";
    auto p = j.find(key); if (p == std::string::npos) return false;
    p = j.find(':', p);   if (p == std::string::npos) return false;
    ++p; while (p < j.size() && std::isspace((unsigned char)j[p])) ++p;
    if (p >= j.size() || j[p] != '"') return false;
    ++p; auto e = j.find('"', p);
    if (e == std::string::npos) return false;
    out = j.substr(p, e - p); return true;
}
static bool json_int(const std::string& j, const std::string& k, int& out) {
    std::string key = "\"" + k + "\"";
    auto p = j.find(key); if (p == std::string::npos) return false;
    p = j.find(':', p);   if (p == std::string::npos) return false;
    ++p; while (p < j.size() && std::isspace((unsigned char)j[p])) ++p;
    if (p >= j.size()) return false;
    int sign = 1;
    if (j[p] == '-') { sign = -1; ++p; }
    if (p >= j.size() || !std::isdigit((unsigned char)j[p])) return false;
    int v = 0;
    while (p < j.size() && std::isdigit((unsigned char)j[p])) { v = v * 10 + (j[p] - '0'); ++p; }
    out = sign * v; return true;
}

// ===== screen metrics =====
static bool read_screen_metrics(int& w, int& h, int& ox, int& oy) {
    w  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h  = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    ox = GetSystemMetrics(SM_XVIRTUALSCREEN);
    oy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    if (w <= 0 || h <= 0) {
        w  = GetSystemMetrics(SM_CXSCREEN);
        h  = GetSystemMetrics(SM_CYSCREEN);
        ox = 0; oy = 0;
    }
    return w > 0 && h > 0;
}

static void init_screen_metrics() {
    int w, h, ox, oy;
    if (read_screen_metrics(w, h, ox, oy)) {
        g_screen_w = w; g_screen_h = h;
        g_screen_origin_x = ox; g_screen_origin_y = oy;
    }
    log("screen " + std::to_string(g_screen_w.load()) + "x" +
        std::to_string(g_screen_h.load()) +
        " origin=(" + std::to_string(g_screen_origin_x.load()) + "," +
        std::to_string(g_screen_origin_y.load()) + ")");
}

// ===== input =====
static void do_mouse_move(int x, int y) {
    int sw = g_screen_w.load();
    int sh = g_screen_h.load();
    if (sw <= 1 || sh <= 1) return;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x >= sw) x = sw - 1;
    if (y >= sh) y = sh - 1;
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)((int64_t)x * 65535 / (sw - 1));
    in.mi.dy = (LONG)((int64_t)y * 65535 / (sh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(INPUT));
}
static void do_mouse_button(int button, bool down) {
    INPUT in{}; in.type = INPUT_MOUSE; DWORD f = 0;
    switch (button) {
        case 0: f = down ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;   break;
        case 1: f = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case 2: f = down ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;  break;
        default: return;
    }
    in.mi.dwFlags = f;
    SendInput(1, &in, sizeof(INPUT));
}
static void do_mouse_wheel(int delta) {
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.mouseData = (DWORD)delta;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &in, sizeof(INPUT));
}

// ===== WebSocket client (RFC 6455, без TLS, без фрагментации payload>1MB) =====
static std::string b64(const unsigned char* d, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; size_t i = 0;
    while (i < n) {
        uint32_t v = 0; int k = (int)std::min<size_t>(3, n - i);
        for (int j = 0; j < k; ++j) v |= d[i + j] << ((2 - j) * 8);
        for (int j = 0; j < 4; ++j) o += (j <= k) ? T[(v >> ((3 - j) * 6)) & 63] : '=';
        i += 3;
    }
    return o;
}
static bool ws_handshake(SOCKET s, const std::string& host, int port, const std::string& path) {
    unsigned char k[16]; std::random_device rd;
    for (int i = 0; i < 16; ++i) k[i] = (unsigned char)(rd() & 0xFF);
    std::ostringstream r;
    r << "GET " << path << " HTTP/1.1\r\n"
      << "Host: " << host << ":" << port << "\r\n"
      << "Upgrade: websocket\r\nConnection: Upgrade\r\n"
      << "Sec-WebSocket-Key: " << b64(k, 16) << "\r\n"
      << "Sec-WebSocket-Version: 13\r\n\r\n";
    std::string rs = r.str();
    if (!send_all(s, rs.data(), (int)rs.size())) return false;
    std::string h; char ch;
    while (h.size() < 8192) {
        if (recv_n(s, &ch, 1) != 1) return false;
        h += ch;
        if (h.size() >= 4 && h.compare(h.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    return h.find(" 101") != std::string::npos;
}
static bool ws_send(SOCKET s, int op, const void* data, size_t len) {
    std::vector<uint8_t> f; f.reserve(len + 14);
    f.push_back((uint8_t)(0x80 | op));
    uint8_t mask[4]; std::random_device rd;
    for (int i = 0; i < 4; ++i) mask[i] = (uint8_t)(rd() & 0xFF);
    if (len < 126) { f.push_back((uint8_t)(0x80 | len)); }
    else if (len < 65536) {
        f.push_back((uint8_t)(0x80 | 126));
        f.push_back((uint8_t)((len >> 8) & 0xFF));
        f.push_back((uint8_t)(len & 0xFF));
    } else {
        f.push_back((uint8_t)(0x80 | 127));
        for (int i = 7; i >= 0; --i) f.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
    }
    for (int i = 0; i < 4; ++i) f.push_back(mask[i]);
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; ++i) f.push_back(p[i] ^ mask[i & 3]);
    return send_all(s, (const char*)f.data(), (int)f.size());
}
static int ws_recv(SOCKET s, std::vector<uint8_t>& payload) {
    uint8_t h[2]; if (recv_n(s, (char*)h, 2) != 2) return -1;
    int op = h[0] & 0x0F; bool masked = (h[1] & 0x80) != 0;
    uint64_t len = h[1] & 0x7F;
    if (len == 126) {
        uint8_t b[2]; if (recv_n(s, (char*)b, 2) != 2) return -1;
        len = ((uint64_t)b[0] << 8) | b[1];
    } else if (len == 127) {
        uint8_t b[8]; if (recv_n(s, (char*)b, 8) != 8) return -1;
        len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | b[i];
    }
    uint8_t mk[4] = {0, 0, 0, 0};
    if (masked && recv_n(s, (char*)mk, 4) != 4) return -1;
    if (len > (1u << 20)) return -1;
    payload.resize((size_t)len);
    if (len && recv_n(s, (char*)payload.data(), (int)len) != (int)len) return -1;
    if (masked) for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mk[i & 3];
    if (op == 0x8) return -1;
    if (op == 0x9) { ws_send(s, 0xA, payload.data(), payload.size()); return 0; }
    if (op == 0xA) return 0;
    if (op == 0x1) return 1;
    if (op == 0x2) return 2;
    return 0;
}

// ===== control hello helpers =====
static std::string make_hello_json() {
    std::ostringstream hs;
    hs << "{\"type\":\"hello\""
       << ",\"screen_w\":" << g_screen_w.load()
       << ",\"screen_h\":" << g_screen_h.load() << "}";
    return hs.str();
}
static void ctrl_send_hello(Runtime& rt) {
    std::lock_guard<std::mutex> lk(rt.ctrl_sock_m);
    if (rt.ctrl_sock == INVALID_SOCKET) return;
    std::string h = make_hello_json();
    ws_send(rt.ctrl_sock, 0x1, h.data(), h.size());
}

// ===== control dispatch =====
static void handle_control(const std::string& j) {
    std::string type; if (!json_str(j, "type", type)) return;
    if (type == "mouse_move") {
        int x = 0, y = 0;
        if (json_int(j, "x", x) && json_int(j, "y", y)) do_mouse_move(x, y);
    } else if (type == "mouse_down" || type == "mouse_up") {
        int btn = 0; json_int(j, "button", btn);
        do_mouse_button(btn, type == "mouse_down");
    } else if (type == "mouse_wheel") {
        int d = 0; if (json_int(j, "delta", d)) do_mouse_wheel(d);
    }
}
static void control_loop(const Config& cfg, Runtime& rt) {
    while (!rt.stop) {
        SOCKET s = tcp_connect(cfg.server_host, cfg.server_port);
        if (s == INVALID_SOCKET) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
        std::string path = "/ws/control/agent/" + cfg.agent_id;
        if (!ws_handshake(s, cfg.server_host, cfg.server_port, path)) {
            log("ctrl ws handshake failed"); closesocket(s);
            std::this_thread::sleep_for(std::chrono::seconds(3)); continue;
        }
        log("ctrl ws connected");

        {
            std::lock_guard<std::mutex> lk(rt.ctrl_sock_m);
            rt.ctrl_sock = s;
        }
        ctrl_send_hello(rt);

        std::vector<uint8_t> buf;
        while (!rt.stop) {
            int r = ws_recv(s, buf);
            if (r < 0) break;
            if (r == 1) {
                std::string msg(buf.begin(), buf.end());
                handle_control(msg);
            }
        }

        {
            std::lock_guard<std::mutex> lk(rt.ctrl_sock_m);
            rt.ctrl_sock = INVALID_SOCKET;
        }
        closesocket(s);
        log("ctrl ws disconnected, retry in 2s");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// ===== resolution watcher =====
static void resolution_watch_loop(Runtime& rt) {
    using namespace std::chrono;
    while (!rt.stop) {
        std::this_thread::sleep_for(seconds(2));
        if (rt.stop) break;

        int w, h, ox, oy;
        if (!read_screen_metrics(w, h, ox, oy)) continue;

        if (w  == g_screen_w.load()        && h  == g_screen_h.load() &&
            ox == g_screen_origin_x.load() && oy == g_screen_origin_y.load())
            continue;

        log("resolution changed: " +
            std::to_string(g_screen_w.load()) + "x" +
            std::to_string(g_screen_h.load()) + " -> " +
            std::to_string(w) + "x" + std::to_string(h));

        g_screen_w = w; g_screen_h = h;
        g_screen_origin_x = ox; g_screen_origin_y = oy;

        rt.restart = true;
        ctrl_send_hello(rt);
    }
}

// ===== ffmpeg =====
static std::string build_ffmpeg_cmd(const Config& base, const Runtime& r) {
    std::ostringstream c;
    c << "\"" << base.ffmpeg_path << "\" -hide_banner -loglevel warning"
      << " -f " << base.input_fmt
      << " -framerate " << r.framerate
      << " -draw_mouse 1";
    if (!base.video_size.empty()) c << " -video_size " << base.video_size;
    c << " -i " << base.input;

    if (r.codec == "mjpeg") {
        c << " -f mjpeg -q:v " << r.mjpeg_q << " -pix_fmt yuvj420p pipe:1";
    } else {
        const std::string& enc = r.encoder;

        if (enc == "amf") {
            int gop = r.framerate * 2;
            c << " -c:v h264_amf"
              << " -usage lowlatency"
              << " -quality balanced"
              << " -rc vbr_latency"
              << " -b:v "     << r.bitrate
              << " -maxrate " << r.bitrate
              << " -g " << gop << " -bf 0"
              << " -vbaq true"
              << " -preanalysis true"
              << " -enforce_hrd true";
        } else if (enc == "qsv") {
            c << " -c:v h264_qsv -preset veryfast -look_ahead 0"
              << " -b:v " << r.bitrate << " -maxrate " << r.bitrate
              << " -g " << r.framerate << " -bf 0";
        } else if (enc == "nvenc") {
            c << " -c:v h264_nvenc -preset p1 -tune ull -rc cbr"
              << " -b:v " << r.bitrate
              << " -g " << r.framerate << " -bf 0";
        } else {
            // libx264:
            //   - veryfast вместо ultrafast -> включён deblocking filter и CABAC,
            //     это убирает прямоугольные артефакты 16x16 на движении (перетаскивание окон и т.п.);
            //   - tune zerolatency -> rc-lookahead=0, sync-lookahead=0, без B-кадров, realtime;
            //   - bufsize = bitrate -> строгий CBR для NAL-HRD, стабильный поток;
            //   - scenecut=0 -> энкодер не выбрасывает лишние IDR из-за движения
            //     (мы сами задаём -g, -keyint_min);
            //   - aq-mode=1 -> адаптивная квантизация, меньше блочности на градиентах;
            //   - aud=1 -> access unit delimiters, чтобы декодер на стороне браузера
            //     корректно нарезал access units (см. findStartCodes в view.html).
            int gop = r.framerate * 2;
            c << " -c:v libx264 -preset veryfast -tune zerolatency"
              << " -profile:v high -pix_fmt yuv420p"
              << " -bf 0 -refs 1"
              << " -b:v "     << r.bitrate
              << " -maxrate " << r.bitrate
              << " -bufsize " << r.bitrate
              << " -g " << gop << " -keyint_min " << r.framerate
              << " -x264-params \""
                   "nal-hrd=cbr:force-cfr=1:aud=1:"
                   "scenecut=0:rc-lookahead=0:sync-lookahead=0:"
                   "aq-mode=1\"";
        }
        if (enc != "cpu")
            c << " -bsf:v h264_metadata=aud=insert";
        c << " -f h264 -flush_packets 1 pipe:1";
    }
    return c.str();
}
static HANDLE start_ffmpeg(const std::string& cmdline, PROCESS_INFORMATION& pi) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), NULL, TRUE};
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 4 * 1024 * 1024)) { log("CreatePipe failed"); return NULL; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    std::vector<char> buf(cmdline.begin(), cmdline.end()); buf.push_back(0);
    log("launching: " + cmdline);
    if (!CreateProcessA(NULL, buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        log("CreateProcess failed, err=" + std::to_string(GetLastError()));
        CloseHandle(rd); CloseHandle(wr); return NULL;
    }
    CloseHandle(wr);
    return rd;
}

// ===== config poll =====
static void poll_config_loop(const Config& base, Runtime& rt) {
    std::string last_sig;
    while (!rt.stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(base.config_poll_ms));
        std::string body = http_get(base.server_host, base.server_port,
                                    "/agents/" + base.agent_id + "/config");
        if (body.empty()) continue;
        std::string codec, encoder, bitrate;
        int fps = 0, mq = 0;
        std::istringstream iss(body);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if      (k == "codec")   codec = v;
            else if (k == "encoder") encoder = v;
            else if (k == "bitrate") bitrate = v;
            else if (k == "fps")     fps = std::atoi(v.c_str());
            else if (k == "mjpeg_q") mq  = std::atoi(v.c_str());
        }
        if (codec.empty()) continue;
        std::string sig = codec + "|" + encoder + "|" + bitrate + "|" +
                          std::to_string(fps) + "|" + std::to_string(mq);
        if (sig == last_sig) continue;
        last_sig = sig;

        std::lock_guard<std::mutex> lk(rt.m);
        rt.codec   = codec;
        rt.encoder = encoder.empty() ? "cpu" : encoder;
        rt.bitrate = bitrate.empty() ? "4M"  : bitrate;
        if (fps > 0) rt.framerate = fps;
        if (mq  > 0) rt.mjpeg_q   = mq;
        rt.restart = true;
        log("config changed: codec=" + rt.codec + " encoder=" + rt.encoder +
            " bitrate=" + rt.bitrate + " fps=" + std::to_string(rt.framerate));
    }
}

// ===== streaming session =====
static void run_session(const Config& base, Runtime& rt) {
    SOCKET sock = tcp_connect(base.server_host, base.server_port);
    if (sock == INVALID_SOCKET) { log("connect failed"); return; }

    std::string codec, encoder, bitrate;
    int fps = 30, mq = 4;
    {
        std::lock_guard<std::mutex> lk(rt.m);
        codec = rt.codec; encoder = rt.encoder; bitrate = rt.bitrate;
        fps = rt.framerate; mq = rt.mjpeg_q;
    }
    rt.restart = false;

    std::string ctype = (codec == "mjpeg") ? "video/x-motion-jpeg" : "video/h264";

    std::ostringstream req;
    req << "POST /ingest/" << base.agent_id << " HTTP/1.1\r\n"
        << "Host: " << base.server_host << ":" << base.server_port << "\r\n"
        << "Content-Type: " << ctype << "\r\n"
        << "X-Agent-Encoder: " << encoder << "\r\n"
        << "X-Agent-Bitrate: " << bitrate << "\r\n"
        << "X-Agent-FPS: "     << fps << "\r\n"
        << "Transfer-Encoding: chunked\r\n"
        << "Connection: close\r\n\r\n";
    std::string s = req.str();
    if (!send_all(sock, s.data(), (int)s.size())) { closesocket(sock); return; }

    Runtime snap;
    snap.codec = codec; snap.encoder = encoder; snap.bitrate = bitrate;
    snap.framerate = fps; snap.mjpeg_q = mq;
    std::string cmd = build_ffmpeg_cmd(base, snap);

    PROCESS_INFORMATION pi{};
    HANDLE pipe = start_ffmpeg(cmd, pi);
    if (!pipe) { closesocket(sock); return; }

    std::vector<char> buf(64 * 1024);
    auto t0 = std::chrono::steady_clock::now();
    uint64_t bytes = 0;

    while (!rt.stop) {
        if (rt.restart) { log("restart requested"); break; }
        DWORD n = 0;
        if (!ReadFile(pipe, buf.data(), (DWORD)buf.size(), &n, NULL) || n == 0) {
            log("ffmpeg pipe closed"); break;
        }
        if (!send_chunk(sock, buf.data(), (int)n)) { log("send failed"); break; }
        bytes += n;
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
        if (ms >= 5000) {
            double kbps = (bytes * 8.0) / ms;
            log("[" + codec + "/" + encoder + "] " + std::to_string((int)kbps) + " kbit/s");
            t0 = now; bytes = 0;
        }
    }

    send_all(sock, "0\r\n\r\n", 5);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(pipe);
    closesocket(sock);
}

// ===== CLI =====
static void parse_cli(Config& c, int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i]; auto eq = a.find('=');
        if (eq == std::string::npos) continue;
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if      (k == "--server")   c.server_host = v;
        else if (k == "--port")     c.server_port = std::stoi(v);
        else if (k == "--id")       c.agent_id    = v;
        else if (k == "--codec")    c.codec       = v;
        else if (k == "--encoder")  c.encoder     = v;
        else if (k == "--bitrate")  c.bitrate     = v;
        else if (k == "--fps")      c.framerate   = std::stoi(v);
        else if (k == "--quality")  c.mjpeg_q     = std::stoi(v);
        else if (k == "--ffmpeg")   c.ffmpeg_path = v;
        else if (k == "--input")    c.input       = v;
        else if (k == "--format")   c.input_fmt   = v;
        else if (k == "--size")     c.video_size  = v;
    }
}

int main(int argc, char** argv) {
    Config cfg; parse_cli(cfg, argc, argv);
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { log("WSAStartup failed"); return 1; }

    init_screen_metrics();

    Runtime rt;
    rt.codec = cfg.codec; rt.encoder = cfg.encoder; rt.bitrate = cfg.bitrate;
    rt.framerate = cfg.framerate; rt.mjpeg_q = cfg.mjpeg_q;

    log("agent_id=" + cfg.agent_id + " -> " + cfg.server_host + ":" + std::to_string(cfg.server_port) +
        " codec=" + rt.codec + " encoder=" + rt.encoder);

    std::thread ctrl_thread(control_loop, std::cref(cfg), std::ref(rt));
    std::thread cfg_thread(poll_config_loop, std::cref(cfg), std::ref(rt));
    std::thread res_thread(resolution_watch_loop, std::ref(rt));

    while (!rt.stop) {
        run_session(cfg, rt);
        if (!rt.stop) std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    rt.stop = true;
    cfg_thread.join();
    ctrl_thread.join();
    res_thread.join();
    WSACleanup();
    return 0;
}
