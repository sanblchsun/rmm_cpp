// Windows-агент: MJPEG или H.264 (libx264/h264_amf/h264_qsv/h264_nvenc).
// Кодек/энкодер переключаются удалённо через HTTPS-поллинг /agents/{id}/config.
// Ввод (мышь + клавиатура + буфер обмена) — WSS /ws/control/agent/{id}.
// TLS реализован через системный Schannel (без внешних библиотек).
// agent/main.cpp

#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
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
#include <unordered_map>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Secur32.lib")
#pragma comment(lib, "user32.lib")

// ===== CONFIG / RUNTIME =====
struct Config
{
    std::string server_host = "127.0.0.1";
    int server_port = 443; // HTTPS/WSS по умолчанию
    std::string agent_id = "agent1";
    bool verify_cert = true; // false → --insecure (самоподписанный сертификат)

    std::string codec = "mjpeg";
    std::string encoder = "cpu";
    std::string bitrate = "4M";
    int framerate = 30;
    int mjpeg_q = 4;

    std::string ffmpeg_path = "ffmpeg.exe";
    std::string input_fmt = "gdigrab";
    std::string input = "desktop";
    std::string video_size = "";
    int config_poll_ms = 2000;
};

// Forward-объявление TlsConn для Runtime
struct TlsConn;

struct Runtime
{
    std::mutex m;
    std::string codec, encoder, bitrate;
    int framerate = 30;
    int mjpeg_q = 4;
    std::atomic<bool> restart{false};
    std::atomic<bool> stop{false};

    std::mutex ctrl_sock_m;
    TlsConn *ctrl_conn = nullptr; // ← вместо SOCKET ctrl_sock
};

static std::atomic<int> g_screen_w{1920};
static std::atomic<int> g_screen_h{1080};
static std::atomic<int> g_screen_origin_x{0};
static std::atomic<int> g_screen_origin_y{0};

static std::mutex g_clip_m;
static std::string g_last_clip;

static void log(const std::string &s) { std::cerr << "[agent] " << s << std::endl; }

// ===== RAW TCP helpers (используются только внутри TLS-рукопожатия и tcp_connect) =====
static bool send_all_raw(SOCKET s, const char *p, int n)
{
    while (n > 0)
    {
        int k = send(s, p, n, 0);
        if (k <= 0)
            return false;
        p += k;
        n -= k;
    }
    return true;
}
static int recv_n_raw(SOCKET s, char *p, int n)
{
    int got = 0;
    while (got < n)
    {
        int k = recv(s, p + got, n - got, 0);
        if (k <= 0)
            return got;
        got += k;
    }
    return got;
}
static SOCKET tcp_connect(const std::string &host, int port)
{
    addrinfo hints{}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string p = std::to_string(port);
    if (getaddrinfo(host.c_str(), p.c_str(), &hints, &res) != 0)
        return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (auto *a = res; a; a = a->ai_next)
    {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0)
            break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET)
    {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    }
    return s;
}

// ===== TLS / Schannel =====
struct TlsConn
{
    SOCKET sock = INVALID_SOCKET;
    CredHandle cred = {};
    CtxtHandle ctx = {};
    bool cred_ok = false;
    bool ctx_ok = false;
    SecPkgContext_StreamSizes sizes = {};
    std::vector<uint8_t> raw;   // зашифрованные байты, принятые из сокета
    std::vector<uint8_t> plain; // расшифрованные байты, ещё не прочитанные caller-ом
};

static void tls_close(TlsConn *c)
{
    if (!c)
        return;
    if (c->ctx_ok)
    {
        DeleteSecurityContext(&c->ctx);
        c->ctx_ok = false;
    }
    if (c->cred_ok)
    {
        FreeCredentialHandle(&c->cred);
        c->cred_ok = false;
    }
    if (c->sock != INVALID_SOCKET)
    {
        closesocket(c->sock);
        c->sock = INVALID_SOCKET;
    }
}

