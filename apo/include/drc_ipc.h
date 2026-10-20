/*
 * drc_ipc.h — IPC 共享内存布局唯一定义点（C 语义，1 字节对齐，小端）
 *
 * SSOT = docs/SPEC-功能规格.md §4。本文件逐字段照抄 SPEC §4 表与代码块：
 *   偏移 0    .. 511  : DrcParams   （GUI -> APO，seqlock 参数下行）
 *   偏移 512  .. 1023 : DrcMeter    （APO -> GUI，电平上行）
 *   偏移 1024 .. 4095 : 保留（0）
 *
 * 共享内存名：Global\DrcApoSharedMem_v1（APO CreateFileMapping，GUI OpenFileMapping）
 *   —— 2026-10-15 e2e 订正：原 Local\ 在 audiodg=会话0 与用户会话间互不可见，必须 Global。
 *      见 docs/SPEC-功能规格.md §4。
 *
 * ── 布局实现说明（重要，见 docs/devlogs/2026-10-15-apo-com.md）──────────────
 * SPEC §4 的代码块给 reserved 计数为 DrcParams[118] / DrcMeter[119]，但按字段实算
 * 分别为 548 / 524 字节，与同一处注释、同表「512 字节」及 TASK-002 验收 static_assert
 * 「各结构 = 512 字节」不自洽（reserved 为补齐字段，其计数属算术笔误）。
 * 本实现以「512 字节」为准（表 / 注释 / 卡面 static_assert 三处一致），
 * 仅把补齐数组计数订正为 DrcParams[109] / DrcMeter[117]：
 *   DrcParams = 76 + 109*4 = 512
 *   DrcMeter  = 44 + 117*4 = 512   （44 = 9 个 u32 =36 + uint64 framesProcessed =8）
 * 命名字段的名称/类型/相对顺序/偏移**零改动**。已上报 lead 请求订正 SPEC §4。
 * ───────────────────────────────────────────────────────────────────────────
 *
 * 对齐：SPEC §4 声明「1 字节对齐」⇒ 用 #pragma pack(push,1) 关掉隐式填充，
 * 使跨编译器 / 跨语言（C# 手工偏移）布局确定。下面用 static_assert(offsetof...)
 * 把每个命名字段的偏移钉死，作为可编译校验的布局证据。
 */
#ifndef DRC_IPC_H
#define DRC_IPC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 常量 ─────────────────────────────────────────────────────────────── */
#define DRC_MAGIC             0x43524421u   /* "DRC!" */
#define DRC_PARAM_VERSION     1u
#define DRC_METER_VERSION     1u

#define DRC_PARAMS_OFFSET     0u
#define DRC_METER_OFFSET      512u
#define DRC_SHM_TOTAL_SIZE    4096u

/* DrcParams.flags 位定义 */
#define DRC_FLAG_BYPASS       0x00000001u   /* bit0 = bypass */

/* 共享内存名（宽字符）。Global\ = 跨会话可见（audiodg=会话0 ↔ 用户会话 GUI）。 */
#define DRC_SHM_NAME          L"Global\\DrcApoSharedMem_v1"

/* 默认参数（SPEC §3 / AGENTS.md §2，GUI 未发布参数时 APO 使用） */
#define DRC_DEFAULT_CEILING_DB    (-6.0f)
#define DRC_DEFAULT_FLOOR_DB      (-50.0f)
#define DRC_DEFAULT_MAX_BOOST_DB  (12.0f)
#define DRC_DEFAULT_LOOKAHEAD_MS  (2.5f)
#define DRC_DEFAULT_RELEASE_MS    (120.0f)

/* ── 结构（pack(1)） ─────────────────────────────────────────────────── */
#pragma pack(push, 1)

typedef struct DrcParams {          /* 总 512 字节 */
  uint32_t magic;                   /* = DRC_MAGIC */
  volatile uint32_t seq;            /* seqlock：写前 ++、写后 ++（偶=稳定） */
  uint32_t version;                 /* = DRC_PARAM_VERSION */
  uint32_t flags;                   /* bit0 = bypass */
  float    ceilingDb;
  float    floorDb;
  float    maxBoostDb;
  float    lookaheadMs;
  float    releaseMs;
  float    eqGainDb[10];            /* 频率顺序 = AGENTS.md §2 冻结表 */
  uint32_t reserved[109];           /* 补齐 512（见文件头说明） */
} DrcParams;

