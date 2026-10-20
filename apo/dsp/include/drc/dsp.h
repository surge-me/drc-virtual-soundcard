// drc/dsp.h — dsp-core 公共接口（TASK-001）
//
// 行为定义（SSOT）= docs/SPEC-功能规格.md §1；参数名 = AGENTS.md §2 冻结词表。
// 纯净性：本层零 Windows 依赖（仅 C++17 标准库），可离线被任意测试宿主驱动。
#pragma once

#include <cstddef>

namespace drc {

// EQ 段数（冻结：AGENTS.md §2 十段表）
inline constexpr int kEqBandCount = 10;
// peaking biquad Q（冻结）
inline constexpr double kEqQ = 1.1;
// EQ 十段频率（冻结）：31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 Hz
extern const double kEqFreqHz[kEqBandCount];

// 参数快照。默认值 = SPEC §3 / AGENTS.md §2。
struct DspParams {
  float ceilingDb = -6.0f;    // 上限，dB
  float floorDb = -50.0f;     // 下限，dB
  float maxBoostDb = 12.0f;   // 上抬封顶，dB
  float lookaheadMs = 2.5f;   // lookahead，ms
  float releaseMs = 120.0f;   // 释放时间，ms
  float eqGainDb[kEqBandCount] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  bool bypass = false;        // 旁路：true ⇒ 逐样本 bit-exact 直通
};

// 处理链（SPEC §1，TASK-014 起）：
//   bypass → 10 段 PEQ 级联（每通道独立状态）
//          → 分频上抬（主段 1500–4000Hz / 辅段 250–1000Hz，各段用本段包络 keying，只叠加带内增量）
//          → 下压链（峰值包络 + lookahead 环形缓冲限幅，增益 ≤ 1）
//          → 输出
// 上抬在限幅**之前** ⇒ 「输出峰值 ≤ ceilingDb」对任意参数组合成立（SPEC §1.1）。
// 频带划分与包络 attack 全部写死在本层，不占 IPC 字段。
// 参数热更新：UpdateParams() 后按 5ms 线性收敛（SPEC §1.4），无样本间增益跳变。
class DspProcessor {
 public:
  explicit DspProcessor(int sampleRate = 48000, int channels = 2,
                        const DspParams& params = DspParams{});
  ~DspProcessor();
  DspProcessor(const DspProcessor&) = delete;
  DspProcessor& operator=(const DspProcessor&) = delete;

  // 采样率/通道数变更 ⇒ 重建全部内部状态（延迟线清空）。
  void Configure(int sampleRate, int channels);
  // 参数热替换：ceilingDb/floorDb/maxBoostDb/eqGainDb 按 5ms 线性收敛；
  // lookaheadMs/releaseMs 为结构性参数，立即生效（lookaheadMs 变更会清空延迟线）。
  void UpdateParams(const DspParams& params);
  // 清空全部内部状态（EQ/包络/延迟线），参数目标保持不变。
  void Reset();

  // 就地处理：interleaved 为 frames*channels 个 float。
  // channels/sampleRate 与已配置值不同 ⇒ 内部自动 Configure（会清状态）。
  void Process(float* interleaved, std::size_t frames, int channels,
               int sampleRate);

  int SampleRate() const;
  int Channels() const;
  // 最近一次 UpdateParams 的目标参数。
  const DspParams& Params() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace drc