// Выполняет TLS-рукопожатие поверх уже подключённого сокета.
// verify_cert=false — разрешает самоподписанные сертификаты.
static bool tls_handshake(TlsConn *c, const std::string &host, bool verify_cert)
{
    // — получаем учётные данные клиента —
    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS;
    if (verify_cert)
        sc.dwFlags |= SCH_CRED_AUTO_CRED_VALIDATION;
    else
        sc.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION; // принять любой сертификат

    SECURITY_STATUS ss = AcquireCredentialsHandleA(
        NULL, const_cast<char *>(UNISP_NAME_A),
        SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, &c->cred, NULL);
    if (ss != SEC_E_OK)
    {
        std::ostringstream o;
        o << std::hex << (DWORD)ss;
        log("AcquireCredentialsHandle failed: 0x" + o.str());

        // Добавляем расшифровку ошибок
        switch (ss)
        {
        case SEC_E_SECPKG_NOT_FOUND:
            log("  -> Security package not found");
            break;
        case SEC_E_NOT_OWNER:
            log("  -> Not owner");
            break;
        case SEC_E_CANNOT_INSTALL:
            log("  -> Cannot install");
            break;
        default:
            log("  -> Unknown error");
        }
        return false;
    }
    c->cred_ok = true;

    const DWORD req_flags =
        ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
        ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR |
        ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

    // Hostname в широкую строку (ASCII → wchar_t, достаточно для IP/домена)
    std::wstring whost(host.begin(), host.end());

    // — первый вызов (без входных данных) — получаем ClientHello —
    SecBuffer out_b = {0, SECBUFFER_TOKEN, NULL};
    SecBufferDesc out_d = {SECBUFFER_VERSION, 1, &out_b};
    DWORD ret_flags = 0;

    ss = InitializeSecurityContextW(
        &c->cred, NULL, const_cast<wchar_t *>(whost.c_str()),
        req_flags, 0, SECURITY_NATIVE_DREP,
        NULL, 0, &c->ctx, &out_d, &ret_flags, NULL);
    c->ctx_ok = true;

    if (out_b.pvBuffer && out_b.cbBuffer > 0)
    {
        bool ok = send_all_raw(c->sock, (const char *)out_b.pvBuffer, (int)out_b.cbBuffer);
        FreeContextBuffer(out_b.pvBuffer);
        out_b.pvBuffer = NULL;
        if (!ok)
            return false;
    }
    if (ss != SEC_I_CONTINUE_NEEDED)
    {
        std::ostringstream o;
        o << std::hex << (DWORD)ss;
        log("ISC initial failed: 0x" + o.str());
        return false;
    }

    // — цикл рукопожатия —
    std::vector<uint8_t> in_buf;
    char tmp[16384];

    while (true)
    {
        // Читаем из сокета (нужно при CONTINUE_NEEDED и INCOMPLETE_MESSAGE)
        {
            int n = recv(c->sock, tmp, sizeof tmp, 0);
            if (n <= 0)
            {
                log("tls_handshake: socket closed");
                return false;
            }
            in_buf.insert(in_buf.end(), tmp, tmp + n);
        }

    retry:
        SecBuffer in_bufs[2] = {
            {(ULONG)in_buf.size(), SECBUFFER_TOKEN, in_buf.data()},
            {0, SECBUFFER_EMPTY, NULL}};
        SecBufferDesc in_d = {SECBUFFER_VERSION, 2, in_bufs};
        out_b = {0, SECBUFFER_TOKEN, NULL};
        out_d = {SECBUFFER_VERSION, 1, &out_b};
        ret_flags = 0;

        ss = InitializeSecurityContextW(
            &c->cred, &c->ctx, NULL,
            req_flags, 0, SECURITY_NATIVE_DREP,
            &in_d, 0, NULL, &out_d, &ret_flags, NULL);

        // Отправляем выходной токен, если есть
        if (out_b.pvBuffer && out_b.cbBuffer > 0)
        {
            bool ok = send_all_raw(c->sock, (const char *)out_b.pvBuffer, (int)out_b.cbBuffer);
            FreeContextBuffer(out_b.pvBuffer);
            out_b.pvBuffer = NULL;
            if (!ok)
                return false;
        }

        // Обрабатываем остаток входных данных (SECBUFFER_EXTRA)
        if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0)
        {
            size_t off = in_buf.size() - in_bufs[1].cbBuffer;
            std::vector<uint8_t> extra(in_buf.begin() + (ptrdiff_t)off, in_buf.end());
            in_buf = std::move(extra);
        }
        else if (ss != SEC_E_INCOMPLETE_MESSAGE)
        {
            in_buf.clear();
        }

        if (ss == SEC_E_OK)
            break;
        if (ss == SEC_I_CONTINUE_NEEDED)
            continue; // нужно больше данных от сервера
        if (ss == SEC_E_INCOMPLETE_MESSAGE)
        { // нужно дочитать текущий пакет
            int n = recv(c->sock, tmp, sizeof tmp, 0);
            if (n <= 0)
            {
                log("tls_handshake: socket closed (incomplete)");
                return false;
            }
            in_buf.insert(in_buf.end(), tmp, tmp + n);
            goto retry;
        }
        // Любая другая ошибка
        std::ostringstream o;
        o << std::hex << (DWORD)ss;
        log("TLS handshake error: 0x" + o.str());
        return false;
    }

    // Остаток после рукопожатия — зашифрованные данные от сервера
    if (!in_buf.empty())
        c->raw = std::move(in_buf);

    QueryContextAttributes(&c->ctx, SECPKG_ATTR_STREAM_SIZES, &c->sizes);
    return true;
}

// TCP + TLS. Возвращает nullptr при ошибке; caller обязан вызвать tls_close + delete.
static TlsConn *tls_connect(const std::string &host, int port, bool verify_cert)
{
    SOCKET s = tcp_connect(host, port);
    if (s == INVALID_SOCKET)
        return nullptr;

    TlsConn *c = new TlsConn();
    c->sock = s;
    if (!tls_handshake(c, host, verify_cert))
    {
        tls_close(c);
        delete c;
        return nullptr;
    }
    return c;
}