typedef struct DrcMeter {           /* 总 512 字节 */
  uint32_t magic;                   /* = DRC_MAGIC */
  volatile uint32_t seq;            /* APO 每次更新 ++（偶=稳定） */
  uint32_t version;                 /* = DRC_METER_VERSION */
  uint32_t activeSampleRate;        /* 0 = 尚未锁定 */
  uint32_t activeChannels;
  uint32_t gainReductionDb_x100;    /* 当前限幅增益衰减 x100（int） */
  uint32_t inPeakDb_x100;           /* 输入峰值 x100 */
  uint32_t outPeakDb_x100;          /* 输出峰值 x100 */
  uint32_t latencyMs_x10;           /* 算法延迟 x10 */
  uint64_t framesProcessed;
  uint32_t reserved[117];           /* 补齐 512（见文件头说明） */
} DrcMeter;

#pragma pack(pop)

/* ── 布局校验（编译期） ──────────────────────────────────────────────── */
#if defined(__cplusplus)
#define DRC_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define DRC_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define DRC_STATIC_ASSERT(cond, msg)
#endif

DRC_STATIC_ASSERT(sizeof(DrcParams) == 512u, "DrcParams must be 512 bytes");
DRC_STATIC_ASSERT(sizeof(DrcMeter)  == 512u, "DrcMeter must be 512 bytes");

DRC_STATIC_ASSERT(offsetof(DrcParams, magic)        == 0u,  "DrcParams.magic");
DRC_STATIC_ASSERT(offsetof(DrcParams, seq)          == 4u,  "DrcParams.seq");
DRC_STATIC_ASSERT(offsetof(DrcParams, version)      == 8u,  "DrcParams.version");
DRC_STATIC_ASSERT(offsetof(DrcParams, flags)        == 12u, "DrcParams.flags");
DRC_STATIC_ASSERT(offsetof(DrcParams, ceilingDb)    == 16u, "DrcParams.ceilingDb");
DRC_STATIC_ASSERT(offsetof(DrcParams, floorDb)      == 20u, "DrcParams.floorDb");
DRC_STATIC_ASSERT(offsetof(DrcParams, maxBoostDb)   == 24u, "DrcParams.maxBoostDb");
DRC_STATIC_ASSERT(offsetof(DrcParams, lookaheadMs)  == 28u, "DrcParams.lookaheadMs");
DRC_STATIC_ASSERT(offsetof(DrcParams, releaseMs)    == 32u, "DrcParams.releaseMs");
DRC_STATIC_ASSERT(offsetof(DrcParams, eqGainDb)     == 36u, "DrcParams.eqGainDb");
DRC_STATIC_ASSERT(offsetof(DrcParams, reserved)     == 76u, "DrcParams.reserved");

DRC_STATIC_ASSERT(offsetof(DrcMeter, magic)               == 0u,  "DrcMeter.magic");
DRC_STATIC_ASSERT(offsetof(DrcMeter, seq)                 == 4u,  "DrcMeter.seq");
DRC_STATIC_ASSERT(offsetof(DrcMeter, version)             == 8u,  "DrcMeter.version");
DRC_STATIC_ASSERT(offsetof(DrcMeter, activeSampleRate)    == 12u, "DrcMeter.activeSampleRate");
DRC_STATIC_ASSERT(offsetof(DrcMeter, activeChannels)      == 16u, "DrcMeter.activeChannels");
DRC_STATIC_ASSERT(offsetof(DrcMeter, gainReductionDb_x100)== 20u, "DrcMeter.gainReductionDb_x100");
DRC_STATIC_ASSERT(offsetof(DrcMeter, inPeakDb_x100)       == 24u, "DrcMeter.inPeakDb_x100");
DRC_STATIC_ASSERT(offsetof(DrcMeter, outPeakDb_x100)      == 28u, "DrcMeter.outPeakDb_x100");
DRC_STATIC_ASSERT(offsetof(DrcMeter, latencyMs_x10)       == 32u, "DrcMeter.latencyMs_x10");
DRC_STATIC_ASSERT(offsetof(DrcMeter, framesProcessed)     == 36u, "DrcMeter.framesProcessed");
DRC_STATIC_ASSERT(offsetof(DrcMeter, reserved)            == 44u, "DrcMeter.reserved");

/* 区偏移自洽 */
DRC_STATIC_ASSERT(DRC_PARAMS_OFFSET + sizeof(DrcParams) == DRC_METER_OFFSET,
                  "params region must end where meter region begins");
DRC_STATIC_ASSERT(DRC_METER_OFFSET + sizeof(DrcMeter) <= DRC_SHM_TOTAL_SIZE,
                  "meter region must fit in shared memory");

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* DRC_IPC_H */
