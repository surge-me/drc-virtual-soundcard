// dsp_processor.cpp — dsp-core 实现（TASK-001）
// 行为 SSOT：docs/SPEC-功能规格.md §1（下压链 §1.1 / 上抬链 §1.2 / EQ §1.3 / 平滑与旁路 §1.4）
#include "drc/dsp.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace drc {

const double kEqFreqHz[kEqBandCount] = {31.0,   62.0,   125.0,  250.0,  500.0,
                                        1000.0, 2000.0, 4000.0, 8000.0, 16000.0};

namespace {

constexpr double kPi = 3.14159265358979323846;
// 下压链软拐点总宽：ceiling 上下各 3dB ⇒ 「包络在 ceiling 以下 3dB 内开始渐进压缩」（SPEC §1.1）
constexpr double kKneeDb = 6.0;
// 上抬链过渡带宽：floor 上方 6dB 内线性混合（SPEC §1.2）
constexpr double kUpKneeDb = 6.0;
// 参数平滑总时长：SPEC §1.4「参数变更后 ≤5ms 内收敛」
constexpr double kParamSmoothMs = 5.0;
// EQ 系数重算阈值（dB）：仅在平滑中的增益实际变化时才重算
constexpr double kEqCoeffEpsDb = 1e-4;

// ===== 分频上抬（TASK-014）：频带固定写死，不可配（IPC 无字段）=====
// 依据（2026-10-15 外部资料）：FPS 脚步的「定义」与方向线索在 1.5–4kHz（核心 2.5–3.5kHz）；
// <500Hz 只有重量感且方向性差；楼上脚步本身就闷、带低频嗡鸣 ⇒ 抬低频会加剧楼层误判。
// 故主段 1500–4000Hz、辅段 250–1000Hz，<120Hz 与 >4kHz 一律不参与。
// 实现方式 = **动态 peaking EQ**（不是带通求和）：
//   · 增益只加在 peaking 滤波器的通带内，带外天然 0dB ⇒ 不存在"带外残留被一起放大"
//     （试过的两种带通求和法都栽在这：带通与主路相位抵消，或互补相减的裙边残留被 +8dB 放大）；
//   · peaking 增益 = 0dB 时系数退化为单位增益 ⇒ g=1 时逐样本直通（T3/T7-up2 依赖此性质）；
//   · 侧链只用带通测包络，相位无所谓，故用最便宜的两级 biquad。
constexpr double kUpMainCenterHz = 2500.0;  // 主段中心（覆盖 1.5–4kHz 的"脚步定义"）
constexpr double kUpMainLoHz = 1500.0;      // 主段侧链带通下沿
constexpr double kUpMainHiHz = 4000.0;      // 主段侧链带通上沿
constexpr double kUpAuxCenterHz = 600.0;    // 辅段中心（250Hz–1kHz 的脚步身体感）
constexpr double kUpAuxLoHz = 250.0;        // 辅段侧链带通下沿
constexpr double kUpAuxHiHz = 1000.0;       // 辅段侧链带通上沿
constexpr double kUpPeakQ = 1.4;
// 段包络低于 floor 24dB 以上 ⇒ 视为本段无内容（见 GainUpDb）
constexpr double kUpGateDb = 20.0;
// 侧链带通（LR4）在通带内的固有插损：两级 Butterworth 级联在交叉点各 -3dB，带内实测 ≈ -2.3dB。
// 不补偿的话，检波到的段电平偏低 2.3dB ⇒ 抬升会系统性地超出 floor 2.3dB（实测 -60dBFS@2.5kHz
// 输出 -47.99 而非 -50）。这是固定常量，不影响任何 IPC 语义。
constexpr double kUpDetCalDb = 2.3;            // peaking Q：覆盖一个倍频程以上，避免"只抬一根线"
constexpr double kUpGainStepDb = 0.125;     // 系数重算的量化步进（防 zipper，同时避免每样本重算）
// 段包络 attack 固定 5ms：主段载波 1.5–4kHz（周期 0.25–0.67ms），瞬时 attack 会让包络逐半周期
// 回落 ⇒ 增益以载波频率抖动 ⇒ 互调失真；5ms ≈ 7.5 个 1.5kHz 周期，远快于脚步包络（瞬态 20–60ms）。
constexpr double kUpAttackMs = 5.0;
// Butterworth（Q = 1/√2）单极：LR2 = 1 级、LR4 = 2 级级联
constexpr double kUpQ = 0.7071067811865476;

inline double DbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double LinToDb(double lin) {
  return 20.0 * std::log10(std::max(lin, 1e-30));
}
inline double Lerp(double a, double b, double t) { return a + (b - a) * t; }

// 下压链增益计算机（SPEC §1.1）：gain = min(1, ceilingLin/envLin) + 软拐点。
// 返回 dB（≤ 0）；envDb ≥ ceilingDb 的硬段输出电平恰为 ceiling。
inline double GainDownDb(double envDb, double ceilingDb) {
  const double d = envDb - ceilingDb;
  const double half = kKneeDb * 0.5;
  if (d <= -half) return 0.0;
  if (d >= half) return -d;
  const double k = d + half;
  return -(k * k) / (2.0 * kKneeDb);
}

// 上抬链增益（SPEC §1.2）：gainUp = min(maxBoostLin, floorLin/envLin)，env ≥ floor 时 = 1。
// 返回 dB（≥ 0）。
// 门限（TASK-014）：段包络比 floor 低 24dB 以上 ⇒ 本段基本没内容，一律不抬。
// 没有这条时，「本段没内容」会被当成「极轻的脚步」按 maxBoost 抬满，把 peaking 滤波器的
// 裙边一起抬上来（实测 8kHz 带外被抬 +2.9dB、2.5kHz 段内被邻段多抬 +0.85dB）。
inline double GainUpDb(double envDb, double floorDb, double maxBoostDb) {
  const double d = floorDb - envDb;
  if (d <= 0.0) return 0.0;
  const double target = std::min(maxBoostDb, d);
  double g = (d >= kUpKneeDb) ? target : target * (d / kUpKneeDb);
  if (d > kUpGateDb + 6.0) return 0.0;
  if (d > kUpGateDb) g *= 1.0 - (d - kUpGateDb) / 6.0;   // 20–26dB 线性收口，防开关咔哒
  return g;
}

// RBJ biquad，Direct Form II Transposed（系数已归一化 a0 = 1）。
struct Biquad {
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
  double z1 = 0.0, z2 = 0.0;

