#include "router/shm_channel.h"

#include <sddl.h>   /* ConvertStringSecurityDescriptorToSecurityDescriptorW */

#include <cmath>
#include <cstring>

namespace router {

namespace {

void CopyNamedParams(const DrcParams* src, DrcParams* dst) noexcept {
    dst->magic       = src->magic;
    dst->version     = src->version;
    dst->flags       = src->flags;
    dst->ceilingDb   = src->ceilingDb;
    dst->floorDb     = src->floorDb;
    dst->maxBoostDb  = src->maxBoostDb;
    dst->lookaheadMs = src->lookaheadMs;
    dst->releaseMs   = src->releaseMs;
    for (int i = 0; i < 10; ++i) {
        dst->eqGainDb[i] = src->eqGainDb[i];
    }
    dst->seq = 0u;
}

}  /* namespace */

ShmChannel::~ShmChannel() { Close(); }

void ShmChannel::FillDefaults(DrcParams* p) noexcept {
    if (p == nullptr) {
        return;
    }
    std::memset(p, 0, sizeof(*p));
    p->magic       = DRC_MAGIC;
    p->version     = DRC_PARAM_VERSION;
    p->flags       = 0u;
    p->ceilingDb   = DRC_DEFAULT_CEILING_DB;
    p->floorDb     = DRC_DEFAULT_FLOOR_DB;
    p->maxBoostDb  = DRC_DEFAULT_MAX_BOOST_DB;
    p->lookaheadMs = DRC_DEFAULT_LOOKAHEAD_MS;
    p->releaseMs   = DRC_DEFAULT_RELEASE_MS;
    for (int i = 0; i < 10; ++i) {
        p->eqGainDb[i] = 0.0f;
    }
}

float ShmChannel::LinearToDb(float lin) noexcept {
    if (!(lin > 1e-6f)) {   /* 同时挡住 NaN */
        return -120.0f;
    }
    return 20.0f * std::log10(lin);
}

uint32_t ShmChannel::DbToX100(float db) noexcept {
    float mag = -db;               /* 低于满刻度的衰减量（SPEC §4.1） */
    if (!(mag > 0.0f)) {
        mag = 0.0f;
    }
    if (mag > 120.0f) {
        mag = 120.0f;
    }
    return static_cast<uint32_t>(mag * 100.0f + 0.5f);
}

void ShmChannel::OpenOrCreate() noexcept {
    if (m_pView != nullptr) {
        return;   /* 幂等 */
    }

    /* Global\ 对象默认 DACL 只授 SYSTEM/Administrators/登录会话 SID，不含 Everyone
     * ⇒ 非提权用户会话 GUI 的 OpenFileMapping(FILE_MAP_ALL_ACCESS) 会被拒。
     * 显式授 Everyone 映射全权（本机本地 IPC，无网络暴露面）。SDDL: D:(A;;GA;;;WD)。 */
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr) != FALSE && sd != nullptr) {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;
    }

    m_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, (sd != nullptr) ? &sa : nullptr,
                                PAGE_READWRITE, 0u, static_cast<DWORD>(DRC_SHM_TOTAL_SIZE),
                                DRC_SHM_NAME);
    if (sd != nullptr) {
        LocalFree(sd);
    }
    if (m_hMap == nullptr) {
        m_state = ShmState::NoMapping;
        return;
    }

    m_pView = MapViewOfFile(m_hMap, FILE_MAP_ALL_ACCESS, 0u, 0u,
                            static_cast<SIZE_T>(DRC_SHM_TOTAL_SIZE));
    if (m_pView == nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
        m_state = ShmState::NoMapping;
        return;
    }

    m_pParams = reinterpret_cast<DrcParams*>(static_cast<unsigned char*>(m_pView) +
                                             DRC_PARAMS_OFFSET);
    m_pMeter = reinterpret_cast<DrcMeter*>(static_cast<unsigned char*>(m_pView) +
                                           DRC_METER_OFFSET);

    const uint32_t pm = m_pParams->magic;
    if (pm != 0u && pm != DRC_MAGIC) {
        m_state = ShmState::ForeignMagic;   /* 他方占用：整个共享区一个字都不写 */
        return;
    }

    if (m_pMeter->magic != DRC_MAGIC) {
        m_pMeter->seq     = 0u;
        m_pMeter->version = DRC_METER_VERSION;
        m_pMeter->magic   = DRC_MAGIC;
    }

    m_state = ShmState::Ok;
}

