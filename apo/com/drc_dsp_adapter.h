/*
 * drc_dsp_adapter.h — 壳与 dsp-core 的集成缝
 *
 * 依赖方向（ARCH §2）：apo/com --include--> apo/dsp（dsp-core 不知道 COM 存在）。
 * 开关由 CMake 决定：apo/dsp 有源文件且存在公共头 dsp/include/drc/dsp.h ⇒ 定义 DRC_HAVE_DSP_CORE，
 * 本适配器把 IPC 参数快照映射为 drc::DspParams 并驱动 drc::DspProcessor；
 * 否则（dsp-core 未落地）⇒ 直通（壳逻辑 IPC/降级/meter 仍完整可测）。
 *
 * 线程纪律：Init/Reset 只在非音频线程（LockForProcess/UnlockForProcess）调用；
 * Process 在音频线程调用，内部**不分配**（DspProcessor 在 Init 期构造，Process 只就地处理）。
 */
#ifndef DRC_DSP_ADAPTER_H
#define DRC_DSP_ADAPTER_H

#include <cstddef>

#include "drc_ipc.h"

#ifdef DRC_HAVE_DSP_CORE
#include <drc/dsp.h>
#include <memory>
#endif

namespace drc {

/* 传给 dsp 的运行期参数（词表字段照 AGENTS.md §2） */
struct DspRuntimeParams {
    float ceilingDb;
    float floorDb;
    float maxBoostDb;
    float lookaheadMs;
    float releaseMs;
    float eqGainDb[10];
    bool  bypass;
};

class DspChain {
public:
    DspChain() noexcept = default;
    ~DspChain();

    DspChain(const DspChain&) = delete;
    DspChain& operator=(const DspChain&) = delete;

    /* 非音频线程（LockForProcess）：构造/重配算法状态（堆分配只发生在这里） */
    bool Init(int channels, int sampleRate) noexcept;
    /* 非音频线程（UnlockForProcess）：清空算法状态，保留实例 */
    void Reset() noexcept;

    /* 音频线程：就地处理 interleaved float32 缓冲。
     * dsp-core 未落地 / 未就绪时 = 直通（原地不动，bit-exact）。 */
    void Process(float* interleaved, std::size_t frames, int channels, int sampleRate,
                 const DspRuntimeParams& params) noexcept;

    /* true = 真接了 dsp-core；false = 直通壳 */
    bool Available() const noexcept { return m_available; }

private:
    bool m_available = false;
    bool m_hasParams = false;
    int  m_channels = 0;
    int  m_sampleRate = 0;
    DspRuntimeParams m_applied{};   /* 已下发的目标参数（相同则不再 UpdateParams，避免收敛被打断） */

#ifdef DRC_HAVE_DSP_CORE
    std::unique_ptr<DspProcessor> m_proc;
#endif
};

}  /* namespace drc */

#endif /* DRC_DSP_ADAPTER_H */
