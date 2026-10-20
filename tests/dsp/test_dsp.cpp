// test_dsp.cpp — dsp-core ctest 断言宿主（TASK-001）
// 覆盖：T1 限幅 / T2 上抬(分频) / T3 EQ / T4 旁路 / T5 参数热更新 / T6 负向对照 / T7 分频上抬(TASK-014)
// 信号自产自检，无外部夹具。判据出处：docs/plans/TASK-001-dsp-core.md。
#include "drc/dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
int g_fail = 0;

// ---------------------------------------------------------------- 断言输出

void Report(const char* name, double got, double lo, double hi, const char* unit) {
  const bool ok = (got >= lo && got <= hi);
  std::printf("  [%s] %-52s got=%.6f  要求[%.4f, %.4f]%s\n", ok ? "OK" : "FAIL",
              name, got, lo, hi, unit);
  if (!ok) ++g_fail;
}

void ReportTrue(const char* name, bool ok, const char* detail) {
  std::printf("  [%s] %-52s %s\n", ok ? "OK" : "FAIL", name, detail);
  if (!ok) ++g_fail;
}

void Note(const char* name, double v, const char* unit) {
  std::printf("  [--] %-52s = %.6f%s\n", name, v, unit);
}

// ---------------------------------------------------------------- 信号工具

std::vector<float> MakeSine(int fs, double freq, double amp, std::size_t frames,
                            int ch) {
  std::vector<float> v(frames * static_cast<std::size_t>(ch), 0.0f);
  for (std::size_t i = 0; i < frames; ++i) {
    const double s = amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) /
                                    static_cast<double>(fs));
    for (int c = 0; c < ch; ++c) {
      v[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)] =
          static_cast<float>(s);
    }
  }
  return v;
}

std::vector<float> MakeConst(double amp, std::size_t frames, int ch) {
  return std::vector<float>(frames * static_cast<std::size_t>(ch),
                            static_cast<float>(amp));
}

// 确定性伪随机（LCG）——避免测试引入外部依赖
std::vector<float> MakeNoise(std::size_t frames, int ch) {
  std::vector<float> v(frames * static_cast<std::size_t>(ch), 0.0f);
  unsigned int s = 0x12345678u;
  for (std::size_t i = 0; i < v.size(); ++i) {
    s = s * 1103515245u + 12345u;
    const double u = static_cast<double>((s >> 8) & 0xFFFFu) / 32768.0 - 1.0;
    v[i] = static_cast<float>(0.3 * u);
  }
  return v;
}

void Run(drc::DspProcessor& p, std::vector<float>& buf, int ch, int fs,
         std::size_t block = 480) {
  const std::size_t n = buf.size() / static_cast<std::size_t>(ch);
  for (std::size_t i = 0; i < n; i += block) {
    const std::size_t m = std::min(block, n - i);
    p.Process(buf.data() + i * static_cast<std::size_t>(ch), m, ch, fs);
  }
}

double Peak(const std::vector<float>& v, int ch, std::size_t from,
            std::size_t to) {
  double m = 0.0;
  for (std::size_t i = from; i < to; ++i) {
    for (int c = 0; c < ch; ++c) {
      m = std::max(m, std::fabs(static_cast<double>(
                          v[i * static_cast<std::size_t>(ch) +
                            static_cast<std::size_t>(c)])));
    }
  }
  return m;
}

double Rms(const std::vector<float>& v, int ch, std::size_t from,
           std::size_t to) {
  double acc = 0.0;
  std::size_t n = 0;
  for (std::size_t i = from; i < to; ++i) {
    for (int c = 0; c < ch; ++c) {
      const double x = static_cast<double>(
          v[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)]);
      acc += x * x;
      ++n;
    }
  }
  return n ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

inline double LinToDb(double lin) { return 20.0 * std::log10(lin); }
inline double DbToLin(double db) { return std::pow(10.0, db / 20.0); }

