// test_router.cpp — audio-router 离线可机械验证宿主（TASK-006）
//   R1 DSP 接线       ：喂正弦 buffer -> 断言与 tests/dsp 同判据（限幅/上抬/旁路 bit-exact）
//   R2 IPC 回路       ：router 写 meter -> 另一句柄读回；seqlock / 口径（SPEC §4.1）
//   R3 WASAPI 探测    ：真实设备上只枚举 + 格式协商（不注入不路由）
//   R4 loopback 自捕获：播放测试音到默认设备 + 同步 loopback 捕获，断言含该频率成分（只捕获不回放）
//   R5 F2 哨兵        ：启动失败不得被吞成 0（环境无关）
//   R6 端点看护策略   ：Part A = DecideWarden 映射（环境无关）；Part B = Run() 真的调用该函数（需能 Open）
//
// 用法：router_tests <R1|R2|R3|R4|R5|R6>；退出码 0 = PASS，1 = FAIL，2 = 用法错误，77 = SKIP。
#include <router/dsp_bridge.h>
#include <router/router_engine.h>
#include <router/shm_channel.h>
#include <router/wasapi.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSkipExitCode = 77;   /* ctest SKIP_RETURN_CODE：环境级缺音频端点 */
int g_fail = 0;
int g_skip = 0;

/* 环境级缺设备判据（必须两条独立信号同时成立，避免掩盖枚举代码缺陷）：
 *   ① MMDevice 全状态枚举为空；② winmm 独立探针报告 0 个输出设备。 */
bool NoAudioEnvironment() {
    return router::WinmmWaveOutDeviceCount() == 0 &&
           router::EnumEndpoints(eRender, 0xFu).empty() &&
           router::EnumEndpoints(eCapture, 0xFu).empty();
}

void PrintSkipBanner(const char* which) {
    std::printf("  [SKIP] %s：本会话/本机无可用音频端点（MMDevice 全状态枚举为空 且 "
                "winmm 报告 0 输出设备）——环境级缺设备，非代码缺陷。\n", which);
    std::printf("         %s 需在有音频端点的交互式会话中重跑（用户在场时）。\n", which);
}

void Report(const char* name, double got, double lo, double hi, const char* unit) {
    const bool ok = (got >= lo && got <= hi);
    std::printf("  [%s] %-56s got=%.6f  要求[%.4f, %.4f]%s\n", ok ? "OK" : "FAIL",
                name, got, lo, hi, unit);
    if (!ok) ++g_fail;
}

void ReportTrue(const char* name, bool ok, const char* detail) {
    std::printf("  [%s] %-56s %s\n", ok ? "OK" : "FAIL", name, detail);
    if (!ok) ++g_fail;
}

void Note(const char* name, double v, const char* unit) {
    std::printf("  [--] %-56s = %.6f%s\n", name, v, unit);
}

void NoteStr(const char* name, const std::string& v) {
    std::printf("  [--] %-56s = %s\n", name, v.c_str());
}

std::vector<float> MakeSine(int fs, double freq, double amp, std::size_t frames, int ch) {
    std::vector<float> v(frames * static_cast<std::size_t>(ch), 0.0f);
    for (std::size_t i = 0; i < frames; ++i) {
        const double s = amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) /
                                        static_cast<double>(fs));
        for (int c = 0; c < ch; ++c) {
            v[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)] =
                static_cast<float>(s);
        }
    }
    return v;
}

double Peak(const std::vector<float>& v, int ch, std::size_t from, std::size_t to) {
    double m = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        for (int c = 0; c < ch; ++c) {
            m = std::max(m, std::fabs(static_cast<double>(
                                v[i * static_cast<std::size_t>(ch) +
                                  static_cast<std::size_t>(c)])));
        }
    }
    return m;
}

inline double LinToDb(double lin) { return 20.0 * std::log10(std::max(lin, 1e-30)); }
inline double DbToLin(double db) { return std::pow(10.0, db / 20.0); }