// Шифрует и отправляет данные через TLS.
static bool tls_send_all(TlsConn *c, const char *p, int n)
{
    const int MAX_MSG = (int)c->sizes.cbMaximumMessage;
    while (n > 0)
    {
        int chunk = std::min(n, MAX_MSG);
        // Буфер: заголовок + данные + хвост
        std::vector<uint8_t> msg(c->sizes.cbHeader + (size_t)chunk + c->sizes.cbTrailer);

        SecBuffer bufs[3] = {
            {c->sizes.cbHeader, SECBUFFER_STREAM_HEADER, msg.data()},
            {(ULONG)chunk, SECBUFFER_DATA, msg.data() + c->sizes.cbHeader},
            {c->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER,
             msg.data() + c->sizes.cbHeader + chunk}};
        SecBufferDesc desc = {SECBUFFER_VERSION, 3, bufs};
        memcpy(bufs[1].pvBuffer, p, (size_t)chunk);

        SECURITY_STATUS ss = EncryptMessage(&c->ctx, 0, &desc, 0);
        if (ss != SEC_E_OK)
            return false;

        int total = (int)(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
        if (!send_all_raw(c->sock, (const char *)msg.data(), total))
            return false;

        p += chunk;
        n -= chunk;
    }
    return true;
}

// Возвращает до want расшифрованных байт (блокирует, пока не получит хотя бы один).
// Возвращает <=0 при ошибке / закрытии соединения.
static int tls_recv_some(TlsConn *c, char *buf, int want)
{
    // Сначала отдаём уже расшифрованное
    if (!c->plain.empty())
    {
        int n = (int)std::min((size_t)want, c->plain.size());
        memcpy(buf, c->plain.data(), (size_t)n);
        c->plain.erase(c->plain.begin(), c->plain.begin() + n);
        return n;
    }

    char tmp[16384];
    for (;;)
    {
        // Пробуем расшифровать из c->raw
        while (!c->raw.empty())
        {
            SecBuffer in_bufs[4] = {
                {(ULONG)c->raw.size(), SECBUFFER_DATA, c->raw.data()},
                {0, SECBUFFER_EMPTY, NULL},
                {0, SECBUFFER_EMPTY, NULL},
                {0, SECBUFFER_EMPTY, NULL}};
            SecBufferDesc in_desc = {SECBUFFER_VERSION, 4, in_bufs};

            SECURITY_STATUS ss = DecryptMessage(&c->ctx, &in_desc, 0, NULL);

            if (ss == SEC_E_INCOMPLETE_MESSAGE)
                break; // нужно дочитать пакет

            if (ss == SEC_I_CONTEXT_EXPIRED)
                return 0; // TLS close_notify
            if (ss != SEC_E_OK && ss != SEC_I_RENEGOTIATE)
                return -1;

            // Собираем расшифрованные данные
            for (int i = 0; i < 4; ++i)
            {
                if (in_bufs[i].BufferType == SECBUFFER_DATA && in_bufs[i].cbBuffer > 0)
                {
                    auto *ptr = (uint8_t *)in_bufs[i].pvBuffer;
                    c->plain.insert(c->plain.end(), ptr, ptr + in_bufs[i].cbBuffer);
                }
            }
            // Остаток зашифрованных данных (следующий TLS-запись)
            bool has_extra = false;
            for (int i = 1; i < 4; ++i)
            {
                if (in_bufs[i].BufferType == SECBUFFER_EXTRA && in_bufs[i].cbBuffer > 0)
                {
                    size_t off = c->raw.size() - in_bufs[i].cbBuffer;
                    std::vector<uint8_t> extra(
                        c->raw.begin() + (ptrdiff_t)off, c->raw.end());
                    c->raw = std::move(extra);
                    has_extra = true;
                    break;
                }
            }
            if (!has_extra)
                c->raw.clear();

            if (!c->plain.empty())
            {
                int n = (int)std::min((size_t)want, c->plain.size());
                memcpy(buf, c->plain.data(), (size_t)n);
                c->plain.erase(c->plain.begin(), c->plain.begin() + n);
                return n;
            }
        }
        // Нужно ещё зашифрованных данных из сокета
        int n = recv(c->sock, tmp, sizeof tmp, 0);
        if (n <= 0)
            return -1;
        c->raw.insert(c->raw.end(), tmp, tmp + n);
    }
}

// Читает ровно n расшифрованных байт.
static int tls_recv_n(TlsConn *c, char *p, int n)
{
    int got = 0;
    while (got < n)
    {
        int k = tls_recv_some(c, p + got, n - got);
        if (k <= 0)
            return got;
        got += k;
    }
    return got;
}

// ===== Единые I/O-обёртки (TlsConn*) =====
static bool send_all(TlsConn *c, const char *p, int n)
{
    return tls_send_all(c, p, n);
}
static bool send_chunk(TlsConn *c, const char *p, int n)
{
    char h[32];
    int hl = std::snprintf(h, sizeof h, "%X\r\n", n);
    if (!send_all(c, h, hl))
        return false;
    if (n > 0 && !send_all(c, p, n))
        return false;
    return send_all(c, "\r\n", 2);
}
static int recv_n(TlsConn *c, char *p, int n)
{
    return tls_recv_n(c, p, n);
}

// ===== HTTPS GET =====
static std::string http_get(const std::string &host, int port,
                            const std::string &path, bool verify_cert)
{
    TlsConn *c = tls_connect(host, port, verify_cert);
    if (!c)
        return {};
    std::ostringstream r;
    r << "GET " << path << " HTTP/1.1\r\nHost: " << host << ":" << port
      << "\r\nConnection: close\r\nAccept: text/plain\r\n\r\n";
    std::string req = r.str();
    if (!send_all(c, req.data(), (int)req.size()))
    {
        tls_close(c);
        delete c;
        return {};
    }

    std::string all;
    char buf[4096];
    for (;;)
    {
        int n = tls_recv_some(c, buf, sizeof buf);
        if (n <= 0)
            break;
        all.append(buf, (size_t)n);
    }
    tls_close(c);
    delete c;
    auto p2 = all.find("\r\n\r\n");
    return p2 == std::string::npos ? std::string{} : all.substr(p2 + 4);
}

// ===== minimal JSON (без изменений) =====
static bool json_str(const std::string &j, const std::string &k, std::string &out)
{
    std::string key = "\"" + k + "\"";
    auto p = j.find(key);
    if (p == std::string::npos)
        return false;
    p = j.find(':', p);
    if (p == std::string::npos)
        return false;
    ++p;
    while (p < j.size() && std::isspace((unsigned char)j[p]))
        ++p;
    if (p >= j.size() || j[p] != '"')
        return false;
    ++p;
    auto e = j.find('"', p);
    if (e == std::string::npos)
        return false;
    out = j.substr(p, e - p);
    return true;
}
static bool json_int(const std::string &j, const std::string &k, int &out)
{
    std::string key = "\"" + k + "\"";
    auto p = j.find(key);
    if (p == std::string::npos)
        return false;
    p = j.find(':', p);
    if (p == std::string::npos)
        return false;
    ++p;
    while (p < j.size() && std::isspace((unsigned char)j[p]))
        ++p;
    if (p >= j.size())
        return false;
    int sign = 1;
    if (j[p] == '-')
    {
        sign = -1;
        ++p;
    }
    if (p >= j.size() || !std::isdigit((unsigned char)j[p]))
        return false;
    int v = 0;
    while (p < j.size() && std::isdigit((unsigned char)j[p]))
    {
        v = v * 10 + (j[p] - '0');
        ++p;
    }
    out = sign * v;
    return true;
}
static bool json_str_ex(const std::string &j, const std::string &k, std::string &out)
{
    std::string key = "\"" + k + "\"";
    auto p = j.find(key);
    if (p == std::string::npos)
        return false;
    p = j.find(':', p);
    if (p == std::string::npos)
        return false;
    ++p;
    while (p < j.size() && std::isspace((unsigned char)j[p]))
        ++p;
    if (p >= j.size() || j[p] != '"')
        return false;
    ++p;
    out.clear();
    auto hex = [](char c, unsigned &v)
    {
        if (c >= '0' && c <= '9')
        {
            v = c - '0';
            return true;
        }
        if (c >= 'a' && c <= 'f')
        {
            v = c - 'a' + 10;
            return true;
        }
        if (c >= 'A' && c <= 'F')
        {
            v = c - 'A' + 10;
            return true;
        }
        return false;
    };
    auto emit_cp = [&](unsigned cp)
    {
        if (cp < 0x80)
            out += (char)cp;
        else if (cp < 0x800)
        {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    };
    while (p < j.size())
    {
        char c = j[p];
        if (c == '"')
            return true;
        if (c == '\\' && p + 1 < j.size())
        {
            char n = j[p + 1];
            if (n == '"' || n == '\\' || n == '/')
            {
                out += n;
                p += 2;
                continue;
            }
            if (n == 'n')
            {
                out += '\n';
                p += 2;
                continue;
            }
            if (n == 't')
            {
                out += '\t';
                p += 2;
                continue;
            }
            if (n == 'r')
            {
                out += '\r';
                p += 2;
                continue;
            }
            if (n == 'b')
            {
                out += '\b';
                p += 2;
                continue;
            }
            if (n == 'f')
            {
                out += '\f';
                p += 2;
                continue;
            }
            if (n == 'u' && p + 5 < j.size())
            {
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i)
                {
                    unsigned v;
                    if (!hex(j[p + 2 + i], v))
                        return false;
                    cp = (cp << 4) | v;
                }
                p += 6;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 5 < j.size() && j[p] == '\\' && j[p + 1] == 'u')
                {
                    unsigned low = 0;
                    bool ok = true;
                    for (int i = 0; i < 4; ++i)
                    {
                        unsigned v;
                        if (!hex(j[p + 2 + i], v))
                        {
                            ok = false;
                            break;
                        }
                        low = (low << 4) | v;
                    }
                    if (ok && low >= 0xDC00 && low <= 0xDFFF)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        p += 6;
                    }
                }
                emit_cp(cp);
                continue;
            }
            return false;
        }
        out += c;
        ++p;
    }
    return false;
}
static std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            }
            else
                out += (char)c;
        }
    }
    out += '"';
    return out;
}