// 理论 peaking biquad 幅值（RBJ 公式，独立于实现的直接传递函数求值）
double PeqMagDb(double f0, double fs, double q, double gainDb, double f) {
  const double A = std::pow(10.0, gainDb / 40.0);
  const double w0 = 2.0 * kPi * f0 / fs;
  const double cw = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);
  const double a0 = 1.0 + alpha / A;
  const double b0 = (1.0 + alpha * A) / a0;
  const double b1 = (-2.0 * cw) / a0;
  const double b2 = (1.0 - alpha * A) / a0;
  const double a1 = (-2.0 * cw) / a0;
  const double a2 = (1.0 - alpha / A) / a0;
  const double w = 2.0 * kPi * f / fs;
  const double c1 = std::cos(w), s1 = std::sin(w);
  const double c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
  const double nr = b0 + b1 * c1 + b2 * c2;
  const double ni = -(b1 * s1 + b2 * s2);
  const double dr = 1.0 + a1 * c1 + a2 * c2;
  const double di = -(a1 * s1 + a2 * s2);
  return 20.0 * std::log10(std::hypot(nr, ni) / std::hypot(dr, di));
}

// 用 RMS 比测某频点的实际增益。
// 探针取 -30dBFS：+6dB 后为 -24dBFS（远低于 ceiling 拐点起点 -7.5dB，限幅增益恒为 1）。
// **maxBoostDb = 0**：T3 只测 EQ，分频上抬链必须整条不参与（TASK-014 起上抬是分频结构，
// 带外泄漏会让 8kHz 探针多出约 0.01dB 的合法偏差，混进来就不再是「EQ 单测」）。
double MeasureEqGainDb(int fs, const drc::DspParams& p, double probeHz) {
  drc::DspParams pe = p;
  pe.maxBoostDb = 0.0f;
  const std::size_t frames = static_cast<std::size_t>(fs) / 2;
  const double amp = DbToLin(-30.0);
  drc::DspProcessor proc(fs, 1, pe);
  std::vector<float> in = MakeSine(fs, probeHz, amp, frames, 1);
  std::vector<float> buf = in;
  Run(proc, buf, 1, fs);
  const double rIn = Rms(in, 1, frames / 2, frames);
  const double rOut = Rms(buf, 1, frames / 2, frames);
  return LinToDb(rOut / rIn);
}

// ---------------------------------------------------------------- T1 限幅

void TestT1() {
  const int fs = 48000, ch = 1;
  const drc::DspParams p;  // 默认：ceiling -6 / floor -50 / maxBoost 12
  const double ceilingDb = p.ceilingDb;
  const std::size_t frames = static_cast<std::size_t>(fs) / 2;

  // 主断言：0dBFS 正弦（稳态取后半段）→ 输出峰值 ≤ ceiling（0.1dB 容差），
  //          且必须贴住 ceiling（防「恒衰减」假绿）。
  {
    drc::DspProcessor proc(fs, ch, p);
    std::vector<float> buf = MakeSine(fs, 1000.0, 1.0, frames, ch);
    Run(proc, buf, ch, fs);
    const double peakDb = LinToDb(Peak(buf, ch, frames / 2, frames));
    Report("T1 0dBFS 输出峰值 ≤ ceiling+0.1dB", peakDb, -1e9, ceilingDb + 0.1,
           " dBFS");
    Report("T1 0dBFS 输出峰值 ≥ ceiling-0.1dB", peakDb, ceilingDb - 0.1, 1e9,
           " dBFS");
  }
  // 下限断言：-12dBFS 稳态正弦，限幅器对中电平不作为（增益变化 ≤ 0.5dB）
  {
    drc::DspProcessor proc(fs, ch, p);
    const double amp = DbToLin(-12.0);
    std::vector<float> in = MakeSine(fs, 1000.0, amp, frames, ch);
    std::vector<float> buf = in;
    Run(proc, buf, ch, fs);
    const double dDb =
        LinToDb(Peak(buf, ch, frames / 2, frames) / Peak(in, ch, frames / 2, frames));
    Report("T1 -12dBFS 增益变化 ≤ 0.5dB", std::fabs(dDb), 0.0, 0.5, " dB");
  }
}