/* ── R1：DSP 接线（判据同 tests/dsp T1/T2/T4） ─────────────────────────── */
void TestR1() {
    const int fs = 48000, ch = 1;
    router::RouterParams p;   /* 默认：ceiling -6 / floor -50 / maxBoost 12 */
    const std::size_t frames = static_cast<std::size_t>(fs) / 2;

    // 限幅：0dBFS 稳态正弦 -> 输出峰值贴住 ceiling（±0.1dB）
    {
        router::DspBridge bridge;
        ReportTrue("R1 DspBridge.Configure(48000,1)", bridge.Configure(fs, ch), "");
        std::vector<float> buf = MakeSine(fs, 1000.0, 1.0, frames, ch);
        const std::size_t block = 480;
        for (std::size_t i = 0; i < frames; i += block) {
            const std::size_t n = std::min(block, frames - i);
            bridge.Process(buf.data() + i * ch, n, fs, ch, p);
        }
        const double peakDb = LinToDb(Peak(buf, ch, frames / 2, frames));
        Report("R1 0dBFS 输出峰值 ≤ ceiling+0.1dB", peakDb, -1e9, p.ceilingDb + 0.1, " dBFS");
        Report("R1 0dBFS 输出峰值 ≥ ceiling-0.1dB", peakDb, p.ceilingDb - 0.1, 1e9, " dBFS");
    }

    // 上抬：-60dBFS 稳态正弦 -> 抬向 floor（±2dB）
    // TASK-014 起上抬是分频结构（主段 1.5–4kHz），探针必须落在主段内：
    // 1kHz 位于辅段下限边缘，只能拿到部分增量（实测 -52.05dBFS），旧口径会假红。
    {
        router::DspBridge bridge;
        bridge.Configure(fs, ch);
        std::vector<float> buf = MakeSine(fs, 2500.0, DbToLin(-60.0), frames, ch);
        const std::size_t block = 480;
        for (std::size_t i = 0; i < frames; i += block) {
            const std::size_t n = std::min(block, frames - i);
            bridge.Process(buf.data() + i * ch, n, fs, ch, p);
        }
        const double peakDb = LinToDb(Peak(buf, ch, frames / 2, frames));
        Report("R1 段内-60dBFS@2.5kHz 输出 ≈ floor(±2dB)", peakDb, p.floorDb - 2.0, p.floorDb + 2.0, " dBFS");
    }

    // 旁路：bypass=true -> 逐样本 bit-exact
    {
        router::DspBridge bridge;
        bridge.Configure(fs, ch);
        router::RouterParams bp = p;
        bp.bypass = true;
        std::vector<float> in = MakeSine(fs, 1000.0, 0.9, frames, ch);
        std::vector<float> buf = in;
        const bool processed = bridge.Process(buf.data(), frames, fs, ch, bp);
        ReportTrue("R1 bypass 时 Process 返回 false（直通）", !processed, "");
        ReportTrue("R1 bypass 输出与输入 bit-exact",
                   std::memcmp(in.data(), buf.data(), in.size() * sizeof(float)) == 0, "");
    }
}