// ===== screen metrics (без изменений) =====
static bool read_screen_metrics(int &w, int &h, int &ox, int &oy)
{
    w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    ox = GetSystemMetrics(SM_XVIRTUALSCREEN);
    oy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    if (w <= 0 || h <= 0)
    {
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
        ox = 0;
        oy = 0;
    }
    return w > 0 && h > 0;
}
static void init_screen_metrics()
{
    int w, h, ox, oy;
    if (read_screen_metrics(w, h, ox, oy))
    {
        g_screen_w = w;
        g_screen_h = h;
        g_screen_origin_x = ox;
        g_screen_origin_y = oy;
    }
    log("screen " + std::to_string(g_screen_w.load()) + "x" + std::to_string(g_screen_h.load()) +
        " origin=(" + std::to_string(g_screen_origin_x.load()) + "," +
        std::to_string(g_screen_origin_y.load()) + ")");
}

// ===== mouse input (без изменений) =====
static void do_mouse_move(int x, int y)
{
    int sw = g_screen_w.load(), sh = g_screen_h.load();
    if (sw <= 1 || sh <= 1)
        return;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= sw)
        x = sw - 1;
    if (y >= sh)
        y = sh - 1;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)((int64_t)x * 65535 / (sw - 1));
    in.mi.dy = (LONG)((int64_t)y * 65535 / (sh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(INPUT));
}
static void do_mouse_button(int button, bool down)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    DWORD f = 0;
    switch (button)
    {
    case 0:
        f = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case 1:
        f = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case 2:
        f = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    default:
        return;
    }
    in.mi.dwFlags = f;
    SendInput(1, &in, sizeof(INPUT));
}
static void do_mouse_wheel(int delta)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.mouseData = (DWORD)delta;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &in, sizeof(INPUT));
}