// ---------------------------------------------------------------- T2 上抬（分频后重述）
// TASK-014 起上抬是分频结构（主段 1.5–4kHz / 辅段 250Hz–1kHz），旧的全带口径失效：
// 「-60dBFS@1kHz 抬到 floor」不再成立（1kHz 落在辅段边缘，只能拿到部分增量）。
// 现按「段内正弦」重述：主段中心 2kHz 作为上抬探针；带外 8kHz 作为「不连带」探针。

void TestT2() {
  const int fs = 48000, ch = 1;
  const drc::DspParams p;  // 默认：ceiling -6 / floor -50 / maxBoost 12
  const std::size_t frames = static_cast<std::size_t>(fs);  // 1s（留包络建立）

  // 主断言：段内 -60dBFS@2.5kHz ⇒ 抬向 floor
  {
    drc::DspProcessor proc(fs, ch, p);
    std::vector<float> buf = MakeSine(fs, 2500.0, DbToLin(-60.0), frames, ch);
    Run(proc, buf, ch, fs);
    const double peakDb = LinToDb(Peak(buf, ch, frames / 2, frames));
    Report("T2 段内-60dBFS@2.5kHz 输出 ≥ floor-2dB", peakDb, p.floorDb - 2.0, 1e9,
           " dBFS");
    Report("T2 段内-60dBFS@2.5kHz 输出 ≤ floor+maxBoost+2dB", peakDb, -1e9,
           p.floorDb + p.maxBoostDb + 2.0, " dBFS");
    Report("T2 段内-60dBFS@2.5kHz 输出 ≈ floor(±2dB)", peakDb, p.floorDb - 2.0,
           p.floorDb + 2.0, " dBFS");
  }
  // 带外不连带：-60dBFS@8kHz（主段 LP 之外）增益 ≤ 0.5dB
  {
    drc::DspProcessor proc(fs, ch, p);
    const double amp = DbToLin(-60.0);
    std::vector<float> in = MakeSine(fs, 8000.0, amp, frames, ch);
    std::vector<float> buf = in;
    Run(proc, buf, ch, fs);
    const double dDb =
        LinToDb(Peak(buf, ch, frames / 2, frames) / Peak(in, ch, frames / 2, frames));
    Report("T2 带外-60dBFS@8kHz 增益变化 ≤0.5dB", std::fabs(dDb), 0.0, 0.5, " dB");
  }
  // 枪声区：-12dBFS@2.5kHz 增益变化 ≤ 0.5dB（上抬不得连带放大）
  {
    drc::DspProcessor proc(fs, ch, p);
    const double amp = DbToLin(-12.0);
    std::vector<float> in = MakeSine(fs, 2500.0, amp, frames, ch);
    std::vector<float> buf = in;
    Run(proc, buf, ch, fs);
    const double dDb =
        LinToDb(Peak(buf, ch, frames / 2, frames) / Peak(in, ch, frames / 2, frames));
    Report("T2 -12dBFS@2.5kHz 增益变化 ≤0.5dB", std::fabs(dDb), 0.0, 0.5, " dB");
  }
}

// ---------------------------------------------------------------- T3 EQ