/* ── R2：IPC 回路（seqlock / 口径 SPEC §4.1） ─────────────────────────── */
void TestR2() {
    // 口径：0 = 0dBFS、5000 = -50dBFS
    Report("R2 DbToX100(0dBFS) == 0", static_cast<double>(router::ShmChannel::DbToX100(0.0f)),
           0.0, 0.0, "");
    Report("R2 DbToX100(-50dBFS) == 5000",
           static_cast<double>(router::ShmChannel::DbToX100(-50.0f)), 5000.0, 5000.0, "");
    Report("R2 DbToX100(-6dBFS) == 600",
           static_cast<double>(router::ShmChannel::DbToX100(-6.0f)), 600.0, 600.0, "");
    // 有区分力：口径不是恒等映射
    ReportTrue("R2 口径非恒等（-50dBFS != 50）",
               router::ShmChannel::DbToX100(-50.0f) != 50u, "");

    router::ShmChannel writer;
    writer.OpenOrCreate();
    ReportTrue("R2 写入侧 OpenOrCreate -> Ok", writer.Ok(), "");

    router::ShmChannel reader;   // 同一进程第二个句柄 => 附着同一映射
    reader.OpenOrCreate();
    ReportTrue("R2 读取侧 OpenOrCreate -> Ok（跨句柄可见）", reader.Ok(), "");
    if (!writer.Ok() || !reader.Ok()) {
        return;
    }

    // 参数回路（seqlock 双写）
    DrcParams p{};
    router::ShmChannel::FillDefaults(&p);
    p.ceilingDb = -12.0f;
    p.floorDb = -40.0f;
    p.eqGainDb[5] = 6.0f;
    p.flags = DRC_FLAG_BYPASS;
    ReportTrue("R2 WriteParams -> true", writer.WriteParams(p), "");
    DrcParams got{};
    const bool stable = reader.ReadParams(&got);
    ReportTrue("R2 ReadParams 读到稳定快照", stable, "");
    Report("R2 ceilingDb 往返一致", static_cast<double>(got.ceilingDb), -12.0, -12.0, "");
    Report("R2 floorDb 往返一致", static_cast<double>(got.floorDb), -40.0, -40.0, "");
    Report("R2 eqGainDb[5] 往返一致", static_cast<double>(got.eqGainDb[5]), 6.0, 6.0, "");
    ReportTrue("R2 flags(bypass) 往返一致", (got.flags & DRC_FLAG_BYPASS) != 0u, "");

    // meter 回路
    DrcMeter m{};
    m.magic = DRC_MAGIC;
    m.version = DRC_METER_VERSION;
    m.activeSampleRate = 48000u;
    m.activeChannels = 2u;
    m.gainReductionDb_x100 = 1234u;
    m.inPeakDb_x100 = router::ShmChannel::DbToX100(-3.0f);
    m.outPeakDb_x100 = router::ShmChannel::DbToX100(-6.0f);
    m.latencyMs_x10 = 25u;
    m.framesProcessed = 48000ull;
    writer.WriteMeter(m);
    DrcMeter gm{};
    const bool mOk = reader.ReadMeter(&gm);
    ReportTrue("R2 ReadMeter 读到稳定快照", mOk, "");
    ReportTrue("R2 meter.magic == DRC_MAGIC", gm.magic == DRC_MAGIC, "");
    Report("R2 meter.activeSampleRate 往返", static_cast<double>(gm.activeSampleRate),
           48000.0, 48000.0, "");
    Report("R2 meter.gainReductionDb_x100 往返",
           static_cast<double>(gm.gainReductionDb_x100), 1234.0, 1234.0, "");
    Report("R2 meter.inPeakDb_x100 == 300（-3dBFS）",
           static_cast<double>(gm.inPeakDb_x100), 300.0, 300.0, "");
    Report("R2 meter.framesProcessed 往返",
           static_cast<double>(gm.framesProcessed), 48000.0, 48000.0, "");
    ReportTrue("R2 meter.seq 为偶（写后稳定）", (gm.seq % 2u) == 0u, "");
    Note("  meter.seq", static_cast<double>(gm.seq), "");

    // seqlock 步进：每次写 ++ 两次（偶）
    const uint32_t s1 = gm.seq;
    writer.WriteMeter(m);
    DrcMeter gm2{};
    reader.ReadMeter(&gm2);
    Report("R2 连续两次写 seq 递增 2", static_cast<double>(gm2.seq - s1), 2.0, 2.0, "");
}

