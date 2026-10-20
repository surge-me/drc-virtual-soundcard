#include "drc_ipc_shared.h"

#include <sddl.h>   /* ConvertStringSecurityDescriptorToSecurityDescriptorW（Global\ SD） */

#include <cmath>
#include <cstring>

#include "drc_globals.h"

namespace drc {

namespace {

/* 跨实例 meter 写入门：无锁 try（exchange/CAS），不阻塞音频线程。
 * 理由：audiodg 可能同时加载本 APO 的两个实例（同一设备的 LFX + GFX 指向同一 CLSID），
 * 两实例并发对同一 meter 区做 seq++/写/seq++ 会破坏 seqlock 奇偶；用本门让同一时刻只有一个写者。 */
std::atomic<uint32_t> g_meterWriteBusy{ 0u };

/* 只复制命名字段（不复制 reserved 补齐区），并保持对 volatile seq 的读序语义。 */
void CopyNamedFields(const DrcParams* src, DrcParams* dst) noexcept {
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
    dst->seq = 0u;   /* 快照不带写方 seq */
}

}  /* namespace */

SharedMemory::~SharedMemory() {
    Close();
}

void SharedMemory::FillDefaults(DrcParams* p) noexcept {
    if (p == nullptr) {
        return;
    }
    std::memset(p, 0, sizeof(*p));
    p->magic       = DRC_MAGIC;
    p->version     = DRC_PARAM_VERSION;
    p->flags       = 0u;                     /* bypass = false */
    p->ceilingDb   = DRC_DEFAULT_CEILING_DB;
    p->floorDb     = DRC_DEFAULT_FLOOR_DB;
    p->maxBoostDb  = DRC_DEFAULT_MAX_BOOST_DB;
    p->lookaheadMs = DRC_DEFAULT_LOOKAHEAD_MS;
    p->releaseMs   = DRC_DEFAULT_RELEASE_MS;
    for (int i = 0; i < 10; ++i) {
        p->eqGainDb[i] = 0.0f;
    }
}

void SharedMemory::OpenOrCreate() noexcept {
    if (m_pView != nullptr) {
        return;  /* 幂等 */
    }

    /* Global\ 命名对象由 audiodg(SYSTEM) 创建：默认 DACL 只授 SYSTEM/Administrators，
     * 非提权用户会话的 GUI（OpenFileMapping(FILE_MAP_ALL_ACCESS)）会被拒。
     * 故显式授 Everyone 映射全权（本机本地 IPC，无网络暴露面）。SDDL: D:(A;;GA;;;WD)。
     * 构造失败（极端情况）⇒ 退回默认 SD（提权/同会话仍可用）。 */
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr) != FALSE && sd != nullptr) {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;
    }

    /* APO 侧负责创建（SPEC §4：APO CreateFileMapping / GUI OpenFileMapping） */
    m_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE,
                                (sd != nullptr) ? &sa : nullptr, PAGE_READWRITE,
                                0u, static_cast<DWORD>(DRC_SHM_TOTAL_SIZE), DRC_SHM_NAME);
    if (sd != nullptr) {
        LocalFree(sd);
    }
    if (m_hMap == nullptr) {
        const unsigned long err = static_cast<unsigned long>(GetLastError());
        m_state = IpcState::NoMapping;
        LogDegrade(L"CreateFileMapping(Global\\DrcApoSharedMem_v1) failed", err);
        return;
    }

    m_pView = MapViewOfFile(m_hMap, FILE_MAP_ALL_ACCESS, 0u, 0u,
                            static_cast<SIZE_T>(DRC_SHM_TOTAL_SIZE));
    if (m_pView == nullptr) {
        const unsigned long err = static_cast<unsigned long>(GetLastError());
        CloseHandle(m_hMap);
        m_hMap = nullptr;
        m_state = IpcState::NoMapping;
        LogDegrade(L"MapViewOfFile failed", err);
        return;
    }

    m_pParams = reinterpret_cast<DrcParams*>(static_cast<unsigned char*>(m_pView) + DRC_PARAMS_OFFSET);
    m_pMeter  = reinterpret_cast<DrcMeter*>(static_cast<unsigned char*>(m_pView) + DRC_METER_OFFSET);

    /* 先判参数区魔数：0 = GUI 尚未发布（正常）；DRC_MAGIC = 正常；其余 = 他方占用（降级）。
     * 降级时**整个共享区一个字都不写**（含 meter 头），保持「降级 ⇒ 不写 meter」自洽。 */
    const uint32_t pm = m_pParams->magic;
    if (pm != 0u && pm != DRC_MAGIC) {
        m_state = IpcState::ForeignMagic;
        LogDegrade(L"params.magic is foreign (not 0 / not DRC_MAGIC)", 0ul);
        return;
    }

    /* meter 头：仅在首次创建（magic 尚未写）时落，供 GUI 校验本区有效 */
    if (m_pMeter->magic != DRC_MAGIC) {
        m_pMeter->seq     = 0u;
        m_pMeter->version = DRC_METER_VERSION;
        m_pMeter->magic   = DRC_MAGIC;
    }

    m_state = IpcState::Ok;
}

void SharedMemory::Close() noexcept {
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
    m_state   = IpcState::NoMapping;
}

bool SharedMemory::ReadParams(DrcParams* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    if (m_state != IpcState::Ok || m_pParams == nullptr) {
        /* 降级 / 无映射：用默认参数，交由上层直通 */
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
            continue;                        /* 奇 seq：写方正在写，重试 */
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m_pParams->magic != DRC_MAGIC) {
            break;                           /* 尚未发布 => 默认参数 */
        }
        CopyNamedFields(m_pParams, out);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s2 = *pseq;
        if (s1 == s2) {
            m_snapshot = *out;               /* 完整快照缓存（音频线程私有） */
            return true;
        }
    }

    /* 上限 2 次仍不稳定（或尚未发布）：沿用上次快照；无快照则默认值。不猜。 */
    m_retryExhausted.fetch_add(1u, std::memory_order_relaxed);
    if (m_snapshot.magic == DRC_MAGIC) {
        *out = m_snapshot;
    } else {
        FillDefaults(out);
    }
    return false;
}

void SharedMemory::WriteMeter(const DrcMeter& in) noexcept {
    if (m_state != IpcState::Ok || m_pMeter == nullptr) {
        return;  /* 降级：不写 meter（SPEC §4 / ARCH §4） */
    }

    uint32_t expected = 0u;
    if (!g_meterWriteBusy.compare_exchange_strong(expected, 1u,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
        return;  /* 另一实例在写：跳过本块，绝不阻塞音频线程 */
    }

    DrcMeter* m = m_pMeter;
    m->seq = m->seq + 1u;                                    /* 奇 */
    std::atomic_thread_fence(std::memory_order_release);
    m->activeSampleRate    = in.activeSampleRate;
    m->activeChannels      = in.activeChannels;
    m->gainReductionDb_x100 = in.gainReductionDb_x100;
    m->inPeakDb_x100       = in.inPeakDb_x100;
    m->outPeakDb_x100      = in.outPeakDb_x100;
    m->latencyMs_x10       = in.latencyMs_x10;
    m->framesProcessed     = in.framesProcessed;
    std::atomic_thread_fence(std::memory_order_release);
    m->seq = m->seq + 1u;                                    /* 偶 */

    g_meterWriteBusy.store(0u, std::memory_order_release);
}

}  /* namespace drc */
