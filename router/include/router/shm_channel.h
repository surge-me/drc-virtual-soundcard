/*
 * shm_channel.h — IPC 共享内存访问器（TASK-006 audio-router）
 *
 * 布局 SSOT = docs/SPEC-功能规格.md §4；唯一定义点 = apo/include/drc_ipc.h（本卡只 include，
 * 逐字段照抄语义，不改其内容）。
 *   - 参数区（偏移 0）：GUI 发布参数，router 每处理块头 seqlock 读。
 *   - 电平区（偏移 512）：router 每处理块尾写 DrcMeter（口径 SPEC §4.1）。
 * 共享内存名 = Global\DrcApoSharedMem_v1（跨会话可见；本进程创建，显式 Everyone DACL）。
 *
 * 线程纪律：
 *   - OpenOrCreate()/Close() 只在启动/退出（非音频线程）调用；
 *   - ReadParams()/WriteMeter() 在音频线程调用：无锁、无堆分配、无阻塞、无 IO。
 * 降级（显式、可观测）：
 *   - 映射创建/映射失败 => Ok()==false => 不写 meter；上层用 SPEC §3 默认参数继续处理。
 *   - 参数区 magic 被他方占用 => Ok()==false（同上）。
 *   - 参数区 magic==0（GUI 尚未发布）=> 非降级，ReadParams 回落到 SPEC §3 默认参数。
 */
#ifndef ROUTER_SHM_CHANNEL_H
#define ROUTER_SHM_CHANNEL_H

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "drc_ipc.h"   /* apo/include/drc_ipc.h（SPEC §4 布局唯一定义点） */

namespace router {

enum class ShmState : int {
    NotOpened = 0,
    Ok = 1,
    NoMapping = 2,
    ForeignMagic = 3
};

class ShmChannel {
public:
    ShmChannel() noexcept = default;
    ~ShmChannel();

    ShmChannel(const ShmChannel&) = delete;
    ShmChannel& operator=(const ShmChannel&) = delete;

    /* 非音频线程：创建（或附着）共享内存，写 meter 头，判定状态。幂等。 */
    void OpenOrCreate() noexcept;
    void Close() noexcept;

    ShmState State() const noexcept { return m_state; }
    bool Ok() const noexcept { return m_state == ShmState::Ok; }

    /* 音频线程：seqlock 读参数快照（SPEC §4）。
     * 返回 true = 读到稳定完整快照；false = 沿用上次快照 / 默认值（out 仍被填好）。 */
    bool ReadParams(DrcParams* out) noexcept;

    /* seqlock 写参数（供测试种子 / 无 GUI 时发布默认参数；与 GUI 写流程同语义：
     * seq++（奇）-> memcpy -> seq++（偶））。仅在非音频线程调用。 */
    bool WriteParams(const DrcParams& p) noexcept;

    /* 音频线程：写 meter（仅 Ok 状态；seqlock 双写）。 */
    void WriteMeter(const DrcMeter& m) noexcept;

    /* 测试缝：seqlock 读 meter（读到奇 seq 重试 1 次，仍奇则返回 false）。 */
    bool ReadMeter(DrcMeter* out) noexcept;

    /* 参数默认快照（SPEC §3 / AGENTS.md §2）。 */
    static void FillDefaults(DrcParams* p) noexcept;

    /* 电平口径换算（SPEC §4.1）：0 = 0dBFS、5000 = -50dBFS。 */
    static float LinearToDb(float lin) noexcept;
    static uint32_t DbToX100(float db) noexcept;

private:
    HANDLE     m_hMap    = nullptr;
    void*      m_pView   = nullptr;
    DrcParams* m_pParams = nullptr;
    DrcMeter*  m_pMeter  = nullptr;
    ShmState   m_state   = ShmState::NotOpened;
    DrcParams  m_snapshot{};
};

}  /* namespace router */

#endif /* ROUTER_SHM_CHANNEL_H */
