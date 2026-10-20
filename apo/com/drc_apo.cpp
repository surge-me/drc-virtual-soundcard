#include "drc_apo.h"

#include <objbase.h>
#include <wchar.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "drc_apo_log.h"
#include "drc_globals.h"

namespace drc {

namespace {

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00AA00389B71}
 * 本地常量避免引入 ksmedia.h（重头文件）。 */
const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

/* KSDATAFORMAT_SUBTYPE_PCM = {00000001-0000-0010-8000-00AA00389B71} */
const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

/* ---- child APO（原厂串联）相关常量 ----
 * PKEY_AudioEndpoint_GUID（mmdeviceapi.h:166，本地定义避免 INITGUID 链接依赖） */
const PROPERTYKEY kPkeyAudioEndpointGuid = {
    { 0x1da5d803, 0xd492, 0x4edd,
      { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 4
};
/* 移交区注册表（与 installer/src/DrcInstaller/Constants.cs 保持一致） */
const wchar_t* kChildAposRoot = L"SOFTWARE\\DrcApo\\ChildApos";
const wchar_t* kChildSlotNames[2] = {
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5",   /* SFX = PKEY_FX_StreamEffectClsid */
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6",   /* MFX = PKEY_FX_ModeEffectClsid   */
};

/* 线性幅度 -> dB（-120dB 以下视为静音底） */
inline float LinearToDb(float lin) noexcept {
    if (!(lin > 1e-6f)) {   /* 同时挡住 NaN */
        return -120.0f;
    }
    return 20.0f * std::log10(lin);
}

/* dB -> x100 幅度整数（SPEC §4 该字段为 uint32，无法承载负值 ⇒ 取「低于满刻度的 dB 幅度」，
 * 即 0 = 0 dBFS、5000 = -50 dBFS。口径已上报 lead / GUI 卡统一）。 */
inline uint32_t DbToX100(float db) noexcept {
    float mag = -db;
    if (!(mag > 0.0f)) {
        mag = 0.0f;
    }
    if (mag > 120.0f) {
        mag = 120.0f;
    }
    return static_cast<uint32_t>(mag * 100.0f + 0.5f);
}

}  /* namespace */

SampleFormat ClassifyWaveFormat(const WAVEFORMATEX* wf) noexcept {
    if (wf == nullptr) {
        return SampleFormat::Unknown;
    }
    switch (wf->wFormatTag) {
        case WAVE_FORMAT_IEEE_FLOAT:
            return (wf->wBitsPerSample == 32u) ? SampleFormat::Float32 : SampleFormat::Unknown;
        case WAVE_FORMAT_PCM:
            if (wf->wBitsPerSample == 16u) { return SampleFormat::Int16; }
            if (wf->wBitsPerSample == 24u) { return SampleFormat::Int24; }   /* 3 字节紧凑 */
            if (wf->wBitsPerSample == 32u) { return SampleFormat::Int32; }
            return SampleFormat::Unknown;
        case WAVE_FORMAT_EXTENSIBLE: {
            if (wf->cbSize < (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
                return SampleFormat::Unknown;
            }
            const WAVEFORMATEXTENSIBLE* wfe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
            const WORD bits = wf->wBitsPerSample;
            const WORD valid = wfe->Samples.wValidBitsPerSample;
            /* 接受:全宽整数(valid==0 或 valid==bits),以及 24-in-32 左对齐
             * (容器 32/有效 24;归一化与 int32 同口径,按 Int32 处理)。 */
            const bool fullWidth = (valid == 0u || valid == bits);
            const bool leftAligned24 = (bits == 32u && valid == 24u);
            if (!fullWidth && !leftAligned24) {
                return SampleFormat::Unknown;
            }
            if (IsEqualGUID(wfe->SubFormat, kSubtypeIeeeFloat) != FALSE) {
                return (bits == 32u) ? SampleFormat::Float32 : SampleFormat::Unknown;
            }
            if (IsEqualGUID(wfe->SubFormat, kSubtypePcm) != FALSE) {
                if (bits == 16u) { return SampleFormat::Int16; }
                if (bits == 24u) { return SampleFormat::Int24; }
                if (bits == 32u) { return SampleFormat::Int32; }
            }
            return SampleFormat::Unknown;
        }
        default:
            return SampleFormat::Unknown;
    }
}

namespace {

/* PCM <-> float32 转换（音频线程调用；无堆分配）。
 * 归一化口径：int16 -> s / 32768；int32 -> s / 2^31（映射到 [-1, 1)）。
 * 反归一化按同一比例并钳位到容器范围（防削顶溢出回绕）。 */
inline void PcmToFloat(const void* src, SampleFormat fmt, float* dst, std::size_t n) noexcept {
    if (fmt == SampleFormat::Int16) {
        const int16_t* s = static_cast<const int16_t*>(src);
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = static_cast<float>(s[i]) * (1.0f / 32768.0f);
        }
    } else if (fmt == SampleFormat::Int24) {   /* 3 字节紧凑,小端,符号扩展,/2^23 */
        const uint8_t* s = static_cast<const uint8_t*>(src);
        for (std::size_t i = 0; i < n; ++i, s += 3) {
            int32_t v = static_cast<int32_t>(s[0]) |
                        (static_cast<int32_t>(s[1]) << 8) |
                        (static_cast<int32_t>(s[2]) << 16);
            if (v & 0x00800000) {
                v |= static_cast<int32_t>(0xFF000000u);
            }
            dst[i] = static_cast<float>(v) * (1.0f / 8388608.0f);
        }
    } else {   /* Int32 */
        const int32_t* s = static_cast<const int32_t*>(src);
        const double k = 1.0 / 2147483648.0;
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = static_cast<float>(static_cast<double>(s[i]) * k);
        }
    }
}

inline void FloatToPcm(const float* src, SampleFormat fmt, void* dst, std::size_t n) noexcept {
    if (fmt == SampleFormat::Int16) {
        int16_t* d = static_cast<int16_t*>(dst);
        for (std::size_t i = 0; i < n; ++i) {
            float v = src[i] * 32768.0f;
            if (v > 32767.0f) { v = 32767.0f; }
            if (v < -32768.0f) { v = -32768.0f; }
            d[i] = static_cast<int16_t>(std::lrintf(v));
        }
    } else if (fmt == SampleFormat::Int24) {   /* 3 字节紧凑,小端,钳位到 [-2^23, 2^23-1] */
        uint8_t* d = static_cast<uint8_t*>(dst);
        for (std::size_t i = 0; i < n; ++i, d += 3) {
            double v = static_cast<double>(src[i]) * 8388608.0;
            if (v > 8388607.0) { v = 8388607.0; }
            if (v < -8388608.0) { v = -8388608.0; }
            const int32_t q = static_cast<int32_t>(std::lrint(v));
            d[0] = static_cast<uint8_t>(q & 0xFF);
            d[1] = static_cast<uint8_t>((q >> 8) & 0xFF);
            d[2] = static_cast<uint8_t>((q >> 16) & 0xFF);
        }
    } else {   /* Int32 */
        int32_t* d = static_cast<int32_t*>(dst);
        for (std::size_t i = 0; i < n; ++i) {
            double v = static_cast<double>(src[i]) * 2147483648.0;
            if (v > 2147483647.0) { v = 2147483647.0; }
            if (v < -2147483648.0) { v = -2147483648.0; }
            d[i] = static_cast<int32_t>(std::llrint(v));
        }
    }
}

/* 诊断（TASK-002B）：把连接格式写成人可读的一行，供 LockForProcess 日志。 */
void FormatWaveForLog(const WAVEFORMATEX* wf, char* buf, int n) noexcept {
    if (wf == nullptr) {
        std::snprintf(buf, static_cast<std::size_t>(n), "(null)");
        return;
    }
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wf->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const WAVEFORMATEXTENSIBLE* w = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        char g[40] = {};
        GuidToAnsi(w->SubFormat, g, sizeof(g));
        std::snprintf(buf, static_cast<std::size_t>(n),
                      "EXT(ch=%u,sr=%lu,bits=%u,valid=%u,sub=%s)",
                      static_cast<unsigned>(wf->nChannels),
                      static_cast<unsigned long>(wf->nSamplesPerSec),
                      static_cast<unsigned>(wf->wBitsPerSample),
                      static_cast<unsigned>(w->Samples.wValidBitsPerSample), g);
    } else {
        std::snprintf(buf, static_cast<std::size_t>(n),
                      "tag=0x%04X(ch=%u,sr=%lu,bits=%u)",
                      static_cast<unsigned>(wf->wFormatTag),
                      static_cast<unsigned>(wf->nChannels),
                      static_cast<unsigned long>(wf->nSamplesPerSec),
                      static_cast<unsigned>(wf->wBitsPerSample));
    }
}

}  /* namespace */