/* ── R3：WASAPI 枚举 / 格式协商（不注入不路由） ───────────────────────── */
void TestR3() {
    if (NoAudioEnvironment()) {
        PrintSkipBanner("R3");
        g_skip = kSkipExitCode;
        return;
    }
    std::printf("  [--] winmm 输出设备数 = %d\n", router::WinmmWaveOutDeviceCount());
    const std::vector<router::EndpointInfo> renders = router::EnumActiveEndpoints(eRender);
    const HRESULT enumHr = router::EnumLastHr();
    const std::vector<router::EndpointInfo> caps = router::EnumActiveEndpoints(eCapture);
    std::printf("  [--] EnumActiveEndpoints(render) lastHr=0x%08lX\n",
                static_cast<unsigned long>(enumHr));
    std::printf("  [--] 活动渲染端点 %zu 个：\n", renders.size());
    for (const router::EndpointInfo& e : renders) {
        std::printf("        %ls\n", e.friendlyName.c_str());
    }
    {
        const std::vector<router::EndpointInfo> allR = router::EnumEndpoints(eRender, 0xFu);
        const std::vector<router::EndpointInfo> allC = router::EnumEndpoints(eCapture, 0xFu);
        std::printf("  [--] DEVICE_STATE_ALL 渲染 %zu / 捕获 %zu 个\n",
                    allR.size(), allC.size());
        for (const router::EndpointInfo& e : allR) {
            std::printf("        (R) %ls\n", e.friendlyName.c_str());
        }
        for (const router::EndpointInfo& e : allC) {
            std::printf("        (C) %ls\n", e.friendlyName.c_str());
        }
    }
    std::printf("  [--] 活动捕获端点 %zu 个：\n", caps.size());
    for (const router::EndpointInfo& e : caps) {
        std::printf("        %ls\n", e.friendlyName.c_str());
    }
    ReportTrue("R3 至少一个活动渲染端点", !renders.empty(), "");

    const router::EndpointInfo def = router::GetDefaultEndpoint(eRender);
    ReportTrue("R3 默认渲染端点存在", !def.id.empty(), "");
    if (def.id.empty()) {
        return;
    }
    std::printf("  [--] 默认渲染端点：%ls\n", def.friendlyName.c_str());

    router::WasapiRender render;
    const bool opened = render.Open(std::wstring(), 10.0, 0, 0);
    ReportTrue("R3 默认渲染设备 float32 共享模式可打开", opened,
               opened ? "" : "(Open 失败)");
    if (!opened) {
        std::wprintf(L"  [--] 失败原因：%ls\n", render.LastError().c_str());
        return;
    }
    NoteStr("  协商连接格式", render.NegotiatedFormat());
    ReportTrue("R3 协商格式为 float32", render.Format() == router::SampleFormat::Float32, "");
    ReportTrue("R3 采样率 > 0", render.SampleRate() > 0, "");
    ReportTrue("R3 通道数 > 0", render.Channels() > 0, "");
    Note("  buffer 帧数", static_cast<double>(render.BufferFrames()), " frames");
}

/* ── R4：loopback 自捕获（只捕获不回放） ─────────────────────────────── */
double GoertzelMag(const std::vector<double>& x, double f, double fs) {
    const std::size_t N = x.size();
    if (N == 0u) {
        return 0.0;
    }
    const double w = 2.0 * kPi * f / fs;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        const double s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) /
           static_cast<double>(N);
}

