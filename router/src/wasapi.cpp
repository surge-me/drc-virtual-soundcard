#include "router/wasapi.h"

#include <functiondiscoverykeys_devpkey.h>
#include <mmsystem.h>   /* waveOutGetNumDevs（独立探针） */
#include <propidl.h>

#include <cstdio>
#include <cstring>
#include <cwctype>

namespace router {

const wchar_t* const kVirtualCableCaptureNeedle = L"CABLE Output";

namespace {

HRESULT g_enumLastHr = S_OK;

REFERENCE_TIME MsToRefTime(double ms) noexcept {
    if (!(ms > 0.0)) {
        ms = 10.0;
    }
    return static_cast<REFERENCE_TIME>(ms * 10000.0 + 0.5);
}

DWORD SharedFlags(bool eventDriven) noexcept {
    DWORD f = 0u;
    if (eventDriven) {
        f |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    }
    return f;
}

HRESULT InitShared(IAudioClient* ac, DWORD flags, REFERENCE_TIME dur,
                   const WAVEFORMATEX* fmt) noexcept {
    if (ac == nullptr || fmt == nullptr) {
        return E_POINTER;
    }
    return ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, fmt, nullptr);
}

/* 用 IAudioClient2 + AUTOCONVERTPCM 请求指定格式（引擎侧转换）。 */
HRESULT InitSharedAutoConvert(IAudioClient* ac, DWORD baseFlags, REFERENCE_TIME dur,
                              const WAVEFORMATEX* fmt) noexcept {
    if (ac == nullptr || fmt == nullptr) {
        return E_POINTER;
    }
    IAudioClient2* ac2 = nullptr;
    HRESULT hr = ac->QueryInterface(__uuidof(IAudioClient2), reinterpret_cast<void**>(&ac2));
    if (FAILED(hr) || ac2 == nullptr) {
        return E_NOINTERFACE;
    }
    const DWORD flags = baseFlags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = ac2->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, fmt, nullptr);
    ac2->Release();
    return hr;
}

/* 把（可能是 WAVEFORMATEXTENSIBLE 的）格式复制进 out（含 cbSize 尾部）。 */
bool CopyFormat(std::vector<BYTE>& out, const WAVEFORMATEX* wf) noexcept {
    if (wf == nullptr) {
        return false;
    }
    const std::size_t n = sizeof(WAVEFORMATEX) + static_cast<std::size_t>(wf->cbSize);
    try {
        out.assign(n, 0u);
    } catch (...) {
        return false;
    }
    std::memcpy(out.data(), wf, n);
    return true;
}

IMMDevice* GetDeviceByFlow(IMMDeviceEnumerator* en, EDataFlow flow,
                           const std::wstring& id) noexcept {
    if (en == nullptr) {
        return nullptr;
    }
    IMMDevice* dev = nullptr;
    HRESULT hr;
    if (id.empty()) {
        hr = en->GetDefaultAudioEndpoint(flow, eConsole, &dev);
    } else {
        hr = en->GetDevice(id.c_str(), &dev);
    }
    if (FAILED(hr)) {
        return nullptr;
    }
    return dev;
}

std::wstring PropString(IPropertyStore* store, REFPROPERTYKEY key) noexcept {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring s;
    if (store != nullptr && SUCCEEDED(store->GetValue(key, &pv))) {
        if (pv.vt == VT_LPWSTR && pv.pwszVal != nullptr) {
            s = pv.pwszVal;
        }
    }
    PropVariantClear(&pv);
    return s;
}

}  /* namespace */

std::string DescribeFormat(const WAVEFORMATEX* wf) {
    char buf[160] = {};
    if (wf == nullptr) {
        return "(null)";
    }
    std::snprintf(buf, sizeof(buf), "%s %luHz %uch blockAlign=%u",
                  SampleFormatName(ClassifyWaveFormat(wf)),
                  static_cast<unsigned long>(wf->nSamplesPerSec),
                  static_cast<unsigned>(wf->nChannels),
                  static_cast<unsigned>(wf->nBlockAlign));
    return std::string(buf);
}

HRESULT EnumLastHr() noexcept { return g_enumLastHr; }

int WinmmWaveOutDeviceCount() noexcept {
    return static_cast<int>(waveOutGetNumDevs());
}

std::vector<EndpointInfo> EnumActiveEndpoints(EDataFlow flow) {
    return EnumEndpoints(flow, DEVICE_STATE_ACTIVE);
}