const WAVEFORMATEX* WaveFormatOf(IAudioMediaType* fmt) noexcept {
    if (fmt == nullptr) {
        return nullptr;
    }
    /* 借用：不得 CoTaskMemFree */
    return fmt->GetAudioFormat();
}

/* ── 生命周期（COM 聚合：delegating / nondelegating IUnknown）─────────── */

/* nondelegating IUnknown：非聚合时即对象自身 unknown；聚合时由引擎持有并控制生命周期 */
class DrcApo::NDUnknown final : public IUnknown {
public:
    explicit NDUnknown(DrcApo* owner) noexcept : m_owner(owner) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override {
        return m_owner->NonDelegatingQueryInterface(riid, ppvObject);
    }
    STDMETHOD_(ULONG, AddRef)() override { return m_owner->NonDelegatingAddRef(); }
    STDMETHOD_(ULONG, Release)() override { return m_owner->NonDelegatingRelease(); }

private:
    DrcApo* m_owner;
};

DrcApo::DrcApo(IUnknown* pUnkOuter) noexcept {
    InterlockedIncrement(&g_cObj);
    SharedMemory::FillDefaults(&m_lastParams);
    m_pND = new (std::nothrow) NDUnknown(this);
    m_pUnkOuter = (pUnkOuter != nullptr) ? pUnkOuter
                                          : static_cast<IUnknown*>(m_pND);
    if (m_pND == nullptr) {
        m_pUnkOuter = nullptr;
    }
}

void DrcApo::ReleaseChildren() noexcept {
    for (int i = 0; i < kMaxChildApos; ++i) {
        ChildApo& c = m_children[i];
        if (c.rt != nullptr) { c.rt->Release(); }
        if (c.cfg != nullptr) { c.cfg->Release(); }
        if (c.apo != nullptr) { c.apo->Release(); }
        c = ChildApo{};
    }
    m_childCount = 0;
    m_childScratch.clear();
    m_childScratch.shrink_to_fit();
}

