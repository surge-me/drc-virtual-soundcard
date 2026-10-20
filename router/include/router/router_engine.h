/*
 * router_engine.h — 捕获 -> dsp-core -> 渲染 主环（TASK-006 audio-router）
 *
 * 单线程泵：capture 事件驱动取包 -> 预分配环形缓冲 -> 每块头读 IPC 参数（seqlock）
 *           -> dsp-core 就地处理 -> 渲染缓冲 -> 每块尾写 IPC meter。
 * 音频线程纪律：无堆分配 / 无锁 / 无文件 IO（环形缓冲与工作缓冲在 Start 前预分配）。
 * 失败降级：DSP 不可用 / 抛异常 / bypass => 原样转发（永不静音）。
 */
#ifndef ROUTER_ENGINE_H
#define ROUTER_ENGINE_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "router/dsp_bridge.h"
#include "router/shm_channel.h"
#include "router/wasapi.h"

namespace router {

struct RouterConfig {
    std::wstring captureDeviceId;   /* 空 = 默认捕获端点 */
    std::wstring renderDeviceId;    /* 空 = 默认渲染端点 */
    double bufferMs = 10.0;         /* 引擎缓冲时长（附加延迟预算） */
    int maxSeconds = 0;             /* >0 = 到点自动停（测试/自检用） */

    /* 端点**名字匹配串**：仅用于「端点热插拔后重连」时按名重新解析端点 ID。
     * 重连后台可能重建端点（ID 变、名不变）⇒ 必须按名重解析，不能缓存 ID。
     * captureName 空 = 沿用默认捕获端点；renderName 空 = 每次重连取当时的默认渲染端点。 */
    std::wstring captureName;
    std::wstring renderName;

    /* ── 测试钩子（故障注入；生产恒为 0 = 关闭）─────────────────────────────
     * >0 = Run() 启动后第 N ms 起，强制两个 Pump 返回失败（等价于端点失效信号），
     * 持续 kSimulatedLossWindowMs（3s），**一次**（成功重连后即消费掉）。
     * 为什么需要它：本机实测无法在不重启、不物理拔插用户耳机的前提下让真实端点消失
     * （见 docs/devlogs/2026-10-15-endpoint-loss-recovery.md「为何用故障注入」）。 */
    unsigned simulateLossAfterMs = 0;
};

struct RouterStats {
    bool     ipcOk = false;
    bool     dspAvailable = false;
    int      captureRate = 0;
    int      captureChannels = 0;
    int      renderRate = 0;
    int      renderChannels = 0;
    std::string captureFormat;
    std::string renderFormat;
    uint64_t capturePackets = 0;
    uint64_t framesProcessed = 0;
    uint64_t underruns = 0;
    uint64_t overflowDrops = 0;
    uint64_t dspPassthroughBlocks = 0;
    double   captureBufferMs = 0.0;
    double   renderBufferMs = 0.0;
    bool     deviceLost = false;    /* 本次运行中**发生过**端点消失（信息性；进程不再因此退出） */
    uint32_t endpointLosses = 0;    /* 端点消失次数（累加；重连成功不归零） */
    uint32_t reconnects = 0;        /* 端点热插拔后成功重连次数 */
};

enum class RouterExitCode : int {
    Ok = 0,
    Usage = 2,
    NoVirtualDevice = 3,
    AudioInitFailed = 4,
    FormatMismatch = 5
};

/* 解析虚拟声卡捕获端点：优先名含 needle 者；找不到 => false（上层 exit 3）。 */
bool ResolveVirtualCapture(const std::wstring& needle, EndpointInfo* out,
                           std::vector<EndpointInfo>* allCapture);

/* ── 端点热插拔看护策略（TASK-012） ─────────────────────────────────────
 * 纯函数、无副作用（除调用计数），便于机械验证；Run() **必须**调用它（同一实现，
 * 不得另写平行分支 —— R6 用调用计数坐实这一点）。
 *   会话因端点失效而结束（deviceLost）且用户未要求停止、未到时间上限 ⇒ Reconnect
 *   其余情况（stop 置位 / 到点 / 非丢失结束）                      ⇒ Exit */
enum class WardenAction { Exit, Reconnect };
WardenAction DecideWarden(bool stopRequested, bool deviceLost, bool timeLimitReached) noexcept;

/* DecideWarden 被调用次数（诊断 + R6 用：证明 Run() 真的走同一策略函数）。 */
unsigned long long WardenDecisionCalls() noexcept;

class RouterEngine {
public:
    RouterEngine() noexcept = default;
    ~RouterEngine();

