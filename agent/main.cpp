// C++17 agent: запускает ffmpeg.exe для захвата экрана в MJPEG
// и шлёт поток на FastAPI как HTTP POST с chunked transfer.
// Никаких внешних зависимостей, кроме WinSock и ffmpeg.exe.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <atomic>
#include <thread>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

struct Config {
    std::string server_host = "127.0.0.1";
    int         server_port = 8000;
    std::string agent_id    = "agent1";
    int         framerate   = 30;
    int         quality     = 4;              // -q:v (2..31, меньше — лучше)
    std::string ffmpeg_path = "ffmpeg.exe";
    std::string input_fmt   = "gdigrab";      // захват рабочего стола Windows
    std::string input       = "desktop";
    std::string video_size  = "";             // напр. "1280x720" (опционально)
};

static void log(const std::string& s) {
    std::cerr << "[agent] " << s << std::endl;
}

static HANDLE start_ffmpeg(const Config& cfg, PROCESS_INFORMATION& pi) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 4 * 1024 * 1024)) {
        log("CreatePipe failed");
        return NULL;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    std::ostringstream cmd;
    cmd << "\"" << cfg.ffmpeg_path << "\""
        << " -hide_banner -loglevel warning"
        << " -f "         << cfg.input_fmt
        << " -framerate " << cfg.framerate;
    if (!cfg.video_size.empty())
        cmd << " -video_size " << cfg.video_size;
    cmd << " -i "         << cfg.input
        << " -f mjpeg"
        << " -q:v "       << cfg.quality
        << " -pix_fmt yuvj420p"
        << " pipe:1";

    std::string cmdline = cmd.str();
    std::vector<char> cmdbuf(cmdline.begin(), cmdline.end());
    cmdbuf.push_back(0);

    log("launching: " + cmdline);

    if (!CreateProcessA(NULL, cmdbuf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        log("CreateProcess failed, err=" + std::to_string(GetLastError()));
        CloseHandle(rd);
        CloseHandle(wr);
        return NULL;
    }
    CloseHandle(wr);
    return rd;
}

static SOCKET connect_server(const std::string& host, int port) {
    addrinfo hints{}, *res = NULL;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string p = std::to_string(port);
    if (getaddrinfo(host.c_str(), p.c_str(), &hints, &res) != 0) return INVALID_SOCKET;

    SOCKET s = INVALID_SOCKET;
    for (auto* a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET) {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
    }
    return s;
}

static bool send_all(SOCKET s, const char* p, int n) {
    while (n > 0) {
        int k = send(s, p, n, 0);
        if (k <= 0) return false;
        p += k; n -= k;
    }
    return true;
}

static bool send_chunk(SOCKET s, const char* p, int n) {
    char hdr[32];
    int hl = std::snprintf(hdr, sizeof(hdr), "%X\r\n", n);
    if (!send_all(s, hdr, hl)) return false;
    if (n > 0 && !send_all(s, p, n)) return false;
    return send_all(s, "\r\n", 2);
}

static void parse_cli(Config& c, int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto eq = a.find('=');
        if (eq == std::string::npos) continue;
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if      (k == "--server")  c.server_host = v;
        else if (k == "--port")    c.server_port = std::stoi(v);
        else if (k == "--id")      c.agent_id    = v;
        else if (k == "--fps")     c.framerate   = std::stoi(v);
        else if (k == "--quality") c.quality     = std::stoi(v);
        else if (k == "--ffmpeg")  c.ffmpeg_path = v;
        else if (k == "--input")   c.input       = v;
        else if (k == "--format")  c.input_fmt   = v;
        else if (k == "--size")    c.video_size  = v;
    }
}

int main(int argc, char** argv) {
    Config cfg;
    parse_cli(cfg, argc, argv);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { log("WSAStartup failed"); return 1; }

    log("agent_id=" + cfg.agent_id + " -> " + cfg.server_host + ":" + std::to_string(cfg.server_port));

    while (true) {
        SOCKET sock = connect_server(cfg.server_host, cfg.server_port);
        if (sock == INVALID_SOCKET) {
            log("connect failed, retry in 3s");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        std::ostringstream req;
        req << "POST /ingest/" << cfg.agent_id << " HTTP/1.1\r\n"
            << "Host: " << cfg.server_host << ":" << cfg.server_port << "\r\n"
            << "Content-Type: video/x-motion-jpeg\r\n"
            << "Transfer-Encoding: chunked\r\n"
            << "Connection: close\r\n"
            << "\r\n";
        std::string rs = req.str();
        if (!send_all(sock, rs.data(), (int)rs.size())) {
            closesocket(sock);
            continue;
        }

        PROCESS_INFORMATION pi{};
        HANDLE pipe = start_ffmpeg(cfg, pi);
        if (!pipe) {
            closesocket(sock);
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        std::vector<char> buf(64 * 1024);
        auto t0 = std::chrono::steady_clock::now();
        uint64_t bytes_sent = 0;

        while (true) {
            DWORD n = 0;
            if (!ReadFile(pipe, buf.data(), (DWORD)buf.size(), &n, NULL) || n == 0) {
                log("ffmpeg pipe closed");
                break;
            }
            if (!send_chunk(sock, buf.data(), (int)n)) {
                log("socket send failed");
                break;
            }
            bytes_sent += n;

            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
            if (ms >= 5000) {
                double kbps = (bytes_sent * 8.0) / ms; // kbit/s
                log("throughput ~" + std::to_string((int)kbps) + " kbit/s");
                t0 = now; bytes_sent = 0;
            }
        }

        // final zero chunk
        send_all(sock, "0\r\n\r\n", 5);

        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(pipe);
        closesocket(sock);

        log("reconnect in 2s");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    WSACleanup();
    return 0;
}