void DrcApo::LoadChildren(UINT32 cbDataSize, BYTE* pbyData) noexcept {
    ReleaseChildren();

    /* 需要 APOInitSystemEffects（pAPOEndpointProperties → 设备 GUID）。
     * 引擎实测传 APOInitSystemEffects3（cbDataSize=80，TASK-002B 日志）；
     * 前缀成员（APOInit + pAPOEndpointProperties）跨版本布局稳定，按 >= 校验。 */
    if (pbyData == nullptr || cbDataSize < sizeof(APOInitSystemEffects)) {
        ApoLog("child: skip (cbDataSize=%lu need>=%zu)", static_cast<unsigned long>(cbDataSize),
               sizeof(APOInitSystemEffects));
        return;
    }
    const APOInitSystemEffects* init = reinterpret_cast<const APOInitSystemEffects*>(pbyData);
    if (init->pAPOEndpointProperties == nullptr) {
        ApoLog("child: no endpoint property store -> no children");
        return;
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    HRESULT hr = init->pAPOEndpointProperties->GetValue(kPkeyAudioEndpointGuid, &var);
    if (FAILED(hr) || var.vt != VT_LPWSTR || var.pwszVal == nullptr) {
        ApoLog("child: endpoint GUID unavailable (hr=0x%08lX vt=%d) -> no children",
               static_cast<unsigned long>(hr), static_cast<int>(var.vt));
        PropVariantClear(&var);
        return;
    }
    wchar_t sub[192] = {};
    _snwprintf_s(sub, ARRAYSIZE(sub), _TRUNCATE, L"%s\\%s", kChildAposRoot, var.pwszVal);
    PropVariantClear(&var);

    for (int i = 0; i < kMaxChildApos && m_childCount < kMaxChildApos; ++i) {
        wchar_t clsidStr[96] = {};
        DWORD cb = sizeof(clsidStr);
        const LONG rc = RegGetValueW(HKEY_LOCAL_MACHINE, sub, kChildSlotNames[i],
                                     RRF_RT_REG_SZ, nullptr, clsidStr, &cb);
        if (rc != ERROR_SUCCESS || clsidStr[0] == L'\0') {
            continue;   /* 该槽位原本就没有原厂 APO */
        }
        GUID clsid{};
        if (FAILED(CLSIDFromString(clsidStr, &clsid))) {
            ApoLog("child: CLSIDFromString failed for %ls", clsidStr);
            continue;
        }
        ChildApo c{};
        HRESULT hrC = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                       __uuidof(IAudioProcessingObject),
                                       reinterpret_cast<void**>(&c.apo));
        if (FAILED(hrC)) {
            ApoLog("child: CoCreateInstance(%ls) -> 0x%08lX (skip)",
                   clsidStr, static_cast<unsigned long>(hrC));
            continue;
        }
        hrC = c.apo->QueryInterface(__uuidof(IAudioProcessingObjectRT),
                                    reinterpret_cast<void**>(&c.rt));
        if (FAILED(hrC)) {
            ApoLog("child: QI(RT) -> 0x%08lX (skip)", static_cast<unsigned long>(hrC));
            c.apo->Release();
            continue;
        }
        hrC = c.apo->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration),
                                    reinterpret_cast<void**>(&c.cfg));
        if (FAILED(hrC)) {
            ApoLog("child: QI(Config) -> 0x%08lX (skip)", static_cast<unsigned long>(hrC));
            c.rt->Release();
            c.apo->Release();
            continue;
        }
        hrC = c.apo->Initialize(cbDataSize, pbyData);   /* EQAPO 同款：init 结构原样转发 */
        if (FAILED(hrC)) {
            ApoLog("child: Initialize(%ls) -> 0x%08lX (skip)",
                   clsidStr, static_cast<unsigned long>(hrC));
            c.cfg->Release();
            c.rt->Release();
            c.apo->Release();
            continue;
        }
        m_children[m_childCount++] = c;
        ApoLog("child[%d] %ls ready (factory APO chained)", m_childCount - 1, clsidStr);
    }
    ApoLog("child: loaded %d child APO(s)", m_childCount);
}

DrcApo::~DrcApo() {
    m_locked = false;
    ReleaseChildren();
    m_shm.Close();
    /* m_pND 不在此删除:NDRelease 归零时可能正处于 NDUnknown::Release 调用栈内,
     * 此处 delete 会释放正在执行成员的对象(use-after-free,实测 audiodg 崩溃)。
     * 由 NonDelegatingRelease 统一收尾。 */
    InterlockedDecrement(&g_cObj);
}

/* ── delegating IUnknown（全部接口路径经此）──────────────────────────── */

HRESULT DrcApo::QueryInterface(REFIID riid, void** ppvObject) {
    if (m_pUnkOuter == nullptr) {
        return E_UNEXPECTED;
    }
    return m_pUnkOuter->QueryInterface(riid, ppvObject);
}

ULONG DrcApo::AddRef() {
    return m_pUnkOuter->AddRef();
}

ULONG DrcApo::Release() {
    return m_pUnkOuter->Release();
}

/* ── nondelegating IUnknown ──────────────────────────────────────────── */

HRESULT DrcApo::NonDelegatingQueryInterface(REFIID riid, void** ppvObject) {
    char g[40] = {};
    if (ppvObject == nullptr) {
        return E_POINTER;
    }
    *ppvObject = nullptr;
    if (m_pND == nullptr) {
        return E_OUTOFMEMORY;
    }
    /* 诊断（TASK-002B）：QI 全量打点（含成功），渲染引擎 Initialize 后接口需求 */
    GuidToAnsi(riid, g, sizeof(g));

    /* 实验（TASK-002B P2）：NoExt3=1 时对 Win11 新接口回 E_NOINTERFACE（伪装老式 APO），
     * 判别「引擎新式 APO 路径」是否为弃链原因。 */
    DWORD noExt3 = 0u;
    DWORD cbNx = sizeof(noExt3);
    const bool bNoExt3 =
        RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\DrcApo", "NoExt3",
                     RRF_RT_REG_DWORD, nullptr, &noExt3, &cbNx) == ERROR_SUCCESS &&
        noExt3 == 1u;

    if (riid == __uuidof(IUnknown)) {
        *ppvObject = static_cast<IUnknown*>(m_pND);
    } else if (riid == __uuidof(IAudioProcessingObject)) {
        *ppvObject = static_cast<IAudioProcessingObject*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        *ppvObject = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *ppvObject = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else if (riid == __uuidof(IAudioSystemEffects)) {
        *ppvObject = static_cast<IAudioSystemEffects*>(this);
    } else if (riid == __uuidof(IAudioSystemEffects2)) {
        *ppvObject = static_cast<IAudioSystemEffects2*>(this);
    } else if (riid == __uuidof(IAudioSystemEffects3)) {
        if (bNoExt3) {
            ApoLog("DrcApo::NDQueryInterface riid=%s -> E_NOINTERFACE (NoExt3)", g);
            return E_NOINTERFACE;
        }
        *ppvObject = static_cast<IAudioSystemEffects3*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectNotifications)) {
        if (bNoExt3) {
            ApoLog("DrcApo::NDQueryInterface riid=%s -> E_NOINTERFACE (NoExt3)", g);
            return E_NOINTERFACE;
        }
        *ppvObject = static_cast<IAudioProcessingObjectNotifications*>(this);
    } else {
        ApoLog("DrcApo::NDQueryInterface riid=%s -> E_NOINTERFACE", g);
        return E_NOINTERFACE;
    }

    ApoLog("DrcApo::NDQueryInterface riid=%s -> S_OK", g);
    NonDelegatingAddRef();
    return S_OK;
}