void ShmChannel::Close() noexcept {
    if (m_pView != nullptr) {
        UnmapViewOfFile(m_pView);
        m_pView = nullptr;
    }
    if (m_hMap != nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
    }
    m_pParams = nullptr;
    m_pMeter  = nullptr;
    m_state   = ShmState::NotOpened;
}

bool ShmChannel::ReadParams(DrcParams* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    if (m_state != ShmState::Ok || m_pParams == nullptr) {
        if (m_snapshot.magic == DRC_MAGIC) {
            *out = m_snapshot;
        } else {
            FillDefaults(out);
        }
        return false;
    }

    const volatile uint32_t* pseq = &m_pParams->seq;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const uint32_t s1 = *pseq;
        if ((s1 & 1u) != 0u) {
            continue;   /* 奇 seq：写方正在写，重试 */
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m_pParams->magic != DRC_MAGIC) {
            break;      /* 尚未发布 => 默认参数 */
        }
        CopyNamedParams(m_pParams, out);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s2 = *pseq;
        if (s1 == s2) {
            m_snapshot = *out;
            return true;
        }
    }

    if (m_snapshot.magic == DRC_MAGIC) {
        *out = m_snapshot;
    } else {
        FillDefaults(out);
    }
    return false;
}

bool ShmChannel::WriteParams(const DrcParams& p) noexcept {
    if (m_state != ShmState::Ok || m_pParams == nullptr) {
        return false;
    }
    DrcParams* dst = m_pParams;
    dst->seq = dst->seq + 1u;                                   /* 奇 */
    std::atomic_thread_fence(std::memory_order_release);
    dst->magic       = DRC_MAGIC;
    dst->version     = DRC_PARAM_VERSION;
    dst->flags       = p.flags;
    dst->ceilingDb   = p.ceilingDb;
    dst->floorDb     = p.floorDb;
    dst->maxBoostDb  = p.maxBoostDb;
    dst->lookaheadMs = p.lookaheadMs;
    dst->releaseMs   = p.releaseMs;
    for (int i = 0; i < 10; ++i) {
        dst->eqGainDb[i] = p.eqGainDb[i];
    }
    std::atomic_thread_fence(std::memory_order_release);
    dst->seq = dst->seq + 1u;                                   /* 偶 */
    return true;
}

void ShmChannel::WriteMeter(const DrcMeter& in) noexcept {
    if (m_state != ShmState::Ok || m_pMeter == nullptr) {
        return;   /* 降级：不写 meter（SPEC §4 / ARCH §4） */
    }
    DrcMeter* m = m_pMeter;
    m->seq = m->seq + 1u;                                       /* 奇 */
    std::atomic_thread_fence(std::memory_order_release);
    m->activeSampleRate     = in.activeSampleRate;
    m->activeChannels       = in.activeChannels;
    m->gainReductionDb_x100 = in.gainReductionDb_x100;
    m->inPeakDb_x100        = in.inPeakDb_x100;
    m->outPeakDb_x100       = in.outPeakDb_x100;
    m->latencyMs_x10        = in.latencyMs_x10;
    m->framesProcessed      = in.framesProcessed;
    std::atomic_thread_fence(std::memory_order_release);
    m->seq = m->seq + 1u;                                       /* 偶 */
}

bool ShmChannel::ReadMeter(DrcMeter* out) noexcept {
    if (out == nullptr || m_state != ShmState::Ok || m_pMeter == nullptr) {
        return false;
    }
    const volatile uint32_t* pseq = &m_pMeter->seq;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const uint32_t s1 = *pseq;
        if ((s1 & 1u) != 0u) {
            continue;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        *out = *m_pMeter;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (*pseq == s1) {
            return true;
        }
    }
    return false;
}

}  /* namespace router */
