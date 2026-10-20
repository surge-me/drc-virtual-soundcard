/*
 * sample_format.h — 采样格式分类 + PCM<->float32 垫片（TASK-006 audio-router）
 *
 * 目的：dsp-core 热路径固定 interleaved float32（SPEC §1）；WASAPI 共享模式
 * 连接格式可能是 int16/int32/float32 ⇒ 需要无堆分配的格式转换垫片。
 * 转换口径与 SPEC/GUI 一致：int16 -> s/32768，int32 -> s/2^31（映射到 [-1,1)）；
 * 反归一化按同比例 + 钳位（防削顶回绕）。
 *
 * 线程纪律：PcmToFloat/FloatToPcm 在音频线程调用，**不做任何分配**。
 * 代码为本卡自写（思路参考 apo/com 已有实现，不拷贝其源码）。
 */
#ifndef ROUTER_SAMPLE_FORMAT_H
#define ROUTER_SAMPLE_FORMAT_H

#include <windows.h>
#include <mmreg.h>   /* WAVE_FORMAT_* / WAVEFORMATEXTENSIBLE */

#include <cstddef>

namespace router {

enum class SampleFormat : int {
    Unknown = 0,
    Float32 = 1,
    Int16 = 2,
    Int32 = 3
};

/* 分类 WAVEFORMATEX（含 WAVE_FORMAT_EXTENSIBLE 子格式）。
 * 接受：IEEE_FLOAT(32)、PCM(16/32)、EXTENSIBLE(IEEE_FLOAT 32 / PCM 16/32)。
 * 拒绝：24-bit 左对齐打包（wValidBitsPerSample != wBitsPerSample）、其余一切。 */
SampleFormat ClassifyWaveFormat(const WAVEFORMATEX* wf) noexcept;

/* 采样格式的人类可读名（日志/测试用）。 */
const char* SampleFormatName(SampleFormat f) noexcept;

/* 把 n 个样本由 src(fmt) 转成 float32 写入 dst；fmt 必须为 Int16/Int32。 */
void PcmToFloat(const void* src, SampleFormat fmt, float* dst, std::size_t n) noexcept;

/* 把 n 个 float32 样本转回 fmt 写入 dst；fmt 必须为 Int16/Int32。 */
void FloatToPcm(const float* src, SampleFormat fmt, void* dst, std::size_t n) noexcept;

/* 构造一个 float32 WAVEFORMATEX（PCM 风格，nBlockAlign = channels*4）。 */
void MakeFloat32Format(WAVEFORMATEX* out, unsigned channels, unsigned sampleRate) noexcept;

}  /* namespace router */

#endif /* ROUTER_SAMPLE_FORMAT_H */