  inline double Process(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void Reset() {
    z1 = 0.0;
    z2 = 0.0;
  }
  void SetPeaking(double f0, double fs, double q, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha / A;
    b0 = (1.0 + alpha * A) / a0;
    b1 = (-2.0 * cw) / a0;
    b2 = (1.0 - alpha * A) / a0;
    a1 = (-2.0 * cw) / a0;
    a2 = (1.0 - alpha / A) / a0;
  }
  // 分频用二阶 Butterworth 低通（Q = 1/√2 时级联两级即 LR4）
  void SetLowPass(double f0, double fs, double q) {
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0 = (1.0 - cw) * 0.5 / a0;
    b1 = (1.0 - cw) / a0;
    b2 = b0;
    a1 = (-2.0 * cw) / a0;
    a2 = (1.0 - alpha) / a0;
  }
  void SetHighPass(double f0, double fs, double q) {
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0 = (1.0 + cw) * 0.5 / a0;
    b1 = -(1.0 + cw) / a0;
    b2 = b0;
    a1 = (-2.0 * cw) / a0;
    a2 = (1.0 - alpha) / a0;
  }
};

struct ChannelState {
  Biquad eq[kEqBandCount];
  std::vector<double> delay;     // lookahead 音频延迟线（长度 D+1）
  std::vector<double> gainRing;  // lookahead 增益环形缓冲（长度 D+1）
  std::size_t w = 0;
  double envDown = 0.0;  // 下压链峰值包络（线性）
  // ===== 分频上抬（TASK-014）= 两条动态 peaking EQ =====
  Biquad scMainHp, scMainHp2, scMainLp, scMainLp2;   // 主段侧链带通 1.5–4kHz（LR4，只喂包络）
  Biquad scAuxHp, scAuxHp2, scAuxLp, scAuxLp2;       // 辅段侧链带通 250Hz–1kHz（LR4）
  Biquad dynMain;              // 主段 peaking @2.5kHz（时变增益，作用于音频）
  Biquad dynAux;               // 辅段 peaking @600Hz
  double envMain = 0.0, envAux = 0.0;   // 段包络（线性，attack/release 平滑）
  double gDbMain = 0.0, gDbAux = 0.0;   // 平滑后的目标增益（dB，≥0）
  double gAppliedMain = -999.0, gAppliedAux = -999.0;  // 已写入系数的量化值（dB）
};

inline double ReleaseCoeff(double releaseMs, int sampleRate) {
  const double tau = std::max(1.0, releaseMs * sampleRate / 1000.0);
  return std::exp(-1.0 / tau);
}

inline double AttackCoeff(double attackMs, int sampleRate) {
  const double tau = std::max(1.0, attackMs * sampleRate / 1000.0);
  return std::exp(-1.0 / tau);
}

// 一极跟随：升沿用 attack 系数、降沿用 release 系数（系数 = exp(-1/tau)）。
inline double Follow(double env, double a, double att, double rel) {
  const double c = (a > env) ? att : rel;
  return a + (env - a) * c;
}

// 量化增益并（必要时）重算 peaking 系数；死区防 0↔1 步进反复切换。
inline void ApplyQuantizedGain(Biquad& f, double gainDb, double& appliedDb,
                               double centerHz, int sampleRate) {
  double q = (gainDb < kUpGainStepDb) ? 0.0
                                      : std::round(gainDb / kUpGainStepDb) * kUpGainStepDb;
  if (q == appliedDb) return;
  f.SetPeaking(centerHz, static_cast<double>(sampleRate), kUpPeakQ, q);
  if (q <= 0.0) f.Reset();   // 关断：清状态，免得下次开启读到陈旧状态
  appliedDb = q;
}

}  // namespace

struct DspProcessor::Impl {
  int sampleRate = 48000;
  int channels = 2;
  int lookaheadSamples = 0;  // D
  double relCoeff = 0.0;
  double attCoeff = 0.0;   // 段包络 attack 系数（5ms 固定，TASK-014）
  double gainCoeff = 0.0;  // 段增益一阶平滑系数（≤5ms）
  bool upOn = false;       // 上抬链是否参与（maxBoostDb > 0）；边沿切换时清段状态
  int smoothTotal = 1;
  int smoothElapsed = 0;
  bool ramping = false;
  DspParams target{};
  DspParams cur{};
  DspParams rampStart{};
  double eqAppliedDb[kEqBandCount] = {};
  std::vector<ChannelState> ch;