// ===== keyboard input (без изменений) =====
static void do_text_input(const std::string &utf8)
{
    if (utf8.empty())
        return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
    if (wlen <= 0)
        return;
    std::vector<wchar_t> w((size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), w.data(), wlen);
    std::vector<INPUT> inputs;
    inputs.reserve(w.size() * 2);
    auto push_vk = [&](WORD vk)
    {
        INPUT d{};
        d.type = INPUT_KEYBOARD;
        d.ki.wVk = vk;
        d.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        d.ki.dwFlags = 0;
        INPUT u = d;
        u.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(d);
        inputs.push_back(u);
    };
    for (wchar_t ch : w)
    {
        if (ch == L'\r')
            continue;
        if (ch == L'\n')
        {
            push_vk(VK_RETURN);
            continue;
        }
        if (ch == L'\t')
        {
            push_vk(VK_TAB);
            continue;
        }
        INPUT d{};
        d.type = INPUT_KEYBOARD;
        d.ki.wVk = 0;
        d.ki.wScan = (WORD)ch;
        d.ki.dwFlags = KEYEVENTF_UNICODE;
        if ((ch & 0xFF00) == 0xE000)
            d.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        INPUT u = d;
        u.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(d);
        inputs.push_back(u);
    }
    if (inputs.empty())
        return;
    const size_t BATCH = 128;
    for (size_t i = 0; i < inputs.size(); i += BATCH)
    {
        UINT n = (UINT)std::min(BATCH, inputs.size() - i);
        SendInput(n, inputs.data() + i, sizeof(INPUT));
    }
}
static int code_to_vk(const std::string &code)
{
    if (code.size() == 4 && code.compare(0, 3, "Key") == 0)
    {
        char c = code[3];
        if (c >= 'A' && c <= 'Z')
            return c;
    }
    if (code.size() == 6 && code.compare(0, 5, "Digit") == 0)
    {
        char c = code[5];
        if (c >= '0' && c <= '9')
            return c;
    }
    if (code.compare(0, 6, "Numpad") == 0)
    {
        if (code.size() == 7)
        {
            char c = code[6];
            if (c >= '0' && c <= '9')
                return VK_NUMPAD0 + (c - '0');
        }
        if (code == "NumpadAdd")
            return VK_ADD;
        if (code == "NumpadSubtract")
            return VK_SUBTRACT;
        if (code == "NumpadMultiply")
            return VK_MULTIPLY;
        if (code == "NumpadDivide")
            return VK_DIVIDE;
        if (code == "NumpadDecimal")
            return VK_DECIMAL;
        if (code == "NumpadEnter")
            return VK_RETURN;
    }
    if (!code.empty() && code[0] == 'F' && code.size() >= 2 && code.size() <= 3)
    {
        bool digits = true;
        for (size_t i = 1; i < code.size(); ++i)
            if (!std::isdigit((unsigned char)code[i]))
            {
                digits = false;
                break;
            }
        if (digits)
        {
            int n = std::atoi(code.c_str() + 1);
            if (n >= 1 && n <= 24)
                return VK_F1 + (n - 1);
        }
    }
    static const std::unordered_map<std::string, int> m = {
        {"Enter", VK_RETURN},
        {"Backspace", VK_BACK},
        {"Tab", VK_TAB},
        {"Space", VK_SPACE},
        {"Escape", VK_ESCAPE},
        {"ArrowLeft", VK_LEFT},
        {"ArrowRight", VK_RIGHT},
        {"ArrowUp", VK_UP},
        {"ArrowDown", VK_DOWN},
        {"Home", VK_HOME},
        {"End", VK_END},
        {"PageUp", VK_PRIOR},
        {"PageDown", VK_NEXT},
        {"Insert", VK_INSERT},
        {"Delete", VK_DELETE},
        {"ShiftLeft", VK_LSHIFT},
        {"ShiftRight", VK_RSHIFT},
        {"ControlLeft", VK_LCONTROL},
        {"ControlRight", VK_RCONTROL},
        {"AltLeft", VK_LMENU},
        {"AltRight", VK_RMENU},
        {"MetaLeft", VK_LWIN},
        {"MetaRight", VK_RWIN},
        {"OSLeft", VK_LWIN},
        {"OSRight", VK_RWIN},
        {"CapsLock", VK_CAPITAL},
        {"NumLock", VK_NUMLOCK},
        {"ScrollLock", VK_SCROLL},
        {"PrintScreen", VK_SNAPSHOT},
        {"Pause", VK_PAUSE},
        {"ContextMenu", VK_APPS},
        {"Minus", VK_OEM_MINUS},
        {"Equal", VK_OEM_PLUS},
        {"BracketLeft", VK_OEM_4},
        {"BracketRight", VK_OEM_6},
        {"Backslash", VK_OEM_5},
        {"Semicolon", VK_OEM_1},
        {"Quote", VK_OEM_7},
        {"Comma", VK_OEM_COMMA},
        {"Period", VK_OEM_PERIOD},
        {"Slash", VK_OEM_2},
        {"Backquote", VK_OEM_3},
        {"IntlBackslash", VK_OEM_102},
    };
    auto it = m.find(code);
    return it == m.end() ? 0 : it->second;
}
static void do_key(const std::string &code, bool down)
{
    int vk = code_to_vk(code);
    if (vk == 0)
        return;
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    switch (vk)
    {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_HOME:
    case VK_END:
    case VK_INSERT:
    case VK_DELETE:
    case VK_SNAPSHOT:
    case VK_APPS:
    case VK_LWIN:
    case VK_RWIN:
    case VK_NUMLOCK:
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        break;
    }
    SendInput(1, &in, sizeof(INPUT));
}

// ===== clipboard (без изменений) =====
static std::string clipboard_read_utf8()
{
    bool opened = false;
    for (int i = 0; i < 10; ++i)
    {
        if (OpenClipboard(NULL))
        {
            opened = true;
            break;
        }
        Sleep(15);
    }
    if (!opened)
        return {};
    std::string result;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h)
    {
        const wchar_t *w = (const wchar_t *)GlobalLock(h);
        if (w)
        {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            if (n > 1)
            {
                result.resize((size_t)n - 1);
                WideCharToMultiByte(CP_UTF8, 0, w, -1, &result[0], n, NULL, NULL);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return result;
}
static void clipboard_write_utf8(const std::string &utf8)
{
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size() + 1, NULL, 0);
    if (wlen <= 0)
        return;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (size_t)wlen * sizeof(wchar_t));
    if (!mem)
        return;
    wchar_t *dst = (wchar_t *)GlobalLock(mem);
    if (!dst)
    {
        GlobalFree(mem);
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size() + 1, dst, wlen);
    GlobalUnlock(mem);
    bool opened = false;
    for (int i = 0; i < 10; ++i)
    {
        if (OpenClipboard(NULL))
        {
            opened = true;
            break;
        }
        Sleep(15);
    }
    if (!opened)
    {
        GlobalFree(mem);
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, mem))
        GlobalFree(mem);
    CloseClipboard();
    std::lock_guard<std::mutex> lk(g_clip_m);
    g_last_clip = utf8;
}

