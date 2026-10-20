#include "router/router_engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace router {

/* ── 端点热插拔看护策略（纯函数；Run() 走同一实现，R6 用调用计数坐实） ────── */
static std::atomic<unsigned long long> g_wardenCalls{0ull};

WardenAction DecideWarden(bool stopRequested, bool deviceLost, bool timeLimitReached) noexcept {
    g_wardenCalls.fetch_add(1ull, std::memory_order_relaxed);
#ifdef DRC_WARDEN_FORCE_EXIT
    /* 变异对照（TASK-012，默认关）：恢复"端点丢失即退出"的修复前行为，
     * 用于证明热插拔实验有区分力（变异体必死、修复版必活）。 */
    (void)stopRequested; (void)deviceLost; (void)timeLimitReached;
    return WardenAction::Exit;
#else
    if (stopRequested || timeLimitReached) {
        return WardenAction::Exit;
    }
    return deviceLost ? WardenAction::Reconnect : WardenAction::Exit;
#endif
}

unsigned long long WardenDecisionCalls() noexcept {
    return g_wardenCalls.load(std::memory_order_relaxed);
}

bool ResolveVirtualCapture(const std::wstring& needle, EndpointInfo* out,
                           std::vector<EndpointInfo>* allCapture) {
    std::vector<EndpointInfo> caps = EnumActiveEndpoints(eCapture);
    if (allCapture != nullptr) {
        *allCapture = caps;
    }
    const EndpointInfo* hit = FindEndpointByName(caps, needle);
    if (hit == nullptr) {
        return false;
    }
    if (out != nullptr) {
        *out = *hit;
    }
    return true;
}

RouterEngine::~RouterEngine() { Close(); }

void RouterEngine::Close() noexcept {
    StopSessions();
    m_render.ReleaseAll();
    m_capture.ReleaseAll();
    m_shm.Close();
    m_ring.clear();
    m_work.clear();
    m_src.clear();
    m_sessionOpen = false;
    /* 收尾复位（调用方应在 Close 之前取走需要的统计）。 */
    m_stats = RouterStats{};
}

RouterExitCode RouterEngine::Open(const RouterConfig& cfg,
                                  const std::wstring& captureDeviceId,
                                  const std::wstring& renderDeviceId) {
    /* 注意：**不**在此复位 m_stats —— 端点热插拔重连会重复 Open，累计量
     * （framesProcessed / 丢失次数 / 重连次数）必须跨重连连续；复位只发生在 Close()。 */

    if (!m_capture.Open(captureDeviceId, cfg.bufferMs, /*loopback=*/false)) {
        return RouterExitCode::AudioInitFailed;
    }
    m_stats.captureFormat = m_capture.NegotiatedFormat();
    m_stats.captureRate = m_capture.SampleRate();
    m_stats.captureChannels = m_capture.Channels();

    if (!m_render.Open(renderDeviceId, cfg.bufferMs, m_capture.SampleRate(),
                       m_capture.Channels())) {
        return RouterExitCode::AudioInitFailed;
    }
    m_stats.renderFormat = m_render.NegotiatedFormat();
    m_stats.renderRate = m_render.SampleRate();
    m_stats.renderChannels = m_render.Channels();

    /* 采样率/通道必须一致（dsp 热路径为固定 interleaved float32；不重采样不混音）。 */
    if (m_capture.SampleRate() != m_render.SampleRate() ||
        m_capture.Channels() != m_render.Channels()) {
        return RouterExitCode::FormatMismatch;
    }
    if (m_capture.Format() == SampleFormat::Unknown ||
        m_render.Format() == SampleFormat::Unknown) {
        return RouterExitCode::FormatMismatch;
    }

    m_channels = m_capture.Channels();
    m_sampleRate = m_capture.SampleRate();

    const UINT32 capBuf = m_capture.BufferFrames();
    const UINT32 renBuf = m_render.BufferFrames();
    m_stats.captureBufferMs = 1000.0 * static_cast<double>(capBuf) /
                              static_cast<double>(m_sampleRate);
    m_stats.renderBufferMs = 1000.0 * static_cast<double>(renBuf) /
                             static_cast<double>(m_sampleRate);

    /* 预分配（音频线程零分配）：环形缓冲 ≥ 渲染块 + 余量。 */
    m_ringFrames = std::max<std::size_t>(
        static_cast<std::size_t>(renBuf) * 8u,
        static_cast<std::size_t>(m_sampleRate) / 2u);
    m_ring.assign(m_ringFrames * static_cast<std::size_t>(m_channels), 0.0f);
    m_work.assign(static_cast<std::size_t>(renBuf) * static_cast<std::size_t>(m_channels),
                  0.0f);
    m_src.assign(m_work.size(), 0.0f);
    m_ringRead = 0u;
    m_ringWrite = 0u;

    m_dsp.Configure(m_sampleRate, m_channels);
    m_stats.dspAvailable = m_dsp.Available();

    m_maxSeconds = cfg.maxSeconds;
    m_shm.OpenOrCreate();
    m_stats.ipcOk = m_shm.Ok();
    DrcParams p{};
    m_shm.ReadParams(&p);      /* 未发布 => 默认参数（SPEC §3） */
    m_lastParams = ParamsFromIpc(p);
    m_hasParams = true;

    m_cfg = cfg;                 /* 重连时复用（含端点名字匹配串） */
    m_sessionOpen = true;
    return RouterExitCode::Ok;
}

