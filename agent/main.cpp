// Windows-агент: MJPEG или H.264 (libx264/h264_amf/h264_qsv/h264_nvenc).
// Кодек переключается удалённо через HTTP-поллинг /agents/{id}/config.

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

#pragma comment(lib, "ws2_32.lib")

// ===== CONFIG =====
struct Config {
    std::string server_host = "127.0.0.1";
    int         server_port = 8000;
    std::string agent_id    = "agent1";

    // стартовые значения (потом могут быть переопределены сервером)
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

// Текущий «желаемый» режим, который меняется по команде с сервера.
struct Runtime {
    std::mutex  m;
    std::string codec, encoder, bitrate;
    int         framerate = 30;
    int         mjpeg_q   = 4;
    std::atomic<bool> restart{false};
    std::atomic<bool> stop{false};
};

static void log(const std::string& s) { std::cerr << "[agent] " << s << std::endl; }

// ===== helpers =====
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
    if (s != INVALID_SOCKET) { int one=1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof one); }
    return s;
}

// Простой HTTP GET: возвращает только body. Для опроса конфига.
static std::string http_get(const std::string& host, int port, const std::string& path) {
    SOCKET s = tcp_connect(host, port);
    if (s == INVALID_SOCKET) return {};
    std::ostringstream r;
    r << "GET " << path << " HTTP/1.1\r\nHost: " << host << ":" << port
      << "\r\nConnection: close\r\nAccept: text/plain\r\n\r\n";
    std::string req = r.str();
    if (!send_all(s, req.data(), (int)req.size())) { closesocket(s); return {}; }
    std::string all;
    char buf[4096];
    for (;;) {
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        all.append(buf, n);
    }
    closesocket(s);
    auto p = all.find("\r\n\r\n");
    return p == std::string::npos ? std::string{} : all.substr(p + 4);
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
        // H.264
        const std::string& enc = r.encoder;

        if (enc == "amf") {
            // Настроено против "keyframe shock" в h264_amf:
            // vbr_latency + vbaq + preanalysis + gop=2s дают ровное качество.
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
            // cpu / libx264
            c << " -c:v libx264 -preset ultrafast -tune zerolatency"
              << " -pix_fmt yuv420p -bf 0 -refs 1"
              << " -b:v "     << r.bitrate
              << " -maxrate " << r.bitrate
              << " -bufsize " << r.bitrate
              << " -g " << r.framerate << " -keyint_min " << r.framerate
              << " -x264-params \"nal-hrd=cbr:force-cfr=1:aud=1\"";
        }

        // Для hw-энкодеров принудительно вставляем AUD — браузерный сплиттер
        // делит access-units именно по ним. libx264 уже делает это через x264-params.
        if (enc != "cpu")
            c << " -bsf:v h264_metadata=aud=insert";

        c << " -f h264 -flush_packets 1 pipe:1";
    }
    return c.str();
}


static HANDLE start_ffmpeg(const std::string& cmdline, PROCESS_INFORMATION& pi) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), NULL, TRUE};
    HANDLE rd=NULL, wr=NULL;
    if (!CreatePipe(&rd, &wr, &sa, 4*1024*1024)) { log("CreatePipe failed"); return NULL; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
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
    // snapshot текущего состояния для сравнения
    std::string last_sig;
    while (!rt.stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(base.config_poll_ms));
        std::string body = http_get(base.server_host, base.server_port,
                                    "/agents/" + base.agent_id + "/config");
        if (body.empty()) continue;

        // формат: key=value\n
        std::string codec, encoder, bitrate;
        int fps = 0, mq = 0;
        std::istringstream iss(body);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back()=='\r') line.pop_back();
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq+1);
            if      (k=="codec")    codec = v;
            else if (k=="encoder")  encoder = v;
            else if (k=="bitrate")  bitrate = v;
            else if (k=="fps")      fps = std::atoi(v.c_str());
            else if (k=="mjpeg_q")  mq  = std::atoi(v.c_str());
        }
        if (codec.empty()) continue;
        std::string sig = codec + "|" + encoder + "|" + bitrate + "|" +
                          std::to_string(fps) + "|" + std::to_string(mq);
        if (sig == last_sig) continue;
        last_sig = sig;

        std::lock_guard<std::mutex> lk(rt.m);
        rt.codec     = codec;
        rt.encoder   = encoder.empty() ? "cpu" : encoder;
        rt.bitrate   = bitrate.empty() ? "4M"  : bitrate;
        if (fps > 0) rt.framerate = fps;
        if (mq  > 0) rt.mjpeg_q   = mq;
        rt.restart = true;
        log("config changed: codec=" + rt.codec + " encoder=" + rt.encoder +
            " bitrate=" + rt.bitrate + " fps=" + std::to_string(rt.framerate));
    }
}

// ===== main session =====
static void run_session(const Config& base, Runtime& rt) {
    SOCKET sock = tcp_connect(base.server_host, base.server_port);
    if (sock == INVALID_SOCKET) { log("connect failed"); return; }

    // snapshot runtime в локальные переменные
    std::string codec, encoder, bitrate;
    int fps, mq;
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

    // подготовить runtime-snapshot для ffmpeg
    Runtime snap; snap.codec=codec; snap.encoder=encoder; snap.bitrate=bitrate;
    snap.framerate=fps; snap.mjpeg_q=mq;
    std::string cmd = build_ffmpeg_cmd(base, snap);

    PROCESS_INFORMATION pi{};
    HANDLE pipe = start_ffmpeg(cmd, pi);
    if (!pipe) { closesocket(sock); return; }

    std::vector<char> buf(64*1024);
    auto t0 = std::chrono::steady_clock::now();
    uint64_t bytes = 0;

    while (!rt.stop) {
        if (rt.restart) { log("restart requested by config change"); break; }
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
            log("["+codec+"/"+encoder+"] "+std::to_string((int)kbps)+" kbit/s");
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
    WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w) != 0) { log("WSAStartup failed"); return 1; }

    Runtime rt;
    rt.codec = cfg.codec; rt.encoder = cfg.encoder; rt.bitrate = cfg.bitrate;
    rt.framerate = cfg.framerate; rt.mjpeg_q = cfg.mjpeg_q;

    log("agent_id="+cfg.agent_id+" -> "+cfg.server_host+":"+std::to_string(cfg.server_port)+
        " codec="+rt.codec+" encoder="+rt.encoder);

    std::thread cfg_thread(poll_config_loop, std::cref(cfg), std::ref(rt));

    while (!rt.stop) {
        run_session(cfg, rt);
        if (!rt.stop) std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    rt.stop = true;
    cfg_thread.join();
    WSACleanup();
    return 0;
}
