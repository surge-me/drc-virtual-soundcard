#include "router/dsp_bridge.h"

#include <new>

namespace router {

RouterParams ParamsFromIpc(const DrcParams& p) noexcept {
    RouterParams r{};
    r.ceilingDb   = p.ceilingDb;
    r.floorDb     = p.floorDb;
    r.maxBoostDb  = p.maxBoostDb;
    r.lookaheadMs = p.lookaheadMs;
    r.releaseMs   = p.releaseMs;
    for (int i = 0; i < 10; ++i) {
        r.eqGainDb[i] = p.eqGainDb[i];
    }
    r.bypass = (p.flags & DRC_FLAG_BYPASS) != 0u;
    return r;
}

bool SameParams(const RouterParams& a, const RouterParams& b) noexcept {
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

DspBridge::~DspBridge() = default;

bool DspBridge::Configure(int sampleRate, int channels) noexcept {
    try {
        if (m_proc == nullptr) {
            m_proc = std::unique_ptr<drc::DspProcessor>(
                new drc::DspProcessor(sampleRate, channels));
        } else {
            m_proc->Configure(sampleRate, channels);
            m_proc->Reset();
        }
        m_available = true;
    } catch (...) {
        m_proc.reset();
        m_available = false;
    }
    m_hasApplied = false;
    return m_available;
}

void DspBridge::Reset() noexcept {
    m_hasApplied = false;
    if (m_proc != nullptr) {
        m_proc->Reset();
    }
}

bool DspBridge::Process(float* interleaved, std::size_t frames, int sampleRate, int channels,
                        const RouterParams& p) noexcept {
    if (!m_available || m_proc == nullptr || interleaved == nullptr || frames == 0u) {
        return false;   /* 不可用 => 直通 */
    }
    if (p.bypass) {
        return false;   /* bypass => 逐样本 bit-exact 直通（SPEC §1.4） */
    }
    try {
        if (!m_hasApplied || !SameParams(m_applied, p)) {
            drc::DspParams dp;
            dp.ceilingDb   = p.ceilingDb;
            dp.floorDb     = p.floorDb;
            dp.maxBoostDb  = p.maxBoostDb;
            dp.lookaheadMs = p.lookaheadMs;
            dp.releaseMs   = p.releaseMs;
            for (int i = 0; i < drc::kEqBandCount; ++i) {
                dp.eqGainDb[i] = p.eqGainDb[i];
            }
            dp.bypass = p.bypass;
            m_proc->UpdateParams(dp);
            m_applied = p;
            m_hasApplied = true;
        }
        m_proc->Process(interleaved, frames, channels, sampleRate);
    } catch (...) {
        return false;   /* DSP 异常 => 直通（调用方恢复原样，永不静音） */
    }
    return true;
}

}  /* namespace router */
