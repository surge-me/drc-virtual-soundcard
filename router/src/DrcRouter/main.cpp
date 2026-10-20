/*
 * DrcRouter — 用户态虚拟声卡管道（TASK-006 audio-router）
 *
 *   虚拟设备捕获端点（默认匹配名含 "CABLE Output"）
 *        -> dsp-core（include apo/dsp/include/drc/dsp.h，参数经 IPC）
 *        -> 指定渲染设备 / 默认渲染设备（WASAPI 共享模式）
 *
 * 注意：渲染端点**应显式用 --render 指定真实设备**（GUI 会传）。
 *       虚拟线场景要求 Windows 默认播放设备 = CABLE Input（游戏声才进线），
 *       若 router 也走「默认渲染设备」就会把处理后的音频送回虚拟线 ⇒ 自递归。
 *
 * 用法：
 *   DrcRouter.exe [--capture <name-substr>] [--render <name-substr>]
 *                 [--buffer-ms <n>] [--duration-ms <n>]
 *                 [--list-devices] [--help]
 *
 * 退出码：0 = 正常；2 = 用法错误；3 = 未检测到虚拟声卡；4 = 音频初始化失败；5 = 格式不匹配。
 */
#include <router/router_engine.h>
#include <router/wasapi.h>

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI CtrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stop.store(true);
            return TRUE;
        default:
            return FALSE;
    }
}

void PrintUsage() {
    std::printf(
        "DrcRouter — 用户态虚拟声卡管道（捕获 -> dsp-core -> 渲染设备）\n"
        "\n"
        "用法：\n"
        "  DrcRouter.exe [选项]\n"
        "\n"
        "选项：\n"
        "  --capture <子串>    捕获端点名匹配子串（默认 \"CABLE Output\"）\n"
        "  --render  <子串>    渲染端点名匹配子串（默认：系统默认渲染设备）\n"
        "                      匹配口径 = 友好名 + 空格 + 设备描述，均不区分大小写，支持中文\n"
        "  --buffer-ms <n>     引擎缓冲时长，毫秒（默认 10）\n"
        "  --duration-ms <n>   运行 n 毫秒后自动退出（默认：直到 Ctrl+C）\n"
        "  --simulate-device-loss-ms <n>\n"
        "                      测试钩子：启动 n ms 后注入一次「端点失效」（持续 3s，单发；\n"
        "                      默认 0 = 关闭）。用于验证端点热插拔自愈，生产不用。\n"
        "  --list-devices      列出活动音频端点后退出\n"
        "  --help              显示本帮助\n"
        "\n"
        "退出码：0 正常 / 2 用法 / 3 未检测到虚拟声卡 / 4 音频初始化失败 / 5 格式不匹配\n");
}