// ===== WebSocket (переведён на TlsConn*) =====
static std::string b64(const unsigned char *d, size_t n)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    while (i < n)
    {
        uint32_t v = 0;
        int k = (int)std::min<size_t>(3, n - i);
        for (int j = 0; j < k; ++j)
            v |= d[i + j] << ((2 - j) * 8);
        for (int j = 0; j < 4; ++j)
            o += (j <= k) ? T[(v >> ((3 - j) * 6)) & 63] : '=';
        i += 3;
    }
    return o;
}
static bool ws_handshake(TlsConn *c, const std::string &host, int port,
                         const std::string &path)
{
    unsigned char k[16];
    std::random_device rd;
    for (int i = 0; i < 16; ++i)
        k[i] = (unsigned char)(rd() & 0xFF);
    std::ostringstream r;
    r << "GET " << path << " HTTP/1.1\r\n"
      << "Host: " << host << ":" << port << "\r\n"
      << "Upgrade: websocket\r\nConnection: Upgrade\r\n"
      << "Sec-WebSocket-Key: " << b64(k, 16) << "\r\n"
      << "Sec-WebSocket-Version: 13\r\n\r\n";
    std::string rs = r.str();
    if (!send_all(c, rs.data(), (int)rs.size()))
        return false;
    std::string h;
    char ch;
    while (h.size() < 8192)
    {
        if (recv_n(c, &ch, 1) != 1)
            return false;
        h += ch;
        if (h.size() >= 4 && h.compare(h.size() - 4, 4, "\r\n\r\n") == 0)
            break;
    }
    return h.find(" 101") != std::string::npos;
}
static bool ws_send(TlsConn *c, int op, const void *data, size_t len)
{
    std::vector<uint8_t> f;
    f.reserve(len + 14);
    f.push_back((uint8_t)(0x80 | op));
    uint8_t mask[4];
    std::random_device rd;
    for (int i = 0; i < 4; ++i)
        mask[i] = (uint8_t)(rd() & 0xFF);
    if (len < 126)
        f.push_back((uint8_t)(0x80 | len));
    else if (len < 65536)
    {
        f.push_back((uint8_t)(0x80 | 126));
        f.push_back((uint8_t)((len >> 8) & 0xFF));
        f.push_back((uint8_t)(len & 0xFF));
    }
    else
    {
        f.push_back((uint8_t)(0x80 | 127));
        for (int i = 7; i >= 0; --i)
            f.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
    }
    for (int i = 0; i < 4; ++i)
        f.push_back(mask[i]);
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i)
        f.push_back(p[i] ^ mask[i & 3]);
    return send_all(c, (const char *)f.data(), (int)f.size());
}
static int ws_recv(TlsConn *c, std::vector<uint8_t> &payload)
{
    uint8_t h[2];
    if (recv_n(c, (char *)h, 2) != 2)
        return -1;
    int op = h[0] & 0x0F;
    bool masked = (h[1] & 0x80) != 0;
    uint64_t len = h[1] & 0x7F;
    if (len == 126)
    {
        uint8_t b[2];
        if (recv_n(c, (char *)b, 2) != 2)
            return -1;
        len = ((uint64_t)b[0] << 8) | b[1];
    }
    else if (len == 127)
    {
        uint8_t b[8];
        if (recv_n(c, (char *)b, 8) != 8)
            return -1;
        len = 0;
        for (int i = 0; i < 8; ++i)
            len = (len << 8) | b[i];
    }
    uint8_t mk[4] = {0, 0, 0, 0};
    if (masked && recv_n(c, (char *)mk, 4) != 4)
        return -1;
    if (len > (8u << 20))
        return -1;
    payload.resize((size_t)len);
    if (len && recv_n(c, (char *)payload.data(), (int)len) != (int)len)
        return -1;
    if (masked)
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] ^= mk[i & 3];
    if (op == 0x8)
        return -1;
    if (op == 0x9)
    {
        ws_send(c, 0xA, payload.data(), payload.size());
        return 0;
    }
    if (op == 0xA)
        return 0;
    if (op == 0x1)
        return 1;
    if (op == 0x2)
        return 2;
    return 0;
}

// ===== control hello / outgoing (TlsConn* в Runtime) =====
static std::string make_hello_json()
{
    std::ostringstream hs;
    hs << "{\"type\":\"hello\""
       << ",\"screen_w\":" << g_screen_w.load()
       << ",\"screen_h\":" << g_screen_h.load() << "}";
    return hs.str();
}
static void ctrl_send_hello(Runtime &rt)
{
    std::lock_guard<std::mutex> lk(rt.ctrl_sock_m);
    if (!rt.ctrl_conn)
        return;
    std::string h = make_hello_json();
    ws_send(rt.ctrl_conn, 0x1, h.data(), h.size());
}
static void ctrl_send_clipboard(Runtime &rt, const std::string &text)
{
    std::string msg = std::string("{\"type\":\"clipboard\",\"text\":") + json_escape(text) + "}";
    std::lock_guard<std::mutex> lk(rt.ctrl_sock_m);
    if (!rt.ctrl_conn)
        return;
    ws_send(rt.ctrl_conn, 0x1, msg.data(), msg.size());
}

// ===== control dispatch (без изменений) =====
static void handle_control(const std::string &j)
{
    std::string type;
    if (!json_str(j, "type", type))
        return;
    if (type == "mouse_move")
    {
        int x = 0, y = 0;
        if (json_int(j, "x", x) && json_int(j, "y", y))
            do_mouse_move(x, y);
    }
    else if (type == "mouse_down" || type == "mouse_up")
    {
        int btn = 0;
        json_int(j, "button", btn);
        do_mouse_button(btn, type == "mouse_down");
    }
    else if (type == "mouse_wheel")
    {
        int d = 0;
        if (json_int(j, "delta", d))
            do_mouse_wheel(d);
    }
    else if (type == "text")
    {
        std::string text;
        if (json_str_ex(j, "text", text))
            do_text_input(text);
    }
    else if (type == "key_down" || type == "key_up")
    {
        std::string code;
        if (json_str(j, "code", code))
            do_key(code, type == "key_down");
    }
    else if (type == "clipboard")
    {
        std::string text;
        if (json_str_ex(j, "text", text))
            clipboard_write_utf8(text);
    }
}