void TestT3() {
  const int fs = 48000;
  const int kBand1k = 5;  // 冻结表第 6 段 = 1000 Hz
  const double probes[6] = {62.0, 250.0, 1000.0, 4000.0, 8000.0, 16000.0};

  for (int sgn = 0; sgn < 2; ++sgn) {
    const double gain = (sgn == 0) ? 6.0 : -6.0;
    drc::DspParams p;
    p.eqGainDb[kBand1k] = static_cast<float>(gain);

    const double g1k = MeasureEqGainDb(fs, p, 1000.0);
    char nm[128];
    std::snprintf(nm, sizeof(nm), "T3 %+.0fdB@1kHz 实测增益(±0.5dB)", gain);
    Report(nm, g1k, gain - 0.5, gain + 0.5, " dB");

    const double g31 = MeasureEqGainDb(fs, p, 31.0);
    std::snprintf(nm, sizeof(nm), "T3 %+.0fdB@1kHz 在 31Hz 增益 ≈0(±0.5dB)", gain);
    Report(nm, g31, -0.5, 0.5, " dB");

    // 合成扫频：实测 vs 理论 biquad 幅值（独立求值）偏差 ≤ 0.5dB
    for (int i = 0; i < 6; ++i) {
      const double f = probes[i];
      const double gm = MeasureEqGainDb(fs, p, f);
      const double th = PeqMagDb(1000.0, static_cast<double>(fs), drc::kEqQ,
                                 gain, f);
      std::snprintf(nm, sizeof(nm), "T3 %+.0fdB 扫频 %.0fHz |实测-理论| ≤0.5dB",
                    gain, f);
      Report(nm, std::fabs(gm - th), 0.0, 0.5, " dB");
    }
  }

  // 增益全 0 ⇒ 单位增益直通（SPEC §1.3）：按固有延迟 lookahead 对齐后逐样本一致。
  // maxBoostDb = 0：本条只判 EQ 透明性；上抬链整条不参与（否则段包络在流开头建立的那 5ms
  // 会带来一次合法提升，混进来就不再是「EQ 单测」——上抬透明性由 T7 up2 单独判）。
  {
    drc::DspParams p;  // eqGainDb 全 0
    p.maxBoostDb = 0.0f;
    const int D = static_cast<int>(std::lround(p.lookaheadMs * fs / 1000.0));
    const std::size_t frames = static_cast<std::size_t>(fs) / 2;
    drc::DspProcessor proc(fs, 1, p);
    std::vector<float> in = MakeSine(fs, 1000.0, 0.25, frames, 1);
    std::vector<float> buf = in;
    Run(proc, buf, 1, fs);
    double maxdiff = 0.0;
    for (std::size_t i = static_cast<std::size_t>(D); i < frames; ++i) {
      maxdiff = std::max(maxdiff, std::fabs(static_cast<double>(buf[i]) -
                                            static_cast<double>(in[i - static_cast<std::size_t>(D)])));
    }
    Report("T3 EQ 全 0 时（延迟对齐）最大样本偏差 ≤1e-6", maxdiff, 0.0, 1e-6, "");
  }
}

// ---------------------------------------------------------------- T4 旁路

void TestT4() {
  const int fs = 48000, ch = 2;

  {
    drc::DspParams p;
    p.bypass = true;
    drc::DspProcessor proc(fs, ch, p);
    std::vector<float> in = MakeNoise(static_cast<std::size_t>(fs) / 4, ch);
    std::vector<float> buf = in;
    Run(proc, buf, ch, fs);
    ReportTrue("T4 bypass=true 输出与输入 bit-exact",
               std::memcmp(in.data(), buf.data(), in.size() * sizeof(float)) == 0,
               "");
  }
  // 运行中切旁路：先做一段真实处理，再开旁路，旁路段仍须 bit-exact
  {
    drc::DspProcessor proc(fs, ch, drc::DspParams{});
    const std::size_t n = static_cast<std::size_t>(fs) / 10;
    std::vector<float> a = MakeSine(fs, 1000.0, 0.9, n, ch);
    proc.Process(a.data(), n, ch, fs);
    drc::DspParams bp;
    bp.bypass = true;
    proc.UpdateParams(bp);
    std::vector<float> in = MakeNoise(n, ch);
    std::vector<float> buf = in;
    proc.Process(buf.data(), n, ch, fs);
    ReportTrue("T4 运行中切 bypass 后仍 bit-exact",
               std::memcmp(in.data(), buf.data(), in.size() * sizeof(float)) == 0,
               "");
  }
}

// ---------------------------------------------------------------- T5 参数热更新

