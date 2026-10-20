/*
 * drc_ipc_shared.h — 共享内存 IPC 访问器（参数 seqlock 下行 / meter 上行）
 *
 * 布局 SSOT = apo/include/drc_ipc.h（逐字段照 SPEC §4）。
 * 线程模型：
 *   - OpenOrCreate()/Close() 只在非音频线程（Initialize / LockForProcess）调用；
 *   - ReadParams()/WriteMeter() 在音频线程调用：无堆分配、无锁、无阻塞、无注册表/IO。
 * 降级（显式、可观测）：
 *   - 映射创建/映射失败  => IpcState::NoMapping  => 直通且不写 meter（drc::LogDegrade 留痕）
 *   - 参数区魔数被他方占用 => IpcState::ForeignMagic => 直通且不写 meter
 *   - 参数区 magic==0（GUI 尚未发布）=> 非降级，使用 SPEC §3 默认参数
 */
#ifndef DRC_IPC_SHARED_H
#define DRC_IPC_SHARED_H

#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "drc_ipc.h"

namespace drc {

enum class IpcState : int {
    Ok = 0,
    NoMapping = 1,
    ForeignMagic = 2
};

class SharedMemory {
public:
    SharedMemory() noexcept = default;
    ~SharedMemory();

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    /* 非音频线程：创建（或附着）共享内存，写 meter 头，判定状态。幂等。 */
    void OpenOrCreate() noexcept;
    void Close() noexcept;

    IpcState State() const noexcept { return m_state; }
    bool Ok() const noexcept { return m_state == IpcState::Ok; }

    /* 音频线程：seqlock 读参数快照。
     * 返回 true = 本次读到稳定完整快照；false = 沿用上次快照 / 默认值（out 仍被填好）。 */
    bool ReadParams(DrcParams* out) noexcept;

    /* 音频线程：写 meter（仅 Ok 状态；跨实例用无锁 try 门防交错破坏 seqlock 奇偶）。 */
    void WriteMeter(const DrcMeter& m) noexcept;

    /* 参数默认快照（SPEC §3 / AGENTS.md §2） */
    static void FillDefaults(DrcParams* p) noexcept;

    /* 观测：ReadParams 因 seq 持续奇 / 非稳定而沿用旧快照的累计次数 */
    uint32_t RetryExhaustedCount() const noexcept {
        return m_retryExhausted.load(std::memory_order_relaxed);
    }

private:
    HANDLE     m_hMap   = nullptr;
    void*      m_pView  = nullptr;
    DrcParams* m_pParams = nullptr;
    DrcMeter*  m_pMeter  = nullptr;
    IpcState   m_state  = IpcState::NoMapping;
    DrcParams  m_snapshot{};                       /* 音频线程私有：上次完整快照 */
    std::atomic<uint32_t> m_retryExhausted{0};
};

}  /* namespace drc */

#endif /* DRC_IPC_SHARED_H */
