#include "drc_dsp_adapter.h"

#ifdef DRC_HAVE_DSP_CORE
#include <new>
#endif

namespace drc {

namespace {

#ifdef DRC_HAVE_DSP_CORE
DspParams ToDspParams(const DspRuntimeParams& r) noexcept {
    DspParams p;
    p.ceilingDb   = r.ceilingDb;
    p.floorDb     = r.floorDb;
    p.maxBoostDb  = r.maxBoostDb;
    p.lookaheadMs = r.lookaheadMs;
    p.releaseMs   = r.releaseMs;
    for (int i = 0; i < kEqBandCount; ++i) {
        p.eqGainDb[i] = r.eqGainDb[i];
    }
    p.bypass = r.bypass;
    return p;
}

bool SameParams(const DspRuntimeParams& a, const DspRuntimeParams& b) noexcept {
    if (a.ceilingDb != b.ceilingDb || a.floorDb != b.floorDb ||
        a.maxBoostDb != b.maxBoostDb || a.lookaheadMs != b.lookaheadMs ||
        a.releaseMs != b.releaseMs || a.bypass != b.bypass) {
        return false;
    }
    for (int i = 0; i < 10; ++i) {
        if (a.eqGainDb[i] != b.eqGainDb[i]) {
            return false;
        }
    }
    return true;
}
#endif

}  /* namespace */

DspChain::~DspChain() = default;

bool DspChain::Init(int channels, int sampleRate) noexcept {
    m_channels = channels;
    m_sampleRate = sampleRate;
    m_hasParams = false;

#ifdef DRC_HAVE_DSP_CORE
    try {
        if (m_proc == nullptr) {
            /* 堆分配发生在非音频线程（LockForProcess）——音频线程内零分配 */
            m_proc = std::unique_ptr<DspProcessor>(new DspProcessor(sampleRate, channels));
        } else {
            m_proc->Configure(sampleRate, channels);
            m_proc->Reset();
        }
        m_available = true;
    } catch (...) {
        m_proc.reset();
        m_available = false;   /* 分配失败 ⇒ 直通降级（显式，可观测于 devlog） */
    }
#else
    m_available = false;       /* dsp-core 未落地 ⇒ 壳直通（TASK-002 既定降级） */
#endif
    return true;
}

void DspChain::Reset() noexcept {
    m_hasParams = false;
#ifdef DRC_HAVE_DSP_CORE
    if (m_proc != nullptr) {
        m_proc->Reset();
    }
#endif
}

void DspChain::Process(float* interleaved, std::size_t frames, int channels, int sampleRate,
                       const DspRuntimeParams& params) noexcept {
#ifdef DRC_HAVE_DSP_CORE
    if (m_available && m_proc != nullptr && interleaved != nullptr && frames > 0u) {
        /* 参数变化才下发：避免每块重发同目标打断 5ms 收敛（SPEC §1.4） */
        if (!m_hasParams || !SameParams(m_applied, params)) {
            m_proc->UpdateParams(ToDspParams(params));
            m_applied = params;
            m_hasParams = true;
        }
        m_proc->Process(interleaved, frames, channels, sampleRate);
    }
#else
    /* 直通：缓冲已由调用方拷贝就位，此处不改动样本（bypass bit-exact 语义） */
    (void)interleaved;
    (void)frames;
    (void)channels;
    (void)sampleRate;
    (void)params;
#endif
}

}  /* namespace drc */