bool RouterEngine::PumpCapture() noexcept {
    const UINT32 ch = static_cast<UINT32>(m_channels);
    for (;;) {
        UINT32 packet = 0u;
        if (!m_capture.GetNextPacketSize(&packet)) {
            return false;   /* 端点失效/错误（上层据此优雅退出） */
        }
        if (packet == 0u) {
            break;
        }
        BYTE* data = nullptr;
        UINT32 frames = 0u;
        DWORD flags = 0u;
        if (!m_capture.GetPacket(&data, &frames, &flags)) {
            return false;
        }
        ++m_stats.capturePackets;
        if (frames > 0u) {
            const std::size_t avail = m_ringWrite - m_ringRead;
            if (avail + frames > m_ringFrames) {
                ++m_stats.overflowDrops;   /* 环形满：丢本包（不阻塞音频线程） */
            } else {
                const std::size_t w = m_ringWrite % m_ringFrames;
                const std::size_t tail = m_ringFrames - w;
                const std::size_t samples = static_cast<std::size_t>(frames) * ch;
                float* dst = m_ring.data() + w * ch;
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0u || data == nullptr) {
                    std::memset(dst, 0, samples * sizeof(float));
                } else if (m_capture.Format() == SampleFormat::Float32) {
                    if (tail >= frames) {
                        std::memcpy(dst, data, samples * sizeof(float));
                    } else {
                        const std::size_t head = frames - tail;
                        std::memcpy(dst, data, tail * ch * sizeof(float));
                        std::memcpy(m_ring.data(), data + tail * ch * sizeof(float),
                                    head * ch * sizeof(float));
                    }
                } else {
                    if (tail >= frames) {
                        PcmToFloat(data, m_capture.Format(), dst, samples);
                    } else {
                        /* 跨环尾：分两段转换（本实现按帧对齐，tail 是帧数） */
                        const std::size_t head = frames - tail;
                        PcmToFloat(data, m_capture.Format(), dst, tail * ch);
                        PcmToFloat(data + tail * ch * (m_capture.Format() == SampleFormat::Int16
                                                           ? 2u : 4u),
                                   m_capture.Format(), m_ring.data(), head * ch);
                    }
                }
                m_ringWrite += frames;
            }
        }
        m_capture.ReleasePacket(frames);
    }
    return true;
}

