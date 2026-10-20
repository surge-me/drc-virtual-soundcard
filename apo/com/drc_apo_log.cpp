#include "drc_apo_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace drc {

namespace {

const wchar_t kDefaultLogPath[] = L"C:\\Windows\\Temp\\DrcApo_log.txt";
const wchar_t kEnvLogPath[]     = L"DRC_APO_LOG_PATH";

HANDLE g_hLog     = INVALID_HANDLE_VALUE;
LONG   g_logState = 0;   /* 0 = 未初始化, 1 = 初始化中, 2 = 就绪, -1 = 失败 */
LONG   g_logGate  = 0;   /* Interlocked 单写者门 */

/* Interlocked 保护的惰性打开；失败置 -1 并永久静默。 */
HANDLE EnsureLog() noexcept {
    const LONG s = InterlockedCompareExchange(&g_logState, 1, 0);
    if (s == 0) {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(kEnvLogPath, path, MAX_PATH);
        const wchar_t* use = (n > 0u && n < MAX_PATH) ? path : kDefaultLogPath;

        HANDLE h = CreateFileW(use, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        g_hLog = (h == INVALID_HANDLE_VALUE) ? nullptr : h;
        InterlockedExchange(&g_logState, (g_hLog != nullptr) ? 2 : -1);
    } else if (s == 1) {
        /* 另一线程正在初始化：有界自旋等待（不长时间阻塞调用线程） */
        for (int i = 0; i < 2000 && InterlockedCompareExchange(&g_logState, 1, 1) == 1; ++i) {
            Sleep(0);
        }
    }
    return (InterlockedCompareExchange(&g_logState, 2, 2) == 2) ? g_hLog : nullptr;
}

/* 单次原子追加（OVERLAPPED 偏移 0xFFFFFFFF = 写到文件尾）。失败静默。 */
void WriteLine(const char* line, std::size_t len) noexcept {
    HANDLE h = EnsureLog();
    if (h == nullptr || len == 0u) {
        return;
    }

    int spins = 0;
    while (InterlockedCompareExchange(&g_logGate, 1, 0) != 0) {
        if (++spins > 100000) {
            break;   /* 有界：门被占也仍以单次原子追加写，绝不长阻塞调用线程 */
        }
        Sleep(0);
    }

    OVERLAPPED ov{};
    ov.Offset     = 0xFFFFFFFFu;
    ov.OffsetHigh = 0xFFFFFFFFu;
    DWORD written = 0;
    WriteFile(h, line, static_cast<DWORD>(len), &written, &ov);
    InterlockedExchange(&g_logGate, 0);
}

}  /* namespace */

const char* GuidToAnsi(const GUID& g, char* buf, int bufLen) noexcept {
    if (buf == nullptr || bufLen <= 0) {
        return "";
    }
    std::snprintf(buf, static_cast<std::size_t>(bufLen),
                  "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                  g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

void ApoLog(const char* fmt, ...) noexcept {
#if DRC_APO_LOG
    if (fmt == nullptr) {
        return;
    }
    char body[768] = {};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[1024] = {};
    const int n = std::snprintf(line, sizeof(line),
                                "[%02u:%02u:%02u.%03u][pid=%lu][tid=%lu] %s\r\n",
                                static_cast<unsigned>(st.wHour),
                                static_cast<unsigned>(st.wMinute),
                                static_cast<unsigned>(st.wSecond),
                                static_cast<unsigned>(st.wMilliseconds),
                                static_cast<unsigned long>(GetCurrentProcessId()),
                                static_cast<unsigned long>(GetCurrentThreadId()), body);
    if (n > 0) {
        std::size_t len = static_cast<std::size_t>(n);
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1u;
        }
        WriteLine(line, len);
    }
#else
    (void)fmt;
#endif
}

void ApoLogModule(const char* what, HINSTANCE hinst) noexcept {
#if DRC_APO_LOG
    wchar_t wpath[MAX_PATH] = {};
    if (GetModuleFileNameW(hinst, wpath, MAX_PATH) == 0u) {
        wcscpy_s(wpath, L"(unknown)");
    }
    /* 用 ANSI(CP_ACP) 而非 UTF-8：日志其余部分全 ASCII，整文件单一编码，
     * 便于旧式读取器（cmd type / PowerShell 5.1 Get-Content 默认 ANSI）正确显示中文路径。 */
    char path[MAX_PATH * 3] = {};
    if (WideCharToMultiByte(CP_ACP, 0, wpath, -1, path,
                            static_cast<int>(sizeof(path)), nullptr, nullptr) <= 0) {
        path[0] = '\0';
    }
    ApoLog("DllMain(%s) hinst=0x%p path=%s",
           (what != nullptr) ? what : "?", static_cast<const void*>(hinst), path);
#else
    (void)what;
    (void)hinst;
#endif
}

}  /* namespace drc */
