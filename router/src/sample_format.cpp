#include "router/sample_format.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace router {

namespace {

/* KSDATAFORMAT_SUBTYPE_*（本地常量，避免引入 ksmedia.h 重头文件） */
const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

}  /* namespace */

SampleFormat ClassifyWaveFormat(const WAVEFORMATEX* wf) noexcept {
    if (wf == nullptr) {
        return SampleFormat::Unknown;
    }
    switch (wf->wFormatTag) {
        case WAVE_FORMAT_IEEE_FLOAT:
            return (wf->wBitsPerSample == 32u) ? SampleFormat::Float32 : SampleFormat::Unknown;
        case WAVE_FORMAT_PCM:
            if (wf->wBitsPerSample == 16u) return SampleFormat::Int16;
            if (wf->wBitsPerSample == 32u) return SampleFormat::Int32;
            return SampleFormat::Unknown;
        case WAVE_FORMAT_EXTENSIBLE: {
            if (wf->cbSize < (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
                return SampleFormat::Unknown;
            }
            const WAVEFORMATEXTENSIBLE* wfe =
                reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
            /* 仅接受「容器位深 = 有效位深」的全宽整数，拒绝 24-in-32 等左对齐打包 */
            if (wfe->Samples.wValidBitsPerSample != 0u &&
                wfe->Samples.wValidBitsPerSample != wf->wBitsPerSample) {
                return SampleFormat::Unknown;
            }
            if (IsEqualGUID(wfe->SubFormat, kSubtypeIeeeFloat) != FALSE) {
                return (wf->wBitsPerSample == 32u) ? SampleFormat::Float32 : SampleFormat::Unknown;
            }
            if (IsEqualGUID(wfe->SubFormat, kSubtypePcm) != FALSE) {
                if (wf->wBitsPerSample == 16u) return SampleFormat::Int16;
                if (wf->wBitsPerSample == 32u) return SampleFormat::Int32;
            }
            return SampleFormat::Unknown;
        }
        default:
            return SampleFormat::Unknown;
    }
}

const char* SampleFormatName(SampleFormat f) noexcept {
    switch (f) {
        case SampleFormat::Float32: return "float32";
        case SampleFormat::Int16:   return "int16";
        case SampleFormat::Int32:   return "int32";
        default:                    return "unknown";
    }
}

void PcmToFloat(const void* src, SampleFormat fmt, float* dst, std::size_t n) noexcept {
    if (src == nullptr || dst == nullptr) {
        return;
    }
    if (fmt == SampleFormat::Int16) {
        const int16_t* s = static_cast<const int16_t*>(src);
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = static_cast<float>(s[i]) * (1.0f / 32768.0f);
        }
    } else if (fmt == SampleFormat::Int32) {
        const int32_t* s = static_cast<const int32_t*>(src);
        const double k = 1.0 / 2147483648.0;
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = static_cast<float>(static_cast<double>(s[i]) * k);
        }
    } else if (fmt == SampleFormat::Float32) {
        std::memcpy(dst, src, n * sizeof(float));
    }
}

void FloatToPcm(const float* src, SampleFormat fmt, void* dst, std::size_t n) noexcept {
    if (src == nullptr || dst == nullptr) {
        return;
    }
    if (fmt == SampleFormat::Int16) {
        int16_t* d = static_cast<int16_t*>(dst);
        for (std::size_t i = 0; i < n; ++i) {
            float v = src[i] * 32768.0f;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            d[i] = static_cast<int16_t>(std::lrintf(v));
        }
    } else if (fmt == SampleFormat::Int32) {
        int32_t* d = static_cast<int32_t*>(dst);
        for (std::size_t i = 0; i < n; ++i) {
            double v = static_cast<double>(src[i]) * 2147483648.0;
            if (v > 2147483647.0) v = 2147483647.0;
            if (v < -2147483648.0) v = -2147483648.0;
            d[i] = static_cast<int32_t>(std::llrint(v));
        }
    } else if (fmt == SampleFormat::Float32) {
        std::memcpy(dst, src, n * sizeof(float));
    }
}

void MakeFloat32Format(WAVEFORMATEX* out, unsigned channels, unsigned sampleRate) noexcept {
    if (out == nullptr) {
        return;
    }
    std::memset(out, 0, sizeof(*out));
    out->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    out->nChannels = static_cast<WORD>(channels);
    out->nSamplesPerSec = sampleRate;
    out->wBitsPerSample = 32u;
    out->nBlockAlign = static_cast<WORD>(channels * 4u);
    out->nAvgBytesPerSec = sampleRate * out->nBlockAlign;
    out->cbSize = 0u;
}

}  /* namespace router */