// ===== control_loop (использует TlsConn*) =====
static void control_loop(const Config &cfg, Runtime &rt)
{
    while (!rt.stop)
    {
        TlsConn *c = tls_connect(cfg.server_host, cfg.server_port, cfg.verify_cert);
        if (!c)
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        std::string path = "/ws/control/agent/" + cfg.agent_id;
        if (!ws_handshake(c, cfg.server_host, cfg.server_port, path))
        {
            log("ctrl wss handshake failed");
            tls_close(c);
            delete c;
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        log("ctrl wss connected");

        {
            std::lock_guard<std::mutex> lk(rt.ctrl_sock_m);
            rt.ctrl_conn = c;
        }
        ctrl_send_hello(rt);

        std::vector<uint8_t> buf;
        while (!rt.stop)
        {
            int r = ws_recv(c, buf);
            if (r < 0)
                break;
            if (r == 1)
            {
                std::string msg(buf.begin(), buf.end());
                handle_control(msg);
            }
        }

        {
            std::lock_guard<std::mutex> lk(rt.ctrl_sock_m);
            rt.ctrl_conn = nullptr;
        }
        tls_close(c);
        delete c;
        log("ctrl wss disconnected, retry in 2s");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// ===== resolution watcher (без изменений) =====
static void resolution_watch_loop(Runtime &rt)
{
    using namespace std::chrono;
    while (!rt.stop)
    {
        std::this_thread::sleep_for(seconds(2));
        if (rt.stop)
            break;
        int w, h, ox, oy;
        if (!read_screen_metrics(w, h, ox, oy))
            continue;
        if (w == g_screen_w.load() && h == g_screen_h.load() &&
            ox == g_screen_origin_x.load() && oy == g_screen_origin_y.load())
            continue;
        log("resolution changed: " +
            std::to_string(g_screen_w.load()) + "x" + std::to_string(g_screen_h.load()) +
            " -> " + std::to_string(w) + "x" + std::to_string(h));
        g_screen_w = w;
        g_screen_h = h;
        g_screen_origin_x = ox;
        g_screen_origin_y = oy;
        rt.restart = true;
        ctrl_send_hello(rt);
    }
}

// ===== clipboard watcher (без изменений) =====
static void clipboard_watch_loop(Runtime &rt)
{
    {
        std::string cur = clipboard_read_utf8();
        std::lock_guard<std::mutex> lk(g_clip_m);
        g_last_clip = cur;
    }
    while (!rt.stop)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (rt.stop)
            break;
        std::string cur = clipboard_read_utf8();
        if (cur.empty())
            continue;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(g_clip_m);
            if (cur != g_last_clip)
            {
                g_last_clip = cur;
                changed = true;
            }
        }
        if (!changed)
            continue;
        if (cur.size() > 512 * 1024)
            continue;
        ctrl_send_clipboard(rt, cur);
    }
}

// ===== ffmpeg (без изменений) =====
static std::string build_ffmpeg_cmd(const Config &base, const Runtime &r)
{
    std::ostringstream c;
    c << "\"" << base.ffmpeg_path << "\" -hide_banner -loglevel warning"
      << " -f " << base.input_fmt
      << " -framerate " << r.framerate
      << " -draw_mouse 1";
    if (!base.video_size.empty())
        c << " -video_size " << base.video_size;
    c << " -i " << base.input;
    if (r.codec == "mjpeg")
    {
        c << " -f mjpeg -q:v " << r.mjpeg_q << " -pix_fmt yuvj420p pipe:1";
    }
    else
    {
        const std::string &enc = r.encoder;
        if (enc == "amf")
        {
            int gop = r.framerate * 2;
            c << " -c:v h264_amf"
              << " -usage lowlatency -quality balanced -rc vbr_latency"
              << " -b:v " << r.bitrate << " -maxrate " << r.bitrate
              << " -g " << gop << " -bf 0"
              << " -vbaq true -preanalysis true -enforce_hrd true";
        }
        else if (enc == "qsv")
        {
            c << " -c:v h264_qsv -preset veryfast -look_ahead 0"
              << " -b:v " << r.bitrate << " -maxrate " << r.bitrate
              << " -g " << r.framerate << " -bf 0";
        }
        else if (enc == "nvenc")
        {
            c << " -c:v h264_nvenc -preset p1 -tune ull -rc cbr"
              << " -b:v " << r.bitrate
              << " -g " << r.framerate << " -bf 0";
        }
        else
        {
            int gop = r.framerate * 2;
            c << " -c:v libx264 -preset veryfast -tune zerolatency"
              << " -profile:v main -pix_fmt yuv420p"
              << " -bf 0 -refs 1"
              << " -b:v " << r.bitrate
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
static HANDLE start_ffmpeg(const std::string &cmdline, PROCESS_INFORMATION &pi)
{
    SECURITY_ATTRIBUTES sa{sizeof(sa), NULL, TRUE};
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 4 * 1024 * 1024))
    {
        log("CreatePipe failed");
        return NULL;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);
    log("launching: " + cmdline);
    if (!CreateProcessA(NULL, buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
        log("CreateProcess failed, err=" + std::to_string(GetLastError()));
        CloseHandle(rd);
        CloseHandle(wr);
        return NULL;
    }
    CloseHandle(wr);
    return rd;
}

// ===== config poll (передаём verify_cert) =====
static void poll_config_loop(const Config &base, Runtime &rt)
{
    std::string last_sig;
    while (!rt.stop)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(base.config_poll_ms));
        std::string body = http_get(base.server_host, base.server_port,
                                    "/agents/" + base.agent_id + "/config",
                                    base.verify_cert);
        if (body.empty())
            continue;
        std::string codec, encoder, bitrate;
        int fps = 0, mq = 0;
        std::istringstream iss(body);
        std::string line;
        while (std::getline(iss, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "codec")
                codec = v;
            else if (k == "encoder")
                encoder = v;
            else if (k == "bitrate")
                bitrate = v;
            else if (k == "fps")
                fps = std::atoi(v.c_str());
            else if (k == "mjpeg_q")
                mq = std::atoi(v.c_str());
        }
        if (codec.empty())
            continue;
        std::string sig = codec + "|" + encoder + "|" + bitrate + "|" +
                          std::to_string(fps) + "|" + std::to_string(mq);
        if (sig == last_sig)
            continue;
        last_sig = sig;
        std::lock_guard<std::mutex> lk(rt.m);
        rt.codec = codec;
        rt.encoder = encoder.empty() ? "cpu" : encoder;
        rt.bitrate = bitrate.empty() ? "4M" : bitrate;
        if (fps > 0)
            rt.framerate = fps;
        if (mq > 0)
            rt.mjpeg_q = mq;
        rt.restart = true;
        log("config changed: codec=" + rt.codec + " encoder=" + rt.encoder +
            " bitrate=" + rt.bitrate + " fps=" + std::to_string(rt.framerate));
    }
}

// ===== streaming session (использует TlsConn*) =====
static void run_session(const Config &base, Runtime &rt)
{
    TlsConn *c = tls_connect(base.server_host, base.server_port, base.verify_cert);
    if (!c)
    {
        log("tls_connect failed");
        return;
    }

    std::string codec, encoder, bitrate;
    int fps = 30, mq = 4;
    {
        std::lock_guard<std::mutex> lk(rt.m);
        codec = rt.codec;
        encoder = rt.encoder;
        bitrate = rt.bitrate;
        fps = rt.framerate;
        mq = rt.mjpeg_q;
    }
    rt.restart = false;

    std::string ctype = (codec == "mjpeg") ? "video/x-motion-jpeg" : "video/h264";
    std::ostringstream req;
    req << "POST /ingest/" << base.agent_id << " HTTP/1.1\r\n"
        << "Host: " << base.server_host << ":" << base.server_port << "\r\n"
        << "Content-Type: " << ctype << "\r\n"
        << "X-Agent-Encoder: " << encoder << "\r\n"
        << "X-Agent-Bitrate: " << bitrate << "\r\n"
        << "X-Agent-FPS: " << fps << "\r\n"
        << "Transfer-Encoding: chunked\r\n"
        << "Connection: close\r\n\r\n";
    std::string s = req.str();
    if (!send_all(c, s.data(), (int)s.size()))
    {
        tls_close(c);
        delete c;
        return;
    }

    Runtime snap;
    snap.codec = codec;
    snap.encoder = encoder;
    snap.bitrate = bitrate;
    snap.framerate = fps;
    snap.mjpeg_q = mq;
    std::string cmd = build_ffmpeg_cmd(base, snap);

    PROCESS_INFORMATION pi{};
    HANDLE pipe = start_ffmpeg(cmd, pi);
    if (!pipe)
    {
        tls_close(c);
        delete c;
        return;
    }

    std::vector<char> buf(64 * 1024);
    auto t0 = std::chrono::steady_clock::now();
    uint64_t bytes = 0;
    while (!rt.stop)
    {
        if (rt.restart)
        {
            log("restart requested");
            break;
        }
        DWORD n = 0;
        if (!ReadFile(pipe, buf.data(), (DWORD)buf.size(), &n, NULL) || n == 0)
        {
            log("ffmpeg pipe closed");
            break;
        }
        if (!send_chunk(c, buf.data(), (int)n))
        {
            log("send failed");
            break;
        }
        bytes += n;
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
        if (ms >= 5000)
        {
            double kbps = (bytes * 8.0) / ms;
            log("[" + codec + "/" + encoder + "] " + std::to_string((int)kbps) + " kbit/s");
            t0 = now;
            bytes = 0;
        }
    }
    send_all(c, "0\r\n\r\n", 5);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(pipe);
    tls_close(c);
    delete c;
}

// ===== CLI =====
static void parse_cli(Config &c, int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        // Флаги без значения
        if (a == "--insecure")
        {
            c.verify_cert = false;
            continue;
        }
        if (a == "--https" || a == "--https=true")
        { /* уже по умолчанию 443 порт */
            continue;
        }

        auto eq = a.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);

        if (k == "--server")
            c.server_host = v;
        else if (k == "--port")
            c.server_port = std::stoi(v);
        else if (k == "--id")
            c.agent_id = v;
        else if (k == "--codec")
            c.codec = v;
        else if (k == "--encoder")
            c.encoder = v;
        else if (k == "--bitrate")
            c.bitrate = v;
        else if (k == "--fps")
            c.framerate = std::stoi(v);
        else if (k == "--quality")
            c.mjpeg_q = std::stoi(v);
        else if (k == "--ffmpeg")
            c.ffmpeg_path = v;
        else if (k == "--input")
            c.input = v;
        else if (k == "--format")
            c.input_fmt = v;
        else if (k == "--size")
            c.video_size = v;
        else if (k == "--https")
        {
            if (v == "false")
                c.server_port = 8000;
        } // HTTP fallback
    }
}