ULONG DrcApo::NonDelegatingAddRef() {
    const LONG v = m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
    return static_cast<ULONG>(v);
}

ULONG DrcApo::NonDelegatingRelease() {
    const LONG v = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (v == 0) {
        /* 先摘下 ND 再删自身,最后删 ND(顺序保证不释放栈内对象) */
        NDUnknown* nd = m_pND;
        m_pND = nullptr;
        delete this;
        delete nd;
        return 0ul;
    }
    return static_cast<ULONG>(v);
}

/* ── IAudioSystemEffects2/3（Win11 引擎契约；非音频线程调用）────────── */

HRESULT DrcApo::GetEffectsList(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE Event) {
    ApoLog("GetEffectsList enter");
    if (pcEffects != nullptr) {
        *pcEffects = 0u;
    }
    if (ppEffectsIds != nullptr) {
        *ppEffectsIds = nullptr;
    }
    if (ppEffectsIds == nullptr || pcEffects == nullptr) {
        return E_POINTER;
    }
    /* 空列表 = 无可通过 Settings 控制的效果（DRC 参数走 IPC，不走系统效果面板） */
    (void)Event;
    return S_OK;
}

HRESULT DrcApo::GetControllableSystemEffectsList(AUDIO_SYSTEMEFFECT** effects,
                                                 UINT* numEffects, HANDLE event) {
    ApoLog("GetControllableSystemEffectsList enter");
    if (numEffects != nullptr) {
        *numEffects = 0u;
    }
    if (effects != nullptr) {
        *effects = nullptr;
    }
    if (effects == nullptr || numEffects == nullptr) {
        return E_POINTER;
    }
    (void)event;
    return S_OK;
}

HRESULT DrcApo::SetAudioSystemEffectState(GUID effectId, AUDIO_SYSTEMEFFECT_STATE state) {
    ApoLog("SetAudioSystemEffectState enter");
    /* 接受任意设置但不改变行为（bypass 由 IPC 参数控制） */
    (void)effectId;
    (void)state;
    return S_OK;
}

HRESULT DrcApo::GetApoNotificationRegistrationInfo(
    APO_NOTIFICATION_DESCRIPTOR** apoNotifications, DWORD* count) {
    ApoLog("GetApoNotificationRegistrationInfo enter");
    if (apoNotifications != nullptr) {
        *apoNotifications = nullptr;   /* 空注册 = 不需要任何通知 */
    }
    if (count != nullptr) {
        *count = 0u;
    }
    if (apoNotifications == nullptr || count == nullptr) {
        return E_POINTER;
    }
    return S_OK;
}

void STDMETHODCALLTYPE DrcApo::HandleNotification(APO_NOTIFICATION* apoNotification) {
    ApoLog("HandleNotification enter type=%d", (int)(apoNotification ? apoNotification->type : -1));
}

/* ── IAudioProcessingObject ──────────────────────────────────────────── */

HRESULT DrcApo::Reset() {
    /* IAudioProcessingObject 的第一个方法：重置内部处理状态（不清参数来源/不动锁） */
    ApoLog("Reset enter");
    for (int i = 0; i < m_childCount; ++i) {
        m_children[i].apo->Reset();   /* 原厂子 APO 状态一并复位 */
    }
    m_dsp.Reset();
    m_framesProcessed = 0u;
    return S_OK;
}

HRESULT DrcApo::Initialize(UINT32 cbDataSize, BYTE* pbyData) {
    /* 诊断（TASK-002B）：引擎传入的可能是 APOInitSystemEffects2/3（cbSize 更大）；
     * 记 cbDataSize + 头部 16 字节（首 4 字节 = APOInitBaseStruct.cbSize），
     * 用于区分「从未加载」与「Initialize 失败被弃用」。 */
    {
        unsigned long h0 = 0ul, h1 = 0ul, h2 = 0ul, h3 = 0ul;
        if (pbyData != nullptr && cbDataSize >= 16u) {
            std::memcpy(&h0, pbyData + 0, 4);
            std::memcpy(&h1, pbyData + 4, 4);
            std::memcpy(&h2, pbyData + 8, 4);
            std::memcpy(&h3, pbyData + 12, 4);
        }
        ApoLog("Initialize cbDataSize=%lu pbyData=%p head[%08lX %08lX %08lX %08lX] -> S_OK",
               static_cast<unsigned long>(cbDataSize), static_cast<const void*>(pbyData),
               h0, h1, h2, h3);
    }

    /* 非音频线程：把资源全部备好（音频线程内零分配/零锁/零阻塞） */
    /* 实验（TASK-002B P2）：NoShm=1 时跳过 shm/refresh（纯 no-op），
     * 判别「Initialize 的副作用」是否为引擎弃链原因。 */
    DWORD noShm = 0u;
    DWORD cbNo = sizeof(noShm);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\DrcApo", "NoShm",
                     RRF_RT_REG_DWORD, nullptr, &noShm, &cbNo) == ERROR_SUCCESS &&
        noShm == 1u) {
        ApoLog("Initialize NOOP-NOSHM (registry switch)");
        return S_OK;
    }
    m_shm.OpenOrCreate();
    ApoLog("Initialize shm ok");
    RefreshParams();
    ApoLog("Initialize refresh ok");
    /* 原厂音效串联（非音频线程；失败/缺席均不挡 DRC） */
    LoadChildren(cbDataSize, pbyData);
    return S_OK;
}