std::vector<EndpointInfo> EnumEndpoints(EDataFlow flow, DWORD stateMask) {
    std::vector<EndpointInfo> out;
    g_enumLastHr = S_OK;
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&en));
    if (FAILED(hr) || en == nullptr) {
        g_enumLastHr = hr;
        return out;
    }
    IMMDeviceCollection* coll = nullptr;
    hr = en->EnumAudioEndpoints(flow, stateMask, &coll);
    if (FAILED(hr) || coll == nullptr) {
        g_enumLastHr = hr;
        en->Release();
        return out;
    }
    {
        UINT32 count = 0u;
        HRESULT hrc = coll->GetCount(&count);
        if (FAILED(hrc)) {
            g_enumLastHr = hrc;
        }
        if (SUCCEEDED(hrc)) {
            for (UINT32 i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (FAILED(coll->Item(i, &dev)) || dev == nullptr) {
                    continue;
                }
                EndpointInfo info;
                LPWSTR wid = nullptr;
                if (SUCCEEDED(dev->GetId(&wid)) && wid != nullptr) {
                    info.id = wid;
                    CoTaskMemFree(wid);
                }
                IPropertyStore* store = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)) &&
                    store != nullptr) {
                    info.friendlyName = PropString(store, PKEY_Device_FriendlyName);
                    info.deviceDesc = PropString(store, PKEY_Device_DeviceDesc);
                    store->Release();
                }
                dev->Release();
                if (!info.id.empty()) {
                    out.push_back(std::move(info));
                }
            }
        }
        coll->Release();
    }
    en->Release();
    return out;
}

EndpointInfo GetDefaultEndpoint(EDataFlow flow) {
    EndpointInfo info;
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&en));
    if (FAILED(hr) || en == nullptr) {
        return info;
    }
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &dev)) && dev != nullptr) {
        LPWSTR wid = nullptr;
        if (SUCCEEDED(dev->GetId(&wid)) && wid != nullptr) {
            info.id = wid;
            CoTaskMemFree(wid);
        }
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)) && store != nullptr) {
            info.friendlyName = PropString(store, PKEY_Device_FriendlyName);
            info.deviceDesc = PropString(store, PKEY_Device_DeviceDesc);
            store->Release();
        }
        dev->Release();
    }
    en->Release();
    return info;
}

const EndpointInfo* FindEndpointByName(const std::vector<EndpointInfo>& eps,
                                       const std::wstring& needle) {
    if (needle.empty()) {
        return nullptr;
    }
    std::wstring lowerNeedle = needle;
    for (wchar_t& c : lowerNeedle) {
        c = static_cast<wchar_t>(towlower(c));
    }
    for (const EndpointInfo& e : eps) {
        std::wstring hay = e.friendlyName + L" " + e.deviceDesc;
        for (wchar_t& c : hay) {
            c = static_cast<wchar_t>(towlower(c));
        }
        if (hay.find(lowerNeedle) != std::wstring::npos) {
            return &e;
        }
    }
    return nullptr;
}

/* ── WasapiCapture ────────────────────────────────────────────────────── */

void WasapiCapture::ReleaseAll() noexcept {
    Stop();
    if (m_capture != nullptr) {
        m_capture->Release();
        m_capture = nullptr;
    }
    if (m_client != nullptr) {
        m_client->Release();
        m_client = nullptr;
    }
    if (m_event != nullptr) {
        CloseHandle(m_event);
        m_event = nullptr;
    }
    m_format = SampleFormat::Unknown;
    m_sampleRate = 0;
    m_channels = 0;
    m_bufferFrames = 0u;
    m_fmtBytes.clear();
    m_fmtText.clear();
}

WasapiCapture::~WasapiCapture() {
    ReleaseAll();
}