void TestR4() {
    if (NoAudioEnvironment()) {
        PrintSkipBanner("R4");
        g_skip = kSkipExitCode;
        return;
    }
    const router::EndpointInfo def = router::GetDefaultEndpoint(eRender);
    ReportTrue("R4 默认渲染端点存在", !def.id.empty(), "");
    if (def.id.empty()) {
        return;
    }

    router::WasapiRender render;
    if (!render.Open(def.id, 10.0, 0, 0)) {
        ReportTrue("R4 打开默认渲染设备", false, "(Open 失败)");
        std::wprintf(L"  [--] 失败原因：%ls\n", render.LastError().c_str());
        return;
    }
    router::WasapiCapture capture;
    if (!capture.Open(def.id, 10.0, /*loopback=*/true)) {
        ReportTrue("R4 打开回环捕获", false, "(Open 失败)");
        std::wprintf(L"  [--] 失败原因：%ls\n", capture.LastError().c_str());
        return;
    }
    NoteStr("  渲染连接格式", render.NegotiatedFormat());
    NoteStr("  回环捕获格式", capture.NegotiatedFormat());

    const int fs = render.SampleRate();
    const int rch = render.Channels();
    const int cch = capture.Channels();
    const double f0 = 1000.0;
    const double offF = 3000.0;
    const double amp = 0.1;   /* -20dBFS：短促、低幅，避免扰人 */

    if (!render.Start()) {
        ReportTrue("R4 渲染 Start", false, "");
        return;
    }
    if (!capture.Start()) {
        ReportTrue("R4 回环捕获 Start", false, "");
        render.Stop();
        return;
    }

    std::vector<double> acc;   /* 单声道（取第 0 通道） */
    double phase = 0.0;
    const double dphase = 2.0 * kPi * f0 / static_cast<double>(fs);
    std::vector<float> scratch;
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 1400ull) {
        // 渲染：持续喂 1kHz 正弦（默认设备播放）
        UINT32 padding = 0u;
        if (render.GetPadding(&padding)) {
            const UINT32 free = render.BufferFrames() - padding;
            if (free > 0u) {
                BYTE* p = nullptr;
                if (render.GetBuffer(free, &p) && p != nullptr) {
                    const std::size_t n = static_cast<std::size_t>(free) *
                                          static_cast<std::size_t>(rch);
                    scratch.assign(n, 0.0f);
                    for (UINT32 f = 0; f < free; ++f) {
                        const float v = static_cast<float>(amp * std::sin(phase));
                        phase += dphase;
                        if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
                        for (int c = 0; c < rch; ++c) {
                            scratch[static_cast<std::size_t>(f) *
                                        static_cast<std::size_t>(rch) +
                                    static_cast<std::size_t>(c)] = v;
                        }
                    }
                    if (render.Format() == router::SampleFormat::Float32) {
                        std::memcpy(p, scratch.data(), n * sizeof(float));
                    } else {
                        router::FloatToPcm(scratch.data(), render.Format(), p, n);
                    }
                    render.ReleaseBuffer(free, 0u);
                }
            }
        }
        // 捕获：回环（只捕获）
        UINT32 pk = 0u;
        while (capture.GetNextPacketSize(&pk) && pk > 0u) {
            BYTE* d = nullptr;
            UINT32 fr = 0u;
            DWORD fl = 0u;
            if (!capture.GetPacket(&d, &fr, &fl)) {
                break;
            }
            if (fr > 0u && d != nullptr && (fl & AUDCLNT_BUFFERFLAGS_SILENT) == 0u) {
                const std::size_t n = static_cast<std::size_t>(fr) *
                                      static_cast<std::size_t>(cch);
                std::vector<float> tmp(n, 0.0f);
                if (capture.Format() == router::SampleFormat::Float32) {
                    std::memcpy(tmp.data(), d, n * sizeof(float));
                } else {
                    router::PcmToFloat(d, capture.Format(), tmp.data(), n);
                }
                for (UINT32 f = 0; f < fr; ++f) {
                    acc.push_back(static_cast<double>(
                        tmp[static_cast<std::size_t>(f) *
                            static_cast<std::size_t>(cch)]));
                }
            }
            capture.ReleasePacket(fr);
        }
        Sleep(3);
    }

    render.Stop();
    capture.Stop();

    Note("  捕获样本数", static_cast<double>(acc.size()), " samples");
    ReportTrue("R4 回环捕获到数据（> 0.1s）",
               acc.size() > static_cast<std::size_t>(fs / 10), "");

    if (acc.size() < static_cast<std::size_t>(fs / 10)) {
        return;
    }
    const double magOn = GoertzelMag(acc, f0, static_cast<double>(fs));
    const double magOff = GoertzelMag(acc, offF, static_cast<double>(fs));
    Note("  Goertzel 幅值 @1kHz", magOn, "");
    Note("  Goertzel 幅值 @3kHz（对照）", magOff, "");
    ReportTrue("R4 捕获含 1kHz 成分（幅值显著 > 0）", magOn > 1e-3, "");
    const double ratio = magOn / std::max(magOff, 1e-12);
    Note("  1kHz / 3kHz 幅值比", ratio, "");
    Report("R4 1kHz 幅值 ≫ 3kHz 对照（比值 > 5，判据有区分力）", ratio, 5.0, 1e9, "");
}

/* ── R5：F2 回归哨兵 —— 启动失败不得被吞成 0（行为级，环境无关） ────── */
void TestR5() {
    // 未 Open() 的引擎：capture/render Start() 必然失败
    // => Run() 必须返回非 0（4 = AudioInitFailed），而不是静默 0。
    router::RouterEngine engine;
    std::atomic<bool> stop{false};
    const int rc = engine.Run(stop);
    ReportTrue("R5 未 Open 的引擎 Run() 返回非 0（启动失败不吞成 0）", rc != 0, "");
    Report("R5 Run() 退出码 == 4（AudioInitFailed）", static_cast<double>(rc), 4.0, 4.0, "");
}

/* ── R6：端点热插拔看护策略（TASK-012） ──────────────────────────────────
 * Part A（环境无关）：DecideWarden 的真值表 —— 端点丢失且用户未停、未到点 ⇒ Reconnect；
 *                     其余（stop / 到点 / 非丢失）⇒ Exit。
 * Part B（需能 Open）：Run() 必须**真的调用**同一函数（用调用计数坐实，防「平行实现」）。
 * 注意：Part A 失败时**不得**置 SKIP（否则 main 的 SKIP 分支会盖掉 FAIL，把真回归藏掉）。 */