HRESULT DrcApo::GetLatency(HNSTIME* pTime) {
    ApoLog("GetLatency enter");
    if (pTime == nullptr) {
        return E_POINTER;
    }
    float ms = m_lastParams.lookaheadMs;
    if (!(ms > 0.0f)) {
        ms = DRC_DEFAULT_LOOKAHEAD_MS;
    }
    /* HNSTIME 单位 = 100ns；总延迟 = 本 APO lookahead + 原厂子 APO 延迟之和 */
    HNSTIME total = static_cast<HNSTIME>(static_cast<double>(ms) * 10000.0 + 0.5);
    for (int i = 0; i < m_childCount; ++i) {
        HNSTIME t = 0;
        if (SUCCEEDED(m_children[i].apo->GetLatency(&t))) {
            total += t;
        }
    }
    *pTime = total;
    return S_OK;
}

HRESULT DrcApo::GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps) {
    ApoLog("GetRegistrationProperties enter");
    if (ppRegProps == nullptr) {
        return E_POINTER;
    }
    *ppRegProps = nullptr;

    APO_REG_PROPERTIES* props =
        static_cast<APO_REG_PROPERTIES*>(CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES)));
    if (props == nullptr) {
        return E_OUTOFMEMORY;
    }
    ZeroMemory(props, sizeof(APO_REG_PROPERTIES));

    props->clsid = CLSID_DrcApo;
    props->Flags = static_cast<APO_FLAG>(APO_FLAG_INPLACE |
                                         APO_FLAG_FRAMESPERSECOND_MUST_MATCH |
                                         APO_FLAG_BITSPERSAMPLE_MUST_MATCH);
    wcscpy_s(props->szFriendlyName, ARRAYSIZE(props->szFriendlyName), DRC_APO_FRIENDLY_NAME);
    wcscpy_s(props->szCopyrightInfo, ARRAYSIZE(props->szCopyrightInfo), L"DRC project");
    props->u32MajorVersion = 1u;
    props->u32MinorVersion = 0u;
    props->u32MinInputConnections = 1u;
    props->u32MaxInputConnections = 1u;
    props->u32MinOutputConnections = 1u;
    props->u32MaxOutputConnections = 1u;
    props->u32MaxInstances = 0xFFFFFFFFu;
    props->u32NumAPOInterfaces = 1u;
    props->iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObjectRT);

    *ppRegProps = props;
    return S_OK;
}

namespace {

/* 协商判据（IsInputFormatSupported / IsOutputFormatSupported 共用）。
 * 接受集：PCM int16/int24(3B 紧凑)/int32（含 24-in-32 左对齐按 int32 口径）与 IEEE float32
 * （见 ClassifyWaveFormat）。
 * 若引擎提供对侧格式（pOppositeFormat），采样率/通道必须一致——本 APO 的
 * APO_FLAG_FRAMESPERSECOND_MUST_MATCH/BITSPERSAMPLE_MUST_MATCH 承诺 1:1、不重采样、不混音。
 * 返回值语义依 MS Learn（IsInputFormatSupported 参考页）：S_OK = 请求格式被接受；
 * APOERR_FORMAT_NOT_SUPPORTED = 本 APO 对该类型无可用格式（引擎将放弃本 APO）。 */
bool AcceptNegotiatedFormat(const WAVEFORMATEX* wf, const WAVEFORMATEX* opposite) noexcept {
    if (ClassifyWaveFormat(wf) == SampleFormat::Unknown) {
        return false;
    }
    if (opposite != nullptr && ClassifyWaveFormat(opposite) != SampleFormat::Unknown) {
        if (opposite->nSamplesPerSec != wf->nSamplesPerSec ||
            opposite->nChannels != wf->nChannels) {
            return false;
        }
    }
    return true;
}

}  /* namespace */