bool RouterEngine::PumpRender() noexcept {
    UINT32 padding = 0u;
    if (!m_render.GetPadding(&padding)) {
        return false;
    }
    const UINT32 renBuf = m_render.BufferFrames();
    if (padding >= renBuf) {
        return true;
    }
    const UINT32 freeFrames = renBuf - padding;
    if (freeFrames == 0u) {
        return true;
    }

    BYTE* out = nullptr;
    if (!m_render.GetBuffer(freeFrames, &out)) {
        return false;
    }

    const std::size_t ch = static_cast<std::size_t>(m_channels);
    const std::size_t avail = m_ringWrite - m_ringRead;
    const std::size_t n = std::min<std::size_t>(freeFrames, avail);

    const float* src = nullptr;
    const float* outF = nullptr;
    if (n > 0u) {
        const std::size_t r = m_ringRead % m_ringFrames;
        const std::size_t samples = n * ch;
        float* work = m_work.data();
        if (r + n <= m_ringFrames) {
            std::memcpy(work, m_ring.data() + r * ch, samples * sizeof(float));
        } else {
            const std::size_t tail = m_ringFrames - r;
            std::memcpy(work, m_ring.data() + r * ch, tail * ch * sizeof(float));
            std::memcpy(work + tail * ch, m_ring.data(),
                        (n - tail) * ch * sizeof(float));
        }
        m_ringRead += n;

        std::memcpy(m_src.data(), work, samples * sizeof(float));   /* 原始备份 */

        DrcParams p{};
        m_shm.ReadParams(&p);
        const RouterParams rp = ParamsFromIpc(p);
        m_lastParams = rp;
        m_hasParams = true;

        const bool processed =
            m_dsp.Process(work, n, m_sampleRate, m_channels, rp);
        if (!processed) {
            ++m_stats.dspPassthroughBlocks;
            std::memcpy(work, m_src.data(), samples * sizeof(float));  /* 原样转发 */
        }
        src = m_src.data();
        outF = work;

        /* 工作缓冲 → 渲染缓冲（按需格式转换） */
        if (m_render.Format() == SampleFormat::Float32) {
            std::memcpy(out, work, samples * sizeof(float));
        } else {
            FloatToPcm(work, m_render.Format(), out, samples);
        }
        m_stats.framesProcessed += n;
    }

    /* 欠载：剩余帧补静音（保持渲染缓冲填满，避免爆音） */
    if (n < freeFrames) {
        const std::size_t bytesPerFrame =
            ch * ((m_render.Format() == SampleFormat::Float32) ? 4u : 2u);
        std::memset(out + n * bytesPerFrame, 0, (freeFrames - n) * bytesPerFrame);
        if (n == 0u) {
            ++m_stats.underruns;
        }
    }

    m_render.ReleaseBuffer(freeFrames, 0u);

    if (n > 0u) {
        UpdateMeter(src, outF, n);
    }
    return true;
}

void RouterEngine::UpdateMeter(const float* in, const float* out, std::size_t frames) noexcept {
    if (!m_shm.Ok()) {
        return;   /* 降级：不写 meter（SPEC §4 / ARCH §4） */
    }
    const std::size_t n = frames * static_cast<std::size_t>(m_channels);
    float inPeak = 0.0f;
    float outPeak = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const float a = (in != nullptr) ? std::fabs(in[i]) : 0.0f;
        if (a > inPeak) inPeak = a;
        const float b = (out != nullptr) ? std::fabs(out[i]) : 0.0f;
        if (b > outPeak) outPeak = b;
    }
    const float inDb = ShmChannel::LinearToDb(inPeak);
    const float outDb = ShmChannel::LinearToDb(outPeak);
    float reduction = inDb - outDb;
    if (!(reduction > 0.0f)) {
        reduction = 0.0f;
    }

    DrcMeter m{};
    m.magic = DRC_MAGIC;
    m.version = DRC_METER_VERSION;
    m.activeSampleRate = static_cast<uint32_t>(m_sampleRate);
    m.activeChannels = static_cast<uint32_t>(m_channels);
    m.gainReductionDb_x100 = static_cast<uint32_t>(reduction * 100.0f + 0.5f);
    m.inPeakDb_x100 = ShmChannel::DbToX100(inDb);
    m.outPeakDb_x100 = ShmChannel::DbToX100(outDb);
    m.latencyMs_x10 = static_cast<uint32_t>(m_lastParams.lookaheadMs * 10.0f + 0.5f);
    m.framesProcessed = m_stats.framesProcessed;
    m_shm.WriteMeter(m);
}