void PrintEndpoints(const char* title, const std::vector<router::EndpointInfo>& eps) {
    std::printf("[%s] %zu 个活动端点\n", title, eps.size());
    for (const router::EndpointInfo& e : eps) {
        std::wprintf(L"    - %ls  (%ls)\n",
                     e.friendlyName.empty() ? L"(无名)" : e.friendlyName.c_str(),
                     e.deviceDesc.empty() ? L"-" : e.deviceDesc.c_str());
    }
}

}  /* namespace */

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    /* 参数来源必须是 Unicode 命令行：CRT 的 argv 按 ANSI 代码页解码，
     * 中文设备名（如「扬声器 <耳机>」）会被解成错误字符 ⇒ --render 永远匹配不到（实测 exit 4）。 */
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv == nullptr) {
        std::fprintf(stderr, "CommandLineToArgvW 失败\n");
        return 2;
    }

    std::wstring captureNeedle = L"CABLE Output";
    std::wstring renderNeedle;
    double bufferMs = 10.0;
    int durationMs = 0;
    unsigned simulateLossAfterMs = 0u;
    bool listDevices = false;

    for (int i = 1; i < wargc; ++i) {
        const std::wstring a = wargv[i];
        auto next = [&](std::wstring* dst) -> bool {
            if (i + 1 >= wargc) {
                return false;
            }
            *dst = wargv[++i];
            return true;
        };
        if (a == L"--help" || a == L"-h" || a == L"/?") {
            PrintUsage();
            LocalFree(wargv);
            return 0;
        } else if (a == L"--list-devices") {
            listDevices = true;
        } else if (a == L"--capture") {
            if (!next(&captureNeedle)) {
                std::fprintf(stderr, "--capture 缺少参数\n");
                LocalFree(wargv);
                return 2;
            }
        } else if (a == L"--render") {
            if (!next(&renderNeedle)) {
                std::fprintf(stderr, "--render 缺少参数\n");
                LocalFree(wargv);
                return 2;
            }
        } else if (a == L"--buffer-ms") {
            std::wstring v;
            if (!next(&v)) {
                std::fprintf(stderr, "--buffer-ms 缺少参数\n");
                LocalFree(wargv);
                return 2;
            }
            bufferMs = _wtof(v.c_str());
            if (!(bufferMs > 0.0)) {
                bufferMs = 10.0;
            }
        } else if (a == L"--duration-ms") {
            std::wstring v;
            if (!next(&v)) {
                std::fprintf(stderr, "--duration-ms 缺少参数\n");
                LocalFree(wargv);
                return 2;
            }
            durationMs = _wtoi(v.c_str());
        } else if (a == L"--simulate-device-loss-ms") {
            std::wstring v;
            if (!next(&v)) {
                std::fprintf(stderr, "--simulate-device-loss-ms 缺少参数\n");
                LocalFree(wargv);
                return 2;
            }
            const int n = _wtoi(v.c_str());
            simulateLossAfterMs = (n > 0) ? static_cast<unsigned>(n) : 0u;
        } else {
            std::fwprintf(stderr, L"未知参数：%ls（用 --help 查看用法）\n", a.c_str());
            LocalFree(wargv);
            return 2;
        }
    }
    LocalFree(wargv);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::fprintf(stderr, "CoInitializeEx 失败（0x%08lX）\n",
                     static_cast<unsigned long>(hr));
        return 4;
    }

    if (listDevices) {
        PrintEndpoints("Render", router::EnumActiveEndpoints(eRender));
        PrintEndpoints("Capture", router::EnumActiveEndpoints(eCapture));
        CoUninitialize();
        return 0;
    }

    router::EndpointInfo captureEndpoint;
    std::vector<router::EndpointInfo> allCapture;
    if (!router::ResolveVirtualCapture(captureNeedle, &captureEndpoint, &allCapture)) {
        std::fwprintf(stderr,
                      L"未检测到虚拟声卡（需安装 VB-CABLE 或等价虚拟设备）："
                      L"捕获端点中无名称含 \"%ls\" 的设备。\n",
                      captureNeedle.c_str());
        PrintEndpoints("可用捕获端点", allCapture);
        CoUninitialize();
        return 3;
    }
    std::wprintf(L"捕获端点：%ls\n", captureEndpoint.friendlyName.c_str());

    std::wstring renderId;
    if (!renderNeedle.empty()) {
        std::vector<router::EndpointInfo> rends = router::EnumActiveEndpoints(eRender);
        const router::EndpointInfo* hit = router::FindEndpointByName(rends, renderNeedle);
        if (hit == nullptr) {
            std::fwprintf(stderr, L"渲染端点中无名称含 \"%ls\" 的设备。\n",
                          renderNeedle.c_str());
            PrintEndpoints("可用渲染端点", rends);
            CoUninitialize();
            return 4;
        }
        renderId = hit->id;
        std::wprintf(L"渲染端点：%ls\n", hit->friendlyName.c_str());
    } else {
        router::EndpointInfo def = router::GetDefaultEndpoint(eRender);
        std::wprintf(L"渲染端点（默认）：%ls\n", def.friendlyName.c_str());
    }

    router::RouterConfig cfg;
    cfg.bufferMs = bufferMs;
    cfg.maxSeconds = (durationMs > 0) ? ((durationMs + 999) / 1000) : 0;
    /* 名字匹配串交给引擎：端点热插拔（耳机断开/重连）后要按**名字**重新解析端点 ID。 */
    cfg.captureName = captureNeedle;
    cfg.renderName = renderNeedle;
    cfg.simulateLossAfterMs = simulateLossAfterMs;   /* 0 = 关闭（生产恒为 0） */

    router::RouterEngine engine;
    const router::RouterExitCode rc = engine.Open(cfg, captureEndpoint.id, renderId);
    if (rc != router::RouterExitCode::Ok) {
        const router::RouterStats& s = engine.Stats();
        if (rc == router::RouterExitCode::FormatMismatch) {
            std::fprintf(stderr,
                         "捕获/渲染格式不匹配：capture[%s] render[%s]"
                         "（要求采样率与通道数一致）\n",
                         s.captureFormat.c_str(), s.renderFormat.c_str());
        } else {
            std::fprintf(stderr, "音频初始化失败：capture[%s] render[%s]\n",
                         s.captureFormat.c_str(), s.renderFormat.c_str());
        }
        CoUninitialize();
        return static_cast<int>(rc);
    }

    const router::RouterStats& s0 = engine.Stats();
    std::printf("capture : %s (buffer %.2f ms)\n", s0.captureFormat.c_str(),
                s0.captureBufferMs);
    std::printf("render  : %s (buffer %.2f ms)\n", s0.renderFormat.c_str(),
                s0.renderBufferMs);
    std::printf("dsp-core: %s / IPC: %s\n", s0.dspAvailable ? "已接线" : "直通",
                s0.ipcOk ? "已接管" : "未接管（不写 meter）");
    const double addedMs = s0.captureBufferMs + s0.renderBufferMs;
    std::printf("附加延迟预算 : %.2f ms（capture %.2f + render %.2f）%s\n", addedMs,
                s0.captureBufferMs, s0.renderBufferMs,
                (addedMs > 30.0) ? "  [警告] 超过 30ms 目标" : "");
    std::printf("运行中…（端点热插拔会自动重连；Ctrl+C 退出）\n");
    std::fflush(stdout);

    const int runRc = engine.Run(g_stop);

    const router::RouterStats& s = engine.Stats();
    if (s.deviceLost) {
        std::fprintf(stderr, "运行期端点丢失 %u 次，成功重连 %u 次。\n",
                     s.endpointLosses, s.reconnects);
    }
    std::printf("停止：frames=%llu packets=%llu underruns=%llu overflow=%llu "
                "dspBypassBlocks=%llu endpointLosses=%u reconnects=%u\n",
                static_cast<unsigned long long>(s.framesProcessed),
                static_cast<unsigned long long>(s.capturePackets),
                static_cast<unsigned long long>(s.underruns),
                static_cast<unsigned long long>(s.overflowDrops),
                static_cast<unsigned long long>(s.dspPassthroughBlocks),
                s.endpointLosses, s.reconnects);

    if (runRc != 0) {
        std::fprintf(stderr, "运行期启动失败（退出码 %d）：capture/render Start() 未成功。\n",
                     runRc);
    }

    engine.Close();
    CoUninitialize();
    return runRc;   /* 启动失败不得被吞成 0（F2） */
}