  void RebuildRings();      // lookahead 深度变更 ⇒ 重建环形缓冲（不可平滑）
  void ResetChannels();     // 清空 EQ 状态 / 包络 / 延迟线
  void UpdateEqCoeffs();    // 平滑增益变化时才重算 biquad 系数
  void AdvanceSmoothing();  // 每样本推进参数平滑（SPEC §1.4）
  void SetupUpBands();      // 分频滤波器系数（与采样率绑定）
  void ResetUpBands(ChannelState& c);  // 只清段状态（开/关边沿用）
};

void DspProcessor::Impl::RebuildRings() {
  Impl& s = *this;
  const double la = std::max(0.0, static_cast<double>(s.cur.lookaheadMs));
  s.lookaheadSamples = static_cast<int>(std::lround(la * s.sampleRate / 1000.0));
  if (s.lookaheadSamples < 0) s.lookaheadSamples = 0;
  const std::size_t size = static_cast<std::size_t>(s.lookaheadSamples) + 1u;
  for (ChannelState& c : s.ch) {
    c.delay.assign(size, 0.0);
    c.gainRing.assign(size, 1.0);
    c.w = 0;
  }
}

void DspProcessor::Impl::ResetChannels() {
  Impl& s = *this;
  const std::size_t size = static_cast<std::size_t>(s.lookaheadSamples) + 1u;
  for (ChannelState& c : s.ch) {
    for (Biquad& b : c.eq) {
      b.Reset();
    }
    c.delay.assign(size, 0.0);
    c.gainRing.assign(size, 1.0);
    c.w = 0;
    c.envDown = 0.0;
    s.ResetUpBands(c);
  }
}

// 分频滤波器系数（与采样率绑定、全通道同值）：Configure() 时对每个通道写一次。
void DspProcessor::Impl::SetupUpBands() {
  Impl& s = *this;
  const double fs = static_cast<double>(s.sampleRate);
  for (ChannelState& c : s.ch) {
    c.scMainHp.SetHighPass(kUpMainLoHz, fs, kUpQ);
    c.scMainHp2 = c.scMainHp;
    c.scMainLp.SetLowPass(kUpMainHiHz, fs, kUpQ);
    c.scMainLp2 = c.scMainLp;
    c.scAuxHp.SetHighPass(kUpAuxLoHz, fs, kUpQ);
    c.scAuxHp2 = c.scAuxHp;
    c.scAuxLp.SetLowPass(kUpAuxHiHz, fs, kUpQ);
    c.scAuxLp2 = c.scAuxLp;
    c.dynMain.SetPeaking(kUpMainCenterHz, fs, kUpPeakQ, 0.0);
    c.dynAux.SetPeaking(kUpAuxCenterHz, fs, kUpPeakQ, 0.0);
    s.ResetUpBands(c);
  }
}

// 只清状态、不动系数（上抬链开/关切换时用，防读陈旧状态产生爆点）
void DspProcessor::Impl::ResetUpBands(ChannelState& c) {
  c.scMainHp.Reset();
  c.scMainHp2.Reset();
  c.scMainLp.Reset();
  c.scMainLp2.Reset();
  c.scAuxHp.Reset();
  c.scAuxHp2.Reset();
  c.scAuxLp.Reset();
  c.scAuxLp2.Reset();
  c.dynMain.Reset();
  c.dynAux.Reset();
  c.dynMain.SetPeaking(kUpMainCenterHz, static_cast<double>(sampleRate), kUpPeakQ, 0.0);
  c.dynAux.SetPeaking(kUpAuxCenterHz, static_cast<double>(sampleRate), kUpPeakQ, 0.0);
  c.envMain = 0.0;
  c.envAux = 0.0;
  c.gDbMain = 0.0;
  c.gDbAux = 0.0;
  c.gAppliedMain = -999.0;
  c.gAppliedAux = -999.0;
}

void DspProcessor::Impl::UpdateEqCoeffs() {
  Impl& s = *this;
  for (int b = 0; b < kEqBandCount; ++b) {
    const double g = static_cast<double>(s.cur.eqGainDb[b]);
    if (std::fabs(g - s.eqAppliedDb[b]) <= kEqCoeffEpsDb) continue;
    for (ChannelState& c : s.ch) {
      c.eq[b].SetPeaking(kEqFreqHz[b], static_cast<double>(s.sampleRate), kEqQ, g);
    }
    s.eqAppliedDb[b] = g;
  }
}

void DspProcessor::Impl::AdvanceSmoothing() {
  Impl& s = *this;
  if (!s.ramping) return;
  ++s.smoothElapsed;
  double t =
      static_cast<double>(s.smoothElapsed) / static_cast<double>(s.smoothTotal);
  if (t > 1.0) t = 1.0;
  s.cur.ceilingDb =
      static_cast<float>(Lerp(s.rampStart.ceilingDb, s.target.ceilingDb, t));
  s.cur.floorDb =
      static_cast<float>(Lerp(s.rampStart.floorDb, s.target.floorDb, t));
  s.cur.maxBoostDb =
      static_cast<float>(Lerp(s.rampStart.maxBoostDb, s.target.maxBoostDb, t));
  for (int b = 0; b < kEqBandCount; ++b) {
    s.cur.eqGainDb[b] = static_cast<float>(
        Lerp(s.rampStart.eqGainDb[b], s.target.eqGainDb[b], t));
  }
  if (s.smoothElapsed >= s.smoothTotal) {
    s.ramping = false;
    s.cur = s.target;  // 精确到位（免浮点残差）
  }
}

DspProcessor::DspProcessor(int sampleRate, int channels, const DspParams& params)
    : impl_(new Impl()) {
  impl_->target = params;
  impl_->cur = params;
  impl_->rampStart = params;
  impl_->ramping = false;
  Configure(sampleRate, channels);
}

DspProcessor::~DspProcessor() {
  delete impl_;
  impl_ = nullptr;
}

void DspProcessor::Configure(int sampleRate, int channels) {
  Impl& s = *impl_;
  if (sampleRate <= 0) sampleRate = 48000;
  if (channels <= 0) channels = 1;
  s.sampleRate = sampleRate;
  s.channels = channels;
  s.smoothTotal =
      static_cast<int>(std::lround(kParamSmoothMs * sampleRate / 1000.0));
  if (s.smoothTotal < 1) s.smoothTotal = 1;
  s.smoothElapsed = 0;
  s.ramping = false;
  s.relCoeff = ReleaseCoeff(static_cast<double>(s.cur.releaseMs), s.sampleRate);
  s.attCoeff = AttackCoeff(kUpAttackMs, s.sampleRate);
  s.gainCoeff = AttackCoeff(kParamSmoothMs, s.sampleRate);
  s.upOn = false;
  s.ch.assign(static_cast<std::size_t>(channels), ChannelState{});
  // 首次配置：参数直接到位（不做平滑），并强制 EQ 系数重算。
  s.cur = s.target;
  for (int b = 0; b < kEqBandCount; ++b) s.eqAppliedDb[b] = 1e9;
  s.RebuildRings();
  s.ResetChannels();
  s.SetupUpBands();
}

void DspProcessor::UpdateParams(const DspParams& params) {
  Impl& s = *impl_;
  s.rampStart = s.cur;
  s.target = params;
  s.smoothElapsed = 0;
  s.ramping = true;
  // 非平滑（结构性）项：立即生效
  s.cur.bypass = params.bypass;
  const bool lookChanged = (params.lookaheadMs != s.cur.lookaheadMs);
  const bool relChanged = (params.releaseMs != s.cur.releaseMs);
  s.cur.lookaheadMs = params.lookaheadMs;
  s.cur.releaseMs = params.releaseMs;
  if (relChanged) {
    s.relCoeff = ReleaseCoeff(static_cast<double>(params.releaseMs), s.sampleRate);
  }
  if (lookChanged) s.RebuildRings();
}

void DspProcessor::Reset() {
  Impl& s = *impl_;
  s.ramping = false;
  s.smoothElapsed = 0;
  s.cur = s.target;
  s.rampStart = s.target;
  for (int b = 0; b < kEqBandCount; ++b) s.eqAppliedDb[b] = 1e9;
  s.ResetChannels();
}

void DspProcessor::Process(float* interleaved, std::size_t frames, int channels,
                           int sampleRate) {
  if (interleaved == nullptr || frames == 0) return;
  Impl& s = *impl_;
  if (channels != s.channels || sampleRate != s.sampleRate) {
    Configure(sampleRate, channels);
  }
  if (s.target.bypass) return;  // SPEC §1.4：逐样本 bit-exact 直通

  const int C = s.channels;
  const int S = s.lookaheadSamples + 1;

  // 上抬链开/关边沿：清段状态，防再次开启时读到陈旧滤波器状态产生爆点（TASK-014）
  {
    const bool upNow = (s.cur.maxBoostDb > 0.0f) && (s.cur.floorDb < 0.0f);
    if (upNow != s.upOn) {
      for (ChannelState& c : s.ch) s.ResetUpBands(c);
      s.upOn = upNow;
    }
  }
  const bool upOn = s.upOn;

  for (std::size_t f = 0; f < frames; ++f) {
    s.AdvanceSmoothing();
    s.UpdateEqCoeffs();

    const double ceilingLin = DbToLin(static_cast<double>(s.cur.ceilingDb));
    const double floorDb = static_cast<double>(s.cur.floorDb);
    const double maxBoostDb = static_cast<double>(s.cur.maxBoostDb);

    for (int c = 0; c < C; ++c) {
      ChannelState& cs = s.ch[c];
      const std::size_t idx =
          f * static_cast<std::size_t>(C) + static_cast<std::size_t>(c);
      double x = static_cast<double>(interleaved[idx]);

      // 1) 10 段 PEQ 级联（每通道独立状态）
      for (int b = 0; b < kEqBandCount; ++b) x = cs.eq[b].Process(x);

      // 2) 分频上抬（TASK-014）：主段 1.5–4kHz（LR4）+ 辅段 250Hz–1kHz（LR2），
      //    各段用**本段包络** keying，只叠加带内增量：y = x + (g-1)·band。
      //    g = 1 时 y ≡ x（EQ 全 0 + 无上抬时仍严格直通，T3 依赖此性质）。
      double y = x;
      if (upOn) {
        // 侧链：带通测本段包络（相位无所谓，只喂检波）
        const double scM = cs.scMainLp2.Process(cs.scMainLp.Process(
            cs.scMainHp2.Process(cs.scMainHp.Process(x))));
        cs.envMain = Follow(cs.envMain, std::fabs(scM), s.attCoeff, s.relCoeff);
        const double tgtM =
            GainUpDb(LinToDb(cs.envMain) + kUpDetCalDb, floorDb, maxBoostDb);
        cs.gDbMain = tgtM + (cs.gDbMain - tgtM) * s.gainCoeff;  // ≤5ms 平滑

        const double scA = cs.scAuxLp2.Process(cs.scAuxLp.Process(
            cs.scAuxHp2.Process(cs.scAuxHp.Process(x))));
        cs.envAux = Follow(cs.envAux, std::fabs(scA), s.attCoeff, s.relCoeff);
        const double tgtA =
            GainUpDb(LinToDb(cs.envAux) + kUpDetCalDb, floorDb, maxBoostDb);
        cs.gDbAux = tgtA + (cs.gDbAux - tgtA) * s.gainCoeff;

        // 增益量化到 0.125dB 步进，只有跨步才重算 peaking 系数（否则每样本算 sin/cos）。
        // 死区：< 1 步进视为关 —— 否则 0↔0.125dB 会反复切换系数，滤波器每次换系数都产生
        // 一次小瞬态（实测在 DC 输入上表现为 0.027dB 稳态偏移 + 0.08dB 相邻样本跳变）。
        ApplyQuantizedGain(cs.dynMain, cs.gDbMain, cs.gAppliedMain, kUpMainCenterHz,
                           s.sampleRate);
        ApplyQuantizedGain(cs.dynAux, cs.gDbAux, cs.gAppliedAux, kUpAuxCenterHz,
                           s.sampleRate);

        // 0dB 时 peaking 系数为单位增益 ⇒ 直通（不跑滤波器，省 CPU 且严格恒等）
        if (cs.gAppliedMain > 0.0) y = cs.dynMain.Process(y);
        if (cs.gAppliedAux > 0.0) y = cs.dynAux.Process(y);
      }

      // 3) 下压链：峰值包络（瞬时 attack）→ 增益计算机 → lookahead 环形缓冲
      //    放在上抬之后 ⇒ 输出峰值 ≤ ceiling 的不变式对任意参数组合都成立（SPEC §1.1）。
      const double ay = std::fabs(y);
      cs.envDown = std::max(ay, cs.envDown * s.relCoeff);
      const double gt =
          DbToLin(GainDownDb(LinToDb(cs.envDown), LinToDb(ceilingLin)));
      cs.delay[cs.w] = y;
      cs.gainRing[cs.w] = gt;
      const std::size_t rd = (cs.w + 1) % static_cast<std::size_t>(S);
      const double yd = cs.delay[rd];
      double g = cs.gainRing[0];
      for (int i = 1; i < S; ++i) g = std::min(g, cs.gainRing[i]);
      cs.w = rd;
      // 不变式：g ≤ gt[n-D] ⇒ |out| ≤ |y[n-D]| * ceilingLin/|y[n-D]| = ceilingLin
      const double out = yd * g;

      interleaved[idx] = static_cast<float>(out);
    }
  }
}

int DspProcessor::SampleRate() const { return impl_->sampleRate; }

int DspProcessor::Channels() const { return impl_->channels; }

const DspParams& DspProcessor::Params() const { return impl_->target; }

}  // namespace drc