bool RouterEngine::StartSessions() noexcept {
    if (!m_capture.Start()) {
        return false;
    }
    if (!m_render.Start()) {
        m_capture.Stop();
        return false;
    }
    return true;
}

void RouterEngine::StopSessions() noexcept {
    m_render.Stop();
    m_capture.Stop();
}

bool RouterEngine::FaultActive() noexcept {
    if (m_cfg.simulateLossAfterMs == 0u || m_faultConsumed) {
        return false;
    }
    const ULONGLONG elapsed = GetTickCount64() - m_runStart;
    const bool want = (elapsed >= static_cast<ULONGLONG>(m_cfg.simulateLossAfterMs)) &&
                      (elapsed < static_cast<ULONGLONG>(m_cfg.simulateLossAfterMs) + 3000ull);
    if (want != m_faultActive) {
        m_faultActive = want;
        std::fprintf(stderr, want ? "[fault-inject] simulated endpoint loss START (test hook)\n"
                                  : "[fault-inject] simulated endpoint loss END (test hook)\n");
        std::fflush(stderr);
    }
    return want;
}

bool RouterEngine::PumpUntilStop(const std::atomic<bool>& stop, ULONGLONG t0,
                                 bool* timeUp) noexcept {
    *timeUp = false;
    HANDLE waits[2] = {m_capture.Event(), m_render.Event()};
    uint32_t failures = 0u;
    while (!stop.load(std::memory_order_relaxed)) {
        const bool fault = FaultActive();
        const bool okCapture = fault ? false : PumpCapture();
        const bool okRender = fault ? false : PumpRender();
        if (!okCapture || !okRender) {
            /* 端点消失/失效：连续失败 ~1s（20ms×50）⇒ 上报 deviceLost（由看护层决定重连） */
            if (++failures > 50u) {
                return true;
            }
        } else {
            failures = 0u;
        }
        if (m_maxSeconds > 0 &&
            (GetTickCount64() - t0) >= static_cast<ULONGLONG>(m_maxSeconds) * 1000ull) {
            *timeUp = true;
            return false;
        }
        WaitForMultipleObjects(2u, waits, FALSE, 20u);
    }
    return false;
}

bool RouterEngine::ResolveEndpointIds(std::wstring* captureId, std::wstring* renderId) const {
    std::vector<EndpointInfo> caps;
    EndpointInfo cap;
    if (!ResolveVirtualCapture(m_cfg.captureName, &cap, &caps)) {
        return false;   /* 捕获端点（虚拟线）不在 ⇒ 还不算就绪 */
    }
    *captureId = cap.id;

    if (m_cfg.renderName.empty()) {
        const EndpointInfo def = GetDefaultEndpoint(eRender);
        if (def.id.empty()) {
            return false;
        }
        *renderId = def.id;
    } else {
        const std::vector<EndpointInfo> rends = EnumActiveEndpoints(eRender);
        const EndpointInfo* hit = FindEndpointByName(rends, m_cfg.renderName);
        if (hit == nullptr) {
            return false;   /* 渲染端点（耳机等）还没回来 */
        }
        *renderId = hit->id;
    }
    return true;
}

bool RouterEngine::WaitEndpointsReady(const std::atomic<bool>& stop) {
    DWORD delayMs = 500u;
    for (;;) {
        if (stop.load(std::memory_order_relaxed)) {
            return false;
        }
        std::wstring capId, renId;
        if (ResolveEndpointIds(&capId, &renId)) {
            return true;
        }
        for (DWORD slept = 0u; slept < delayMs; slept += 100u) {
            if (stop.load(std::memory_order_relaxed)) {
                return false;
            }
            Sleep(100u);
        }
        if (delayMs < 2000u) {
            delayMs *= 2u;   /* 0.5s → 1s → 2s 封顶（不放弃，直到端点回来或用户停止） */
        }
    }
}