void TestR6() {
    using router::WardenAction;
    using router::DecideWarden;

    const bool a1 = DecideWarden(false, true, false) == WardenAction::Reconnect;
    const bool a2 = DecideWarden(true, true, false) == WardenAction::Exit;
    const bool a3 = DecideWarden(true, false, false) == WardenAction::Exit;
    const bool a4 = DecideWarden(false, false, true) == WardenAction::Exit;
    const bool a5 = DecideWarden(false, true, true) == WardenAction::Exit;   /* 到点优先于丢失 */
    ReportTrue("R6A 端点丢失 + 未 stop + 未到点 ⇒ Reconnect", a1, "");
    ReportTrue("R6A stop 置位（且丢失）⇒ Exit", a2, "");
    ReportTrue("R6A stop 置位（无丢失）⇒ Exit", a3, "");
    ReportTrue("R6A 到点（maxSeconds）⇒ Exit", a4, "");
    ReportTrue("R6A 到点优先于丢失 ⇒ Exit", a5, "");
    ReportTrue("R6A 策略非恒等（存在两种不同决策）",
               DecideWarden(false, true, false) != DecideWarden(true, true, false), "");

    if (g_fail != 0) {
        std::printf("  [--] R6 Part A 已失败 ⇒ 不做 SKIP（避免掩盖 FAIL）\n");
        return;
    }

    /* Part B：Run() 走同一策略函数 */
    const unsigned long long before = router::WardenDecisionCalls();
    router::RouterConfig cfg;
    cfg.captureName = L"CABLE Output";
    cfg.bufferMs = 10.0;

    router::EndpointInfo cap;
    if (!router::ResolveVirtualCapture(cfg.captureName, &cap, nullptr)) {
        PrintSkipBanner("R6B");
        g_skip = kSkipExitCode;
        return;
    }
    const router::EndpointInfo rend = router::GetDefaultEndpoint(eRender);
    if (rend.id.empty()) {
        PrintSkipBanner("R6B");
        g_skip = kSkipExitCode;
        return;
    }

    router::RouterEngine engine;
    const router::RouterExitCode oc = engine.Open(cfg, cap.id, rend.id);
    if (oc != router::RouterExitCode::Ok) {
        std::printf("  [--] R6B Open 未成功（rc=%d）⇒ 环境级 SKIP\n", static_cast<int>(oc));
        PrintSkipBanner("R6B");
        g_skip = kSkipExitCode;
        return;
    }
    ReportTrue("R6B Open 后 Stats().endpointLosses == 0", engine.Stats().endpointLosses == 0u, "");
    ReportTrue("R6B Open 后 Stats().reconnects == 0", engine.Stats().reconnects == 0u, "");

    std::atomic<bool> stop{true};   /* 立刻要求停止：泵循环立即退出 ⇒ 必然走一次看护决策 */
    const int rc = engine.Run(stop);
    Report("R6B Run()（Open 成功 + 立即 stop）退出码 == 0", static_cast<double>(rc), 0.0, 0.0, "");
    ReportTrue("R6B Run() 真的调用了 DecideWarden（调用计数增加）",
               router::WardenDecisionCalls() > before, "");
    Note("  DecideWarden 调用前/后", static_cast<double>(before), "");
    Note("  DecideWarden 调用次数增量",
         static_cast<double>(router::WardenDecisionCalls() - before), "");
    engine.Close();
}

}  /* namespace */

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: router_tests <R1|R2|R3|R4|R5|R6>\n");
        return 2;
    }
    const std::string t = argv[1];
    std::printf("== %s ==\n", t.c_str());

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::printf("CoInitializeEx failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    if (t == "R1") {
        TestR1();
    } else if (t == "R2") {
        TestR2();
    } else if (t == "R3") {
        TestR3();
    } else if (t == "R4") {
        TestR4();
    } else if (t == "R5") {
        TestR5();
    } else if (t == "R6") {
        TestR6();
    } else {
        std::printf("unknown test: %s\n", t.c_str());
        CoUninitialize();
        return 2;
    }

    CoUninitialize();
    if (g_skip != 0) {
        std::printf("%s SKIP（环境级缺音频端点）\n", t.c_str());
        return g_skip;
    }
    if (g_fail != 0) {
        std::printf("%s FAIL (%d 项未通过)\n", t.c_str(), g_fail);
        return 1;
    }
    std::printf("%s PASS\n", t.c_str());
    return 0;
}