void TestT5() {
  const int fs = 48000, ch = 1;
  const double amp = 0.25;             // 常量输入（-12.04dBFS）：out/amp 即限幅增益
  const double ceilingNew = -16.0;     // 10dB 步进
  const double gTarget = DbToLin(ceilingNew) / amp;
  const std::size_t settle = static_cast<std::size_t>(fs) / 10;  // 0.1s 预跑
  const std::size_t after = static_cast<std::size_t>(fs) / 2;    // 0.5s 观察窗
  const std::size_t smoothSamples =
      static_cast<std::size_t>(std::lround(0.005 * fs));  // 5ms

  for (int arm = 0; arm < 2; ++arm) {
    const char* tag = (arm == 0) ? "A(lookahead=2.5ms)" : "B(lookahead=0)";
    drc::DspParams p;
    p.maxBoostDb = 0.0f;   // 本条只测限幅与参数平滑；上抬链的系数换挡另有 T7-up8 单独判
    if (arm == 1) p.lookaheadMs = 0.0f;
    drc::DspProcessor proc(fs, ch, p);
    std::vector<float> a = MakeConst(amp, settle, ch);
    proc.Process(a.data(), settle, ch, fs);
    drc::DspParams p2 = p;
    p2.ceilingDb = static_cast<float>(ceilingNew);
    proc.UpdateParams(p2);
    std::vector<float> b = MakeConst(amp, after, ch);
    proc.Process(b.data(), after, ch, fs);

    std::vector<double> g(after);
    for (std::size_t i = 0; i < after; ++i) g[i] = static_cast<double>(b[i]) / amp;

    const std::size_t none = after;
    std::size_t t0 = none, t1 = none;
    double maxAdj = 0.0;
    for (std::size_t i = 1; i < after; ++i) {
      maxAdj = std::max(maxAdj, std::fabs(LinToDb(g[i] / g[i - 1])));
      if (t0 == none && std::fabs(LinToDb(g[i])) > 0.001) t0 = i;
      if (t0 != none && t1 == none &&
          std::fabs(LinToDb(g[i] / gTarget)) <= 0.05) {
        t1 = i;
      }
    }

    char nm[160];
    std::snprintf(nm, sizeof(nm), "T5%s 相邻样本增益跳变 ≤0.05dB", tag);
    Report(nm, maxAdj, 0.0, 0.05, " dB");

    const double convMs = (t1 == none) ? -1.0
                                       : static_cast<double>(t1) * 1000.0 /
                                             static_cast<double>(fs);
    std::snprintf(nm, sizeof(nm), "T5%s 自参数变更起收敛 ≤5ms", tag);
    Report(nm, convMs, 0.0, 5.0, " ms");

    const double spanMs =
        (t0 == none || t1 == none)
            ? -1.0
            : static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(fs);
    std::snprintf(nm, sizeof(nm), "T5%s 变化起点→收敛跨度 ≤5ms", tag);
    Report(nm, spanMs, 0.0, 5.0, " ms");

    const double finalErrDb =
        std::fabs(LinToDb(g[after - 1] / gTarget));
    std::snprintf(nm, sizeof(nm), "T5%s 稳态增益误差 ≤0.05dB", tag);
    Report(nm, finalErrDb, 0.0, 0.05, " dB");

    std::snprintf(nm, sizeof(nm), "T5%s 检出变化起点/收敛点", tag);
    ReportTrue(nm, t0 != none && t1 != none && t1 > t0, "");
    Note("  t0(首个增益变化样本)", static_cast<double>(t0), " samples");
    Note("  t1(收敛样本)", static_cast<double>(t1), " samples");
    Note("  5ms 理论样本数", static_cast<double>(smoothSamples), " samples");
    Note("  变化前增益(dB)", LinToDb(g[0]), " dB");
    Note("  稳态增益(dB)", LinToDb(g[after - 1]), " dB");
  }
}

// ---------------------------------------------------------------- T6 负向对照