bool WasapiCapture::Open(const std::wstring& deviceId, double bufferMs, bool loopback) noexcept {
    m_error.clear();
    ReleaseAll();   /* 可重复 Open（端点热插拔重连）：旧 client 在端点失效后不可复用 */
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&en));
    if (FAILED(hr) || en == nullptr) {
        m_error = L"CoCreateInstance(MMDeviceEnumerator) 失败";
        return false;
    }
    /* loopback 捕获挂在渲染端点；普通捕获挂捕获端点 */
    IMMDevice* dev = GetDeviceByFlow(en, loopback ? eRender : eCapture, deviceId);
    en->Release();
    if (dev == nullptr) {
        m_error = L"打开音频端点失败";
        return false;
    }
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                       reinterpret_cast<void**>(&m_client));
    dev->Release();
    if (FAILED(hr) || m_client == nullptr) {
        m_error = L"Activate(IAudioClient) 失败";
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(m_client->GetMixFormat(&mix)) || mix == nullptr) {
        m_error = L"GetMixFormat 失败";
        return false;
    }
    const SampleFormat mixFmt = ClassifyWaveFormat(mix);
    WAVEFORMATEX desired{};
    MakeFloat32Format(&desired, mix->nChannels, mix->nSamplesPerSec);

    const REFERENCE_TIME dur = MsToRefTime(bufferMs);
    const DWORD flags = SharedFlags(true) | (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0u);

    bool ok = false;
    const WAVEFORMATEX* chosen = nullptr;
    if (loopback) {
        /* 回环捕获：用混音格式（AUTOCONVERTPCM 与 LOOPBACK 组合兼容性差，不走） */
        if (SUCCEEDED(InitShared(m_client, flags, dur, mix))) {
            chosen = mix;
            ok = true;
        }
    } else if (mixFmt == SampleFormat::Float32) {
        if (SUCCEEDED(InitShared(m_client, flags, dur, mix))) {
            chosen = mix;
            ok = true;
        } else if (SUCCEEDED(InitSharedAutoConvert(m_client, flags, dur, &desired))) {
            chosen = &desired;
            ok = true;
        }
    } else {
        if (SUCCEEDED(InitSharedAutoConvert(m_client, flags, dur, &desired))) {
            chosen = &desired;
            ok = true;
        } else if (SUCCEEDED(InitShared(m_client, flags, dur, mix))) {
            chosen = mix;
            ok = true;
        }
    }

    if (!ok || chosen == nullptr) {
        CoTaskMemFree(mix);
        m_error = L"共享模式 Initialize 失败（格式协商未通过）";
        return false;
    }
    if (!CopyFormat(m_fmtBytes, chosen)) {
        CoTaskMemFree(mix);
        m_error = L"格式副本分配失败";
        return false;
    }
    m_fmtText = DescribeFormat(reinterpret_cast<const WAVEFORMATEX*>(m_fmtBytes.data()));
    m_format = ClassifyWaveFormat(reinterpret_cast<const WAVEFORMATEX*>(m_fmtBytes.data()));
    m_sampleRate = static_cast<int>(
        reinterpret_cast<const WAVEFORMATEX*>(m_fmtBytes.data())->nSamplesPerSec);
    m_channels = static_cast<int>(
        reinterpret_cast<const WAVEFORMATEX*>(m_fmtBytes.data())->nChannels);
    CoTaskMemFree(mix);

    if (m_event == nullptr) {
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_event == nullptr) {
            m_error = L"CreateEvent 失败";
            return false;
        }
    }
    if (FAILED(m_client->SetEventHandle(m_event))) {
        m_error = L"SetEventHandle 失败";
        return false;
    }
    if (FAILED(m_client->GetBufferSize(&m_bufferFrames))) {
        m_error = L"GetBufferSize 失败";
        return false;
    }
    hr = m_client->GetService(__uuidof(IAudioCaptureClient),
                              reinterpret_cast<void**>(&m_capture));
    if (FAILED(hr) || m_capture == nullptr) {
        m_error = L"GetService(IAudioCaptureClient) 失败";
        return false;
    }
    return true;
}

bool WasapiCapture::Start() noexcept {
    if (m_client == nullptr) {
        return false;
    }
    return SUCCEEDED(m_client->Start());
}

void WasapiCapture::Stop() noexcept {
    if (m_client != nullptr) {
        m_client->Stop();
    }
}

bool WasapiCapture::GetNextPacketSize(UINT32* frames) noexcept {
    if (m_capture == nullptr || frames == nullptr) {
        return false;
    }
    return SUCCEEDED(m_capture->GetNextPacketSize(frames));
}

bool WasapiCapture::GetPacket(BYTE** data, UINT32* frames, DWORD* flags) noexcept {
    if (m_capture == nullptr || data == nullptr || frames == nullptr || flags == nullptr) {
        return false;
    }
    return SUCCEEDED(m_capture->GetBuffer(data, frames, flags, nullptr, nullptr));
}

void WasapiCapture::ReleasePacket(UINT32 frames) noexcept {
    if (m_capture != nullptr) {
        m_capture->ReleaseBuffer(frames);
    }
}

/* ── WasapiRender ─────────────────────────────────────────────────────── */

void WasapiRender::ReleaseAll() noexcept {
    Stop();
    if (m_render != nullptr) {
        m_render->Release();
        m_render = nullptr;
    }
    if (m_client != nullptr) {
        m_client->Release();
        m_client = nullptr;
    }
    if (m_event != nullptr) {
        CloseHandle(m_event);
        m_event = nullptr;
    }
    m_format = SampleFormat::Unknown;
    m_sampleRate = 0;
    m_channels = 0;
    m_bufferFrames = 0u;
    m_fmtBytes.clear();
    m_fmtText.clear();
}

WasapiRender::~WasapiRender() {
    ReleaseAll();
}