bool RouterEngine::ReopenSession() {
    std::wstring capId, renId;
    if (!ResolveEndpointIds(&capId, &renId)) {
        return false;
    }
    if (Open(m_cfg, capId, renId) != RouterExitCode::Ok) {
        return false;   /* 格式不匹配/初始化失败：继续等下一次（端点可能在枚举过程中又变了） */
    }
    return true;
}

int RouterEngine::Run(const std::atomic<bool>& stop) {
    if (!m_sessionOpen) {
        /* 未 Open：capture/render Start() 必然失败 ⇒ 不吞成 0（R5 哨兵）。 */
        return static_cast<int>(RouterExitCode::AudioInitFailed);
    }
    if (!StartSessions()) {
        return static_cast<int>(RouterExitCode::AudioInitFailed);   /* 启动失败：不吞成 0 */
    }

    const ULONGLONG t0 = GetTickCount64();
    m_runStart = t0;
    m_faultActive = false;
    m_faultConsumed = false;
    for (;;) {
        bool timeUp = false;
        const bool lost = PumpUntilStop(stop, t0, &timeUp);
        StopSessions();

        if (lost) {
            m_stats.deviceLost = true;
            ++m_stats.endpointLosses;
            /* 行首 ASCII 标记：便于外部脚本/日志做机器解析（中文说明跟在后面，人可读）。
             * 先记账再决策：即使本次策略判为退出，也必须留下"发生过端点丢失"的证据。 */
            std::fprintf(stderr, "[endpoint-lost] 端点丢失（已丢失 %u 次 / 已重连 %u 次）\n",
                         m_stats.endpointLosses, m_stats.reconnects);
            std::fflush(stderr);
        }
        if (DecideWarden(stop.load(std::memory_order_relaxed), lost, timeUp) == WardenAction::Exit) {
            break;
        }

        /* ── 端点热插拔看护：不退出，等端点回来再按**名字**重解析、重开会话 ──
         * （耳机断开/重连 ⇒ 旧 IAudioClient 不可复用；重连后台端点 ID 可能变。） */
        std::fprintf(stderr, "[waiting-device] 等待端点回来…\n");
        std::fflush(stderr);

        bool reconnected = false;
        while (!reconnected) {
            if (!WaitEndpointsReady(stop)) {
                break;      /* 只可能因 stop 置位 */
            }
            reconnected = ReopenSession() && StartSessions();
            if (!reconnected) {
                for (DWORD slept = 0u; slept < 1000u; slept += 100u) {
                    if (stop.load(std::memory_order_relaxed)) {
                        break;
                    }
                    Sleep(100u);
                }
                if (stop.load(std::memory_order_relaxed)) {
                    break;
                }
            }
        }
        if (!reconnected) {
            break;          /* stop：正常收尾 */
        }
        ++m_stats.reconnects;
        m_faultConsumed = true;      /* 故障注入是单发的：重连一次后不再触发 */
        m_faultActive = false;
        std::fprintf(stderr, "[reconnected] 已重连（第 %u 次）：capture[%s] render[%s]\n",
                     m_stats.reconnects, m_stats.captureFormat.c_str(),
                     m_stats.renderFormat.c_str());
        std::fflush(stderr);
        /* 回到泵循环（重新取事件句柄：Open 已重建句柄）。 */
    }

    /* 优雅退出：framesProcessed 停更（GUI 心跳判定「下线」，SPEC §4 口径不变）。
     * 端点丢失已由看护层处理（重连），走到这里只可能是 stop / 到点 ⇒ 退出码 0。 */
    return static_cast<int>(RouterExitCode::Ok);
}

}  /* namespace router */