void TestT6() {
  const int fs = 48000, ch = 1;
  const drc::DspParams p;
  const std::size_t frames = static_cast<std::size_t>(fs) / 2;
  const double ceilingDb = p.ceilingDb;

  drc::DspProcessor proc(fs, ch, p);
  std::vector<float> limited = MakeSine(fs, 1000.0, 1.0, frames, ch);
  std::vector<float> passthru = limited;  // 无处理直通路径（复刻）
  Run(proc, limited, ch, fs);

  // 对照 1：直通路径确实违反 ceiling ⇒ T1 判据有区分力（非恒真）
  const double passDb = LinToDb(Peak(passthru, ch, frames / 2, frames));
  Report("T6 直通路径峰值 > ceiling+1dB（判据有区分力）", passDb,
         ceilingDb + 1.0, 1e9, " dBFS");

  // 对照 2：限幅输出与直通显著不同。
  // 必须按固有 lookahead 延迟对齐后再比：否则「只延迟、不限幅」的变异也会因相位差而
  // 通过（该变异实测已证伪本断言的未对齐版本 —— 见 devlog 踩坑）。
  const int D = static_cast<int>(std::lround(p.lookaheadMs * fs / 1000.0));
  double maxdiff = 0.0;
  double maxdiffAligned = 0.0;
  for (std::size_t i = frames / 2; i < frames; ++i) {
    maxdiff = std::max(maxdiff, std::fabs(static_cast<double>(limited[i]) -
                                          static_cast<double>(passthru[i])));
    maxdiffAligned =
        std::max(maxdiffAligned,
                 std::fabs(static_cast<double>(limited[i]) -
                           static_cast<double>(passthru[i - static_cast<std::size_t>(D)])));
  }
  Report("T6 限幅输出 vs 直通 最大样本差 > 0.1", maxdiff, 0.1, 1e9, "");
  Report("T6 限幅输出 vs 直通(延迟对齐) 最大样本差 > 0.1", maxdiffAligned, 0.1,
         1e9, "");
}

// ---------------------------------------------------------------- T7 分频上抬（TASK-014）
// 判据来源：docs/plans/TASK-014-footstep-uplift.md §7。
// 段内正弦 = 主段 1.5–4kHz（取 2kHz）/ 辅段 250Hz–1kHz（取 500Hz）；带外 = 80Hz / 8kHz / 12kHz。

// 用正交相关测某单音幅值（含 lookahead 延迟后的稳态窗），比峰值更抗宽带泄漏。
double ToneAmp(const std::vector<float>& v, int ch, int fs, double f, std::size_t from,
               std::size_t to) {
  double acc = 0.0;
  for (std::size_t i = from; i < to; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(fs);
    acc += static_cast<double>(v[i * static_cast<std::size_t>(ch)]) *
           std::sin(2.0 * kPi * f * t);
  }
  const double n = static_cast<double>(to - from);
  return std::fabs(2.0 * acc / n);   // 与同频正弦的投影幅值
}