HRESULT DrcApo::IsInputFormatSupported(IAudioMediaType* pOppositeFormat,
                                       IAudioMediaType* pRequestedInputFormat,
                                       IAudioMediaType** ppSupportedInputFormat) {
    if (ppSupportedInputFormat != nullptr) {
        *ppSupportedInputFormat = nullptr;
    }
    if (pRequestedInputFormat == nullptr) {
        return E_POINTER;
    }
    /* 子链优先（EQAPO 同款）：子 APO 接受 ⇒ 沿用其结论；失败 ⇒ 弃子链、回落自身协商 */
    if (m_childCount > 0) {
        HRESULT hrC = S_OK;
        for (int i = 0; i < m_childCount && SUCCEEDED(hrC); ++i) {
            hrC = m_children[i].apo->IsInputFormatSupported(pOppositeFormat,
                                                            pRequestedInputFormat,
                                                            ppSupportedInputFormat);
        }
        if (SUCCEEDED(hrC)) {
            ApoLog("IsInputFormatSupported -> child chain hr=0x%08lX", static_cast<unsigned long>(hrC));
            return hrC;
        }
        ApoLog("IsInputFormatSupported child chain failed 0x%08lX -> drop children",
               static_cast<unsigned long>(hrC));
        ReleaseChildren();
    }
    if (!AcceptNegotiatedFormat(WaveFormatOf(pRequestedInputFormat),
                                WaveFormatOf(pOppositeFormat))) {
        char w[100] = {};
        char o[100] = {};
        FormatWaveForLog(WaveFormatOf(pRequestedInputFormat), w, sizeof(w));
        if (pOppositeFormat != nullptr) {
            FormatWaveForLog(WaveFormatOf(pOppositeFormat), o, sizeof(o));
        } else {
            std::snprintf(o, sizeof(o), "null");
        }
        ApoLog("IsInputFormatSupported %s opp=%s -> APOERR_FORMAT_NOT_SUPPORTED", w, o);
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    if (ppSupportedInputFormat != nullptr) {
        pRequestedInputFormat->AddRef();
        *ppSupportedInputFormat = pRequestedInputFormat;
    }
    char w[100] = {};
    FormatWaveForLog(WaveFormatOf(pRequestedInputFormat), w, sizeof(w));
    ApoLog("IsInputFormatSupported %s -> S_OK", w);
    return S_OK;
}

HRESULT DrcApo::IsOutputFormatSupported(IAudioMediaType* pOppositeFormat,
                                        IAudioMediaType* pRequestedOutputFormat,
                                        IAudioMediaType** ppSupportedOutputFormat) {
    if (ppSupportedOutputFormat != nullptr) {
        *ppSupportedOutputFormat = nullptr;
    }
    if (pRequestedOutputFormat == nullptr) {
        return E_POINTER;
    }
    if (!AcceptNegotiatedFormat(WaveFormatOf(pRequestedOutputFormat),
                                WaveFormatOf(pOppositeFormat))) {
        char w[100] = {};
        char o[100] = {};
        FormatWaveForLog(WaveFormatOf(pRequestedOutputFormat), w, sizeof(w));
        if (pOppositeFormat != nullptr) {
            FormatWaveForLog(WaveFormatOf(pOppositeFormat), o, sizeof(o));
        } else {
            std::snprintf(o, sizeof(o), "null");
        }
        ApoLog("IsOutputFormatSupported %s opp=%s -> APOERR_FORMAT_NOT_SUPPORTED", w, o);
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    if (ppSupportedOutputFormat != nullptr) {
        pRequestedOutputFormat->AddRef();
        *ppSupportedOutputFormat = pRequestedOutputFormat;
    }
    return S_OK;
}

HRESULT DrcApo::GetInputChannelCount(UINT32* pu32ChannelCount) {
    if (pu32ChannelCount == nullptr) {
        return E_POINTER;
    }
    /* 本 APO 为 1 输入连接；该返回值语义 = 输入连接数。
     * unverified：待 e2e 复核按 audioenginebaseapo.h 注释/官方示例核对（见 devlog 未验证项）。 */
    *pu32ChannelCount = 1u;
    return S_OK;
}

/* ── IAudioProcessingObjectConfiguration ─────────────────────────────── */

HRESULT DrcApo::LockForProcess(UINT32 u32NumInputConnections,
                               APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                               UINT32 u32NumOutputConnections,
                               APO_CONNECTION_DESCRIPTOR** ppOutputConnections) {
    /* 诊断（TASK-002B）：记录 in/out 连接格式 + 返回 hr（各出口统一打点）。 */
    char inFmt[128] = {};
    char outFmt[128] = {};
    const auto done = [&](HRESULT hr) -> HRESULT {
        ApoLog("LockForProcess in[%s] out[%s] -> 0x%08lX", inFmt, outFmt,
               static_cast<unsigned long>(hr));
        return hr;
    };

    if (u32NumInputConnections != 1u || u32NumOutputConnections != 1u ||
        ppInputConnections == nullptr || ppOutputConnections == nullptr ||
        ppInputConnections[0] == nullptr || ppOutputConnections[0] == nullptr) {
        return done(E_INVALIDARG);
    }

    IAudioMediaType* fmtIn = ppInputConnections[0]->pFormat;
    IAudioMediaType* fmtOut = ppOutputConnections[0]->pFormat;
    if (fmtIn == nullptr || fmtOut == nullptr) {
        return done(E_POINTER);
    }
    const WAVEFORMATEX* wfIn = WaveFormatOf(fmtIn);
    const WAVEFORMATEX* wfOut = WaveFormatOf(fmtOut);
    FormatWaveForLog(wfIn, inFmt, sizeof(inFmt));
    FormatWaveForLog(wfOut, outFmt, sizeof(outFmt));
    const SampleFormat inFmtKind = ClassifyWaveFormat(wfIn);
    const SampleFormat outFmtKind = ClassifyWaveFormat(wfOut);
    if (inFmtKind == SampleFormat::Unknown || outFmtKind == SampleFormat::Unknown) {
        return done(APOERR_FORMAT_NOT_SUPPORTED);
    }
    /* APO_FLAG_FRAMESPERSECOND_MUST_MATCH | BITSPERSAMPLE_MUST_MATCH：引擎保证 in/out
     * 采样率与位深一致、1:1、不重采样不混音；此处防御性校验（不一致 ⇒ 拒绝，宁缺毋滥）。 */
    if (wfIn->nSamplesPerSec != wfOut->nSamplesPerSec ||
        wfIn->nChannels != wfOut->nChannels) {
        return done(APOERR_FORMAT_NOT_SUPPORTED);
    }
    const int channels = static_cast<int>(wfIn->nChannels);
    const int sampleRate = static_cast<int>(wfIn->nSamplesPerSec);
    if (channels <= 0 || sampleRate <= 0) {
        return done(APOERR_FORMAT_NOT_SUPPORTED);
    }

    m_inFormat = inFmtKind;
    m_outFormat = outFmtKind;
    m_channels = channels;
    m_sampleRate = sampleRate;
    m_maxFrameCount = ppInputConnections[0]->u32MaxFrameCount;

    /* 转换垫片：dsp 热路径固定 interleaved float32。非 float32 连接 ⇒ 预分配
     * （非音频线程，保证音频线程零分配；float32 连接保持零拷贝）。 */
    const std::size_t maxSamples =
        static_cast<std::size_t>(m_maxFrameCount) * static_cast<std::size_t>(channels);
    try {
        if (inFmtKind != SampleFormat::Float32) {
            m_scratchIn.assign(maxSamples, 0.0f);
        }
        if (outFmtKind != SampleFormat::Float32) {
            m_scratchOut.assign(maxSamples, 0.0f);
        }
    } catch (...) {
        return done(E_OUTOFMEMORY);   /* 分配失败 ⇒ 显式失败（不静默降级为直通） */
    }

    /* 资源就绪（幂等）：共享内存 + dsp 状态，全部发生在音频线程启动之前 */
    m_shm.OpenOrCreate();
    RefreshParams();
    m_dsp.Init(m_channels, m_sampleRate);

    /* 子 APO 链：预分配级间垫片（音频线程零分配）+ 转发锁定。
     * 垫片按连接原生格式（nBlockAlign 字节/帧）⇒ 子 APO 看到的与引擎连接完全同构。 */
    if (m_childCount > 0) {
        m_childBlockAlign = (wfIn->nBlockAlign != 0) ? wfIn->nBlockAlign : 1;
        const std::size_t childBytes =
            static_cast<std::size_t>(m_maxFrameCount != 0u ? m_maxFrameCount : 1u) *
            static_cast<std::size_t>(m_childBlockAlign);
        try {
            m_childScratch.assign(childBytes, 0);
        } catch (...) {
            ApoLog("child: scratch alloc (%zu B) failed -> drop children", childBytes);
            ReleaseChildren();
        }
    }
    if (m_childCount > 0) {
        for (int i = 0; i < m_childCount; ++i) {
            HRESULT hrC = m_children[i].cfg->LockForProcess(
                u32NumInputConnections, ppInputConnections,
                u32NumOutputConnections, ppOutputConnections);
            if (FAILED(hrC)) {
                ApoLog("child[%d] LockForProcess -> 0x%08lX -> drop children (DRC continues alone)",
                       i, static_cast<unsigned long>(hrC));
                ReleaseChildren();
                break;
            }
        }
        ApoLog("child chain locked: %d APO(s)", m_childCount);
    }

    m_framesProcessed = 0u;
    m_locked = true;
    PublishMeterHeader();
    return done(S_OK);
}

HRESULT DrcApo::UnlockForProcess() {
    ApoLog("UnlockForProcess enter");
    for (int i = 0; i < m_childCount; ++i) {
        m_children[i].cfg->UnlockForProcess();   /* 失败不阻断（EQAPO 同款只记日志语义） */
    }
    m_locked = false;
    m_dsp.Reset();
    return S_OK;
}

/* ── IAudioProcessingObjectRT（音频线程） ────────────────────────────── */

void DrcApo::APOProcess(UINT32 u32NumInputConnections,
                        APO_CONNECTION_PROPERTY** ppInputConnections,
                        UINT32 u32NumOutputConnections,
                        APO_CONNECTION_PROPERTY** ppOutputConnections) {
    /* 诊断（TASK-002B）：首次 APOProcess 一次性打点（证明音频路径真的跑起来了）。
     * 注意：这是音频线程，日志仅此一次（once 门），之后零开销；写失败静默不影响音频。 */
    if (!m_firstProcessLogged.exchange(true, std::memory_order_acq_rel)) {
        ApoLog("APOProcess(first) locked=%d in=%lu out=%lu sr=%d ch=%d",
               m_locked ? 1 : 0,
               static_cast<unsigned long>(u32NumInputConnections),
               static_cast<unsigned long>(u32NumOutputConnections),
               m_sampleRate, m_channels);
    }

    if (!m_locked || u32NumInputConnections < 1u || u32NumOutputConnections < 1u ||
        ppInputConnections == nullptr || ppOutputConnections == nullptr ||
        ppInputConnections[0] == nullptr || ppOutputConnections[0] == nullptr) {
        return;
    }

    APO_CONNECTION_PROPERTY* in = ppInputConnections[0];
    APO_CONNECTION_PROPERTY* out = ppOutputConnections[0];

    UINT32 frames = in->u32ValidFrameCount;
    if (out->u32ValidFrameCount < frames) {
        frames = out->u32ValidFrameCount;
    }
    out->u32ValidFrameCount = frames;

    if (in->u32BufferFlags == BUFFER_SILENT) {
        out->u32BufferFlags = BUFFER_SILENT;
        m_framesProcessed += frames;
        /* 心跳：静音块也回写 meter（峰值=静音），否则静音 >3s 时 GUI 心跳超时误判「未接管」 */
        WriteMeterHeartbeat();
        return;
    }

    if (in->pBuffer == 0u || out->pBuffer == 0u || m_channels <= 0) {
        out->u32BufferFlags = BUFFER_INVALID;
        return;
    }

    const std::size_t samples =
        static_cast<std::size_t>(frames) * static_cast<std::size_t>(m_channels);

    /* ① 原厂子 APO 链先行（顺序 = SFX → MFX；级间经预分配垫片，音频线程零分配）。
     * 子链输出统一落在 m_childScratch（连接原生格式），随后作为本 APO 的输入。 */
    const BYTE* srcBytes = nullptr;
    if (m_childCount > 0) {
        /* 原生缓冲字节数 = 帧数 × nBlockAlign（块对齐已含通道与位深） */
        const std::size_t needBytes = static_cast<std::size_t>(frames) *
            static_cast<std::size_t>(m_childBlockAlign != 0 ? m_childBlockAlign : 1);
        if (m_childScratch.size() < needBytes) {
            if (!m_childScratchWarned.exchange(true, std::memory_order_acq_rel)) {
                ApoLog("APOProcess child scratch too small (%zu < %zu) -> skip children (warn once)",
                       m_childScratch.size(), needBytes);
            }
            srcBytes = reinterpret_cast<const BYTE*>(in->pBuffer);
        } else {
            APO_CONNECTION_PROPERTY link{};
            link.pBuffer = reinterpret_cast<UINT_PTR>(m_childScratch.data());
            link.u32ValidFrameCount = in->u32ValidFrameCount;
            link.u32BufferFlags = in->u32BufferFlags;
            link.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;

            APO_CONNECTION_PROPERTY* linkProps[1] = { &link };
            m_children[0].rt->APOProcess(u32NumInputConnections, ppInputConnections, 1u, linkProps);
            for (int i = 1; i < m_childCount; ++i) {
                APO_CONNECTION_PROPERTY src = link;   /* scratch → scratch（in-place） */
                APO_CONNECTION_PROPERTY* srcProps[1] = { &src };
                m_children[i].rt->APOProcess(1u, srcProps, 1u, linkProps);
            }

            if (link.u32BufferFlags == BUFFER_SILENT ||
                link.u32ValidFrameCount == 0u) {
                out->u32BufferFlags = BUFFER_SILENT;
                out->u32ValidFrameCount = frames;
                m_framesProcessed += frames;
                WriteMeterHeartbeat();   /* 同上：静音心跳 */
                return;
            }
            srcBytes = m_childScratch.data();
        }
    } else {
        srcBytes = reinterpret_cast<const BYTE*>(in->pBuffer);
    }

    /* 输入侧 → float32 工作缓冲（非 float32 连接用预分配垫片；无堆分配） */
    const float* pin = nullptr;
    if (m_inFormat == SampleFormat::Float32) {
        pin = reinterpret_cast<const float*>(srcBytes);
    } else {
        if (m_scratchIn.size() < samples) {
            out->u32BufferFlags = BUFFER_INVALID;
            return;
        }
        PcmToFloat(reinterpret_cast<const void*>(srcBytes), m_inFormat,
                   m_scratchIn.data(), samples);
        pin = m_scratchIn.data();
    }

    /* float32 工作输出缓冲 */
    float* pwork = nullptr;
    if (m_outFormat == SampleFormat::Float32) {
        pwork = reinterpret_cast<float*>(out->pBuffer);
    } else {
        if (m_scratchOut.size() < samples) {
            out->u32BufferFlags = BUFFER_INVALID;
            return;
        }
        pwork = m_scratchOut.data();
    }

    if (pwork != pin) {
        std::memcpy(pwork, pin, samples * sizeof(float));
    }

    /* 每处理块开头读参数（seqlock，SPEC §4） */
    DrcParams p{};
    m_shm.ReadParams(&p);

    const bool bypass = (p.flags & DRC_FLAG_BYPASS) != 0u;
    if (m_shm.Ok() && !bypass) {
        m_dsp.Process(pwork, static_cast<std::size_t>(frames), m_channels, m_sampleRate,
                      MakeRuntimeParams(p));
    }

    /* float32 工作缓冲 → 输出（非 float32 连接） */
    if (m_outFormat != SampleFormat::Float32) {
        FloatToPcm(pwork, m_outFormat, reinterpret_cast<void*>(out->pBuffer), samples);
    }

    out->u32BufferFlags = BUFFER_VALID;
    UpdateMeter(pin, pwork, frames);
}

UINT32 DrcApo::CalcInputFrames(UINT32 u32OutputFrameCount) {
    ApoLog("CalcInputFrames %lu", static_cast<unsigned long>(u32OutputFrameCount));
    return u32OutputFrameCount;   /* 1:1 */
}

UINT32 DrcApo::CalcOutputFrames(UINT32 u32InputFrameCount) {
    ApoLog("CalcOutputFrames %lu", static_cast<unsigned long>(u32InputFrameCount));
    return u32InputFrameCount;    /* 1:1 */
}

/* ── 私有 ────────────────────────────────────────────────────────────── */

void DrcApo::RefreshParams() noexcept {
    m_shm.ReadParams(&m_lastParams);
}

void DrcApo::PublishMeterHeader() noexcept {
    if (!m_shm.Ok()) {
        return;
    }
    DrcMeter m{};
    m.magic = DRC_MAGIC;
    m.version = DRC_METER_VERSION;
    m.activeSampleRate = static_cast<uint32_t>(m_sampleRate);
    m.activeChannels = static_cast<uint32_t>(m_channels);
    m.latencyMs_x10 = static_cast<uint32_t>(m_lastParams.lookaheadMs * 10.0f + 0.5f);
    m.framesProcessed = m_framesProcessed;
    m_shm.WriteMeter(m);
}

/* 音频线程：静音块心跳回写（峰值字段保持 0 = 静音口径），维持 GUI 3s 心跳判定 */
void DrcApo::WriteMeterHeartbeat() noexcept {
    if (!m_shm.Ok()) {
        return;
    }
    DrcMeter m{};
    m.magic = DRC_MAGIC;
    m.version = DRC_METER_VERSION;
    m.activeSampleRate = static_cast<uint32_t>(m_sampleRate);
    m.activeChannels = static_cast<uint32_t>(m_channels);
    m.latencyMs_x10 = static_cast<uint32_t>(m_lastParams.lookaheadMs * 10.0f + 0.5f);
    m.framesProcessed = m_framesProcessed;
    m_shm.WriteMeter(m);
}

void DrcApo::UpdateMeter(const float* pin, const float* pout, UINT32 frames) noexcept {
    if (!m_shm.Ok()) {
        return;   /* 降级：不写 meter（显式降级，非静默失败） */
    }

    const std::size_t n =
        static_cast<std::size_t>(frames) * static_cast<std::size_t>(m_channels);
    float inPeak = 0.0f;
    float outPeak = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const float a = std::fabs(pin[i]);
        if (a > inPeak) {
            inPeak = a;
        }
        const float b = std::fabs(pout[i]);
        if (b > outPeak) {
            outPeak = b;
        }
    }
    m_framesProcessed += frames;

    const float inDb = LinearToDb(inPeak);
    const float outDb = LinearToDb(outPeak);
    float reduction = inDb - outDb;
    if (!(reduction > 0.0f)) {
        reduction = 0.0f;
    }

    DrcMeter m{};
    m.magic = DRC_MAGIC;
    m.version = DRC_METER_VERSION;
    m.activeSampleRate = static_cast<uint32_t>(m_sampleRate);
    m.activeChannels = static_cast<uint32_t>(m_channels);
    m.gainReductionDb_x100 = static_cast<uint32_t>(reduction * 100.0f + 0.5f);
    m.inPeakDb_x100 = DbToX100(inDb);
    m.outPeakDb_x100 = DbToX100(outDb);
    m.latencyMs_x10 = static_cast<uint32_t>(m_lastParams.lookaheadMs * 10.0f + 0.5f);
    m.framesProcessed = m_framesProcessed;
    m_shm.WriteMeter(m);
}

DspRuntimeParams DrcApo::MakeRuntimeParams(const DrcParams& p) noexcept {
    DspRuntimeParams r{};
    r.ceilingDb = p.ceilingDb;
    r.floorDb = p.floorDb;
    r.maxBoostDb = p.maxBoostDb;
    r.lookaheadMs = p.lookaheadMs;
    r.releaseMs = p.releaseMs;
    for (int i = 0; i < 10; ++i) {
        r.eqGainDb[i] = p.eqGainDb[i];
    }
    r.bypass = (p.flags & DRC_FLAG_BYPASS) != 0u;
    return r;
}

}  /* namespace drc */