int main(int argc, char **argv)
{
    Config cfg;
    parse_cli(cfg, argc, argv);
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0)
    {
        log("WSAStartup failed");
        return 1;
    }

    init_screen_metrics();

    Runtime rt;
    rt.codec = cfg.codec;
    rt.encoder = cfg.encoder;
    rt.bitrate = cfg.bitrate;
    rt.framerate = cfg.framerate;
    rt.mjpeg_q = cfg.mjpeg_q;

    log("agent_id=" + cfg.agent_id + " -> https://" + cfg.server_host + ":" +
        std::to_string(cfg.server_port) +
        " codec=" + rt.codec + " encoder=" + rt.encoder +
        (cfg.verify_cert ? "" : " [insecure]"));

    std::thread ctrl_thread(control_loop, std::cref(cfg), std::ref(rt));
    std::thread cfg_thread(poll_config_loop, std::cref(cfg), std::ref(rt));
    std::thread res_thread(resolution_watch_loop, std::ref(rt));
    std::thread clip_thread(clipboard_watch_loop, std::ref(rt));

    while (!rt.stop)
    {
        run_session(cfg, rt);
        if (!rt.stop)
            std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    rt.stop = true;
    cfg_thread.join();
    ctrl_thread.join();
    res_thread.join();
    clip_thread.join();
    WSACleanup();
    return 0;
}