    RouterEngine(const RouterEngine&) = delete;
    RouterEngine& operator=(const RouterEngine&) = delete;

    /* 打开设备 + 建环；返回 RouterExitCode。可重复调用（端点热插拔重连时重用）。 */
    RouterExitCode Open(const RouterConfig& cfg, const std::wstring& captureDeviceId,
                        const std::wstring& renderDeviceId);
    /* 主泵 + 端点看护；stop 置位后返回。
     * 返回 = 退出码（直接用作进程退出码，见 RouterExitCode）：
     *   0 = 正常退出（stop 置位 / 到达 maxSeconds）
     *   4 = 启动失败（未 Open，或 capture/render Start() 失败）——**不得被吞成 0**
     * 3（未检测到虚拟声卡）由 Open()/上层判定，不走本函数。
     * **端点失效不再结束进程**：按 DecideWarden 决策进入「等端点回来 → 按名重解析 → 重连」。 */
    int Run(const std::atomic<bool>& stop);
    void Close() noexcept;

    const RouterStats& Stats() const noexcept { return m_stats; }

private:
    bool PumpCapture() noexcept;
    bool PumpRender() noexcept;
    void UpdateMeter(const float* in, const float* out, std::size_t frames) noexcept;

    /* 内层泵：返回 true = 端点失效（deviceLost）；*timeUp 置位 = 到 maxSeconds；stop 置位则两者皆 false。 */
    bool PumpUntilStop(const std::atomic<bool>& stop, ULONGLONG t0, bool* timeUp) noexcept;
    bool StartSessions() noexcept;      /* capture.Start + render.Start（失败回滚） */
    void StopSessions() noexcept;
    /* 按 cfg 里的**名字**重新解析端点 ID（热插拔后台可能重建端点 ⇒ ID 会变）。 */
    bool ResolveEndpointIds(std::wstring* captureId, std::wstring* renderId) const;
    /* 等两端点按名可解析（退避 0.5s→1s→2s 封顶）；stop 置位 ⇒ false。 */
    bool WaitEndpointsReady(const std::atomic<bool>& stop);
    /* 重新打开会话（重解析 + Open）；成功 ⇒ true（随后由 StartSessions 启动）。 */
    bool ReopenSession();
    /* 测试钩子：故障注入窗口内 ⇒ true（Run 的泵据此强制失败，模拟端点失效信号）。 */
    bool FaultActive() noexcept;

    WasapiCapture m_capture;
    WasapiRender  m_render;
    DspBridge     m_dsp;
    ShmChannel    m_shm;

    RouterConfig m_cfg{};           /* 重连时复用（含名字匹配串） */
    bool m_sessionOpen = false;     /* Open() 成功且未被 Close() 清掉 */
    ULONGLONG m_runStart = 0;       /* 测试钩子计时基准 */
    bool m_faultActive = false;
    bool m_faultConsumed = false;   /* 成功重连一次后即消费掉（单发） */

    std::vector<float> m_ring;      /* interleaved float32，容量 m_ringFrames*ch */
    std::vector<float> m_work;      /* 渲染块工作缓冲 */
    std::vector<float> m_src;       /* 原始捕获备份（meter 入 + 异常恢复） */
    std::size_t m_ringFrames = 0;
    std::size_t m_ringRead = 0;     /* 帧计数（单调） */
    std::size_t m_ringWrite = 0;
    int m_channels = 0;
    int m_sampleRate = 0;

    RouterParams m_lastParams{};
    bool m_hasParams = false;
    int  m_maxSeconds = 0;          /* >0 = 到点自动停（测试/自检用） */

    RouterStats m_stats{};
};

}  /* namespace router */

#endif /* ROUTER_ENGINE_H */