bool WasapiRender::Open(const std::wstring& deviceId, double bufferMs, int wantRate,
                        int wantChannels) noexcept {
    m_error.clear();
    ReleaseAll();   /* 可重复 Open（端点热插拔重连） */
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&en));
    if (FAILED(hr) || en == nullptr) {
        m_error = L"CoCreateInstance(MMDeviceEnumerator) 失败";
        return false;
    }
    IMMDevice* dev = GetDeviceByFlow(en, eRender, deviceId);
    en->Release();
    if (dev == nullptr) {
        m_error = L"打开渲染端点失败";
        return false;
    }
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                       reinterpret_cast<void**>(&m_client));
    dev->Release();
    if (FAILED(hr) || m_client == nullptr) {
        m_error = L"Activate(IAudioClient) 失败";
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(m_client->GetMixFormat(&mix)) || mix == nullptr) {
        m_error = L"GetMixFormat 失败";
        return false;
    }
    const SampleFormat mixFmt = ClassifyWaveFormat(mix);
    const int rate = (wantRate > 0) ? wantRate : static_cast<int>(mix->nSamplesPerSec);
    const int chans = (wantChannels > 0) ? wantChannels : static_cast<int>(mix->nChannels);
    WAVEFORMATEX desired{};
    MakeFloat32Format(&desired, static_cast<unsigned>(chans), static_cast<unsigned>(rate));

    const REFERENCE_TIME dur = MsToRefTime(bufferMs);
    const DWORD flags = SharedFlags(true);

    bool ok = false;
    const WAVEFORMATEX* chosen = nullptr;
    if (mixFmt == SampleFormat::Float32 &&
        static_cast<int>(mix->nSamplesPerSec) == rate &&
        static_cast<int>(mix->nChannels) == chans) {
        if (SUCCEEDED(InitShared(m_client, flags, dur, mix))) {
            chosen = mix;
            ok = true;
        }
    }
    if (!ok && SUCCEEDED(InitSharedAutoConvert(m_client, flags, dur, &desired))) {
        chosen = &desired;
        ok = true;
    }
    if (!ok && static_cast<int>(mix->nSamplesPerSec) == rate &&
        static_cast<int>(mix->nChannels) == chans &&
        SUCCEEDED(InitShared(m_client, flags, dur, mix))) {
        chosen = mix;
        ok = true;
    }

    if (!ok || chosen == nullptr) {
        CoTaskMemFree(mix);
        m_error = L"共享模式 Initialize 失败（渲染格式协商未通过）";
        return false;
    }
    if (!CopyFormat(m_fmtBytes, chosen)) {
        CoTaskMemFree(mix);
        m_error = L"格式副本分配失败";
        return false;
    }
    m_fmtText = DescribeFormat(reinterpret_cast<const WAVEFORMATEX*>(m_fmtBytes.data()));
    m_format = ClassifyWaveFormat(reinterpret_cast<const WAVEFORMATEX*>(m_fmtBytes.data()));
    m_sampleRate = static_cast<int>(
        reinterpret_cast<const WAVEFORMATEX*>(m_fmtBytes.data())->nSamplesPerSec);
    m_channels = static_cast<int>(
        reinterpret_cast<const WAVEFORMATEX*>(m_fmtBytes.data())->nChannels);
    CoTaskMemFree(mix);

    if (m_event == nullptr) {
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_event == nullptr) {
            m_error = L"CreateEvent 失败";
            return false;
        }
    }
    if (FAILED(m_client->SetEventHandle(m_event))) {
        m_error = L"SetEventHandle 失败";
        return false;
    }
    if (FAILED(m_client->GetBufferSize(&m_bufferFrames))) {
        m_error = L"GetBufferSize 失败";
        return false;
    }
    hr = m_client->GetService(__uuidof(IAudioRenderClient),
                              reinterpret_cast<void**>(&m_render));
    if (FAILED(hr) || m_render == nullptr) {
        m_error = L"GetService(IAudioRenderClient) 失败";
        return false;
    }
    return true;
}

bool WasapiRender::Start() noexcept {
    if (m_client == nullptr) {
        return false;
    }
    return SUCCEEDED(m_client->Start());
}

void WasapiRender::Stop() noexcept {
    if (m_client != nullptr) {
        m_client->Stop();
    }
}

bool WasapiRender::GetPadding(UINT32* padding) noexcept {
    if (m_client == nullptr || padding == nullptr) {
        return false;
    }
    return SUCCEEDED(m_client->GetCurrentPadding(padding));
}

bool WasapiRender::GetBuffer(UINT32 frames, BYTE** data) noexcept {
    if (m_render == nullptr || data == nullptr) {
        return false;
    }
    return SUCCEEDED(m_render->GetBuffer(frames, data));
}

void WasapiRender::ReleaseBuffer(UINT32 frames, DWORD flags) noexcept {
    if (m_render != nullptr) {
        m_render->ReleaseBuffer(frames, flags);
    }
}

}  /* namespace router */
