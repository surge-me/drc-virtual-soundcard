/*
 * dsp_bridge.h — router 与 dsp-core 的接线（TASK-006 audio-router）
 *
 * 依赖方向：router --include--> apo/dsp（只 include 公共头 drc/dsp.h，不改其内容）。
 * 职责：把 IPC 参数快照（DrcParams）映射为 drc::DspParams 并驱动 drc::DspProcessor。
 * 线程纪律：Configure()/Reset() 在启动（非音频线程）调用；Process() 在音频线程调用，
 * 内部**不分配**（DspProcessor 在 Configure 期构造）。
 * 降级：bypass=true / DSP 不可用 / Process 抛异常 => 返回 false（调用方原样转发，永不静音）。
 */
#ifndef ROUTER_DSP_BRIDGE_H
#define ROUTER_DSP_BRIDGE_H

#include <cstddef>
#include <memory>

#include <drc/dsp.h>   /* apo/dsp 公共头（只 include，不改其内容） */

#include "drc_ipc.h"   /* DrcParams */

namespace router {

/* 运行期参数（词表字段照 AGENTS.md §2） */
struct RouterParams {
    float ceilingDb = -6.0f;
    float floorDb = -50.0f;
    float maxBoostDb = 12.0f;
    float lookaheadMs = 2.5f;
    float releaseMs = 120.0f;
    float eqGainDb[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    bool  bypass = false;
};

RouterParams ParamsFromIpc(const DrcParams& p) noexcept;
bool SameParams(const RouterParams& a, const RouterParams& b) noexcept;

class DspBridge {
public:
    DspBridge() noexcept = default;
    ~DspBridge();

    DspBridge(const DspBridge&) = delete;
    DspBridge& operator=(const DspBridge&) = delete;

    /* 非音频线程：构造算法状态（堆分配只发生在这里）。失败 => Available()==false。 */
    bool Configure(int sampleRate, int channels) noexcept;
    /* 非音频线程：清空算法状态，保留实例。 */
    void Reset() noexcept;

    /* 音频线程：就地处理 interleaved float32。
     * 返回 true = 已按参数处理；false = 直通（bypass / 不可用 / 异常），缓冲未定义时
     * 由调用方负责恢复原样（本函数保证不写入超出 frames*channels 的范围）。 */
    bool Process(float* interleaved, std::size_t frames, int sampleRate, int channels,
                 const RouterParams& p) noexcept;

    bool Available() const noexcept { return m_available; }

private:
    bool m_available = false;
    bool m_hasApplied = false;
    RouterParams m_applied{};
    std::unique_ptr<drc::DspProcessor> m_proc;
};

}  /* namespace router */

#endif /* ROUTER_DSP_BRIDGE_H */