void TestT7() {
  const int fs = 48000, ch = 1;
  const std::size_t frames = static_cast<std::size_t>(fs);  // 1s，取后 250ms 观测
  const std::size_t from = frames * 3 / 4;

  // up1 段内提升：-40dBFS@2.5kHz，floor -30 / maxBoost 12 ⇒ 增益 ∈ [+6, +12.5] dB
  {
    drc::DspParams p;
    p.floorDb = -30.0f;
    p.maxBoostDb = 12.0f;
    drc::DspProcessor proc(fs, ch, p);
    std::vector<float> in = MakeSine(fs, 2500.0, DbToLin(-40.0), frames, ch);
    std::vector<float> buf = in;
    Run(proc, buf, ch, fs);
    const double g = LinToDb(Rms(buf, ch, from, frames) / Rms(in, ch, from, frames));
    Report("T7 up1 段内-40dBFS@2.5kHz 增益 ≥+6dB", g, 6.0, 1e9, " dB");
    Report("T7 up1 段内-40dBFS@2.5kHz 增益 ≤+12.5dB", g, -1e9, 12.5, " dB");
    Note("  up1 实测增益", g, " dB");
  }

  // up2 上抬可关：maxBoost=0 ⇒ 与「从不参与」的输出逐样本一致（≤1e-6）
  {
    drc::DspParams p;
    p.floorDb = -30.0f;
    p.maxBoostDb = 0.0f;
    drc::DspProcessor proc(fs, ch, p);
    std::vector<float> in = MakeSine(fs, 2500.0, DbToLin(-40.0), frames, ch);
    std::vector<float> buf = in;
    Run(proc, buf, ch, fs);
    // 参考：参数完全相同、但输入再跑一遍的「直通 + 限幅」路径（ceil 不介入 ⇒ 应与输入一致）
    const int D = static_cast<int>(std::lround(p.lookaheadMs * fs / 1000.0));
    double maxdiff = 0.0;
    for (std::size_t i = static_cast<std::size_t>(D); i < frames; ++i) {
      maxdiff = std::max(maxdiff, std::fabs(static_cast<double>(buf[i]) -
                                            static_cast<double>(in[i - static_cast<std::size_t>(D)])));
    }
    Report("T7 up2 maxBoost=0 时上抬零参与（延迟对齐）≤1e-6", maxdiff, 0.0, 1e-6, "");
  }

  // up3 带外不连带：80Hz / 8kHz / 12kHz 各 ≤ +0.5dB（floor -36 / maxBoost 8）
  {
    const double freqs[3] = {80.0, 8000.0, 12000.0};
    drc::DspParams p;
    p.floorDb = -36.0f;
    p.maxBoostDb = 8.0f;
    for (int i = 0; i < 3; ++i) {
      drc::DspProcessor proc(fs, ch, p);
      std::vector<float> in = MakeSine(fs, freqs[i], DbToLin(-40.0), frames, ch);
      std::vector<float> buf = in;
      Run(proc, buf, ch, fs);
      const double g = LinToDb(Rms(buf, ch, from, frames) / Rms(in, ch, from, frames));
      char nm[128];
      std::snprintf(nm, sizeof(nm), "T7 up3 带外%.0fHz 增益 ≤+0.5dB", freqs[i]);
      Report(nm, g, -1e9, 0.5, " dB");
    }
  }

  // up4 封顶生效：floor -12（d=28）/ maxBoost 6 ⇒ 增益 ∈ [+2.5, +6.5]
  {
    drc::DspParams p;
    p.floorDb = -32.0f;   // d = 8dB（< 门限 24dB），maxBoost 6 ⇒ 只测封顶
    p.maxBoostDb = 6.0f;
    drc::DspProcessor proc(fs, ch, p);
    std::vector<float> in = MakeSine(fs, 2500.0, DbToLin(-40.0), frames, ch);
    std::vector<float> buf = in;
    Run(proc, buf, ch, fs);
    const double g = LinToDb(Rms(buf, ch, from, frames) / Rms(in, ch, from, frames));
    Report("T7 up4 maxBoost=6 封顶生效 增益 ≥+2.5dB", g, 2.5, 1e9, " dB");
    Report("T7 up4 maxBoost=6 封顶生效 增益 ≤+6.5dB", g, -1e9, 6.5, " dB");
    Note("  up4 实测增益", g, " dB");
  }

  // up5 ceiling 不变式（病态组合 floor 高于 ceiling 附近的响成分）：峰值仍 ≤ ceiling，
  //      且「开上抬」与「关上抬」两次运行的输出必须显著不同（证明抬升没被限幅全抹平）。
  {
    drc::DspParams pOn;
    pOn.ceilingDb = -6.0f;
    pOn.floorDb = -20.0f;
    pOn.maxBoostDb = 12.0f;
    drc::DspParams pOff = pOn;
    pOff.maxBoostDb = 0.0f;

    std::vector<float> in = MakeSine(fs, 100.0, 1.0, frames, ch);   // 响的一根放在两段之外
    std::vector<float> low = MakeSine(fs, 2500.0, DbToLin(-40.0), frames, ch);
    for (std::size_t i = 0; i < frames; ++i) in[i] += low[i];

    drc::DspProcessor on(fs, ch, pOn);
    drc::DspProcessor off(fs, ch, pOff);
    std::vector<float> a = in, b = in;
    Run(on, a, ch, fs);
    Run(off, b, ch, fs);

    const double peakDb = LinToDb(Peak(a, ch, from, frames));
    Report("T7 up5 混合信号输出峰值 ≤ ceiling+0.1dB", peakDb, -1e9,
           pOn.ceilingDb + 0.1, " dBFS");

    double maxdiff = 0.0;
    for (std::size_t i = from; i < frames; ++i)
      maxdiff = std::max(maxdiff, std::fabs(static_cast<double>(a[i]) -
                                            static_cast<double>(b[i])));
    Report("T7 up5 开/关上抬输出差异 >0.01（抬升真实发生）", maxdiff, 0.01, 1e9, "");
  }

  // up6 旁路 bit-exact：先跑一段带抬升的信号（段滤波器已有状态），再切 bypass
  {
    drc::DspParams p;
    p.floorDb = -30.0f;
    drc::DspProcessor proc(fs, ch, p);
    const std::size_t n = static_cast<std::size_t>(fs) / 4;
    std::vector<float> warm = MakeSine(fs, 2500.0, DbToLin(-40.0), n, ch);
    proc.Process(warm.data(), n, ch, fs);
    drc::DspParams bp;
    bp.floorDb = -30.0f;
    bp.bypass = true;
    proc.UpdateParams(bp);
    std::vector<float> in = MakeNoise(n, ch);
    std::vector<float> buf = in;
    proc.Process(buf.data(), n, ch, fs);
    ReportTrue("T7 up6 上抬活跃后切 bypass 仍 bit-exact",
               std::memcmp(in.data(), buf.data(), in.size() * sizeof(float)) == 0, "");
  }

  // up7 辅段生效：-40dBFS@500Hz，floor -46 / maxBoost 8 ⇒ 增益 ∈ [+3.0, +8.5]
  {
    drc::DspParams p;
    p.floorDb = -34.0f;   // 探针 -40dBFS ⇒ d = 6dB（辅段 6dB soft-knee 的满量程点）
    p.maxBoostDb = 8.0f;
    drc::DspProcessor proc(fs, ch, p);
    std::vector<float> in = MakeSine(fs, 500.0, DbToLin(-40.0), frames, ch);
    std::vector<float> buf = in;
    Run(proc, buf, ch, fs);
    const double g = LinToDb(Rms(buf, ch, from, frames) / Rms(in, ch, from, frames));
    Report("T7 up7 辅段-40dBFS@500Hz 增益 ≥+3.0dB", g, 3.0, 1e9, " dB");
    Report("T7 up7 辅段-40dBFS@500Hz 增益 ≤+8.5dB", g, -1e9, 8.5, " dB");
    Note("  up7 实测增益", g, " dB");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: dsp_tests <T1|T2|T3|T4|T5|T6>\n");
    return 2;
  }
  const std::string t = argv[1];
  std::printf("== %s ==\n", t.c_str());
  if (t == "T1") {
    TestT1();
  } else if (t == "T2") {
    TestT2();
  } else if (t == "T3") {
    TestT3();
  } else if (t == "T4") {
    TestT4();
  } else if (t == "T5") {
    TestT5();
  } else if (t == "T6") {
    TestT6();
  } else if (t == "T7") {
    TestT7();
  } else {
    std::printf("unknown test: %s\n", t.c_str());
    return 2;
  }
  if (g_fail != 0) {
    std::printf("%s FAIL (%d 项未通过)\n", t.c_str(), g_fail);
    return 1;
  }
  std::printf("%s PASS\n", t.c_str());
  return 0;
}
