/*
 * wasapi.h — WASAPI 端点枚举 / 捕获 / 渲染封装（TASK-006 audio-router）
 *
 * 目标链路：虚拟设备捕获端点（如 VB-CABLE 的 "CABLE Output"）-> dsp-core -> 默认渲染设备。
 * 全部走 **共享模式**（IAudioClient::AUDCLNT_SHAREMODE_SHARED）。
 * 采样格式：优先 float32（IAudioClient2 + AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 让引擎转换），
 * 失败则退回混音格式 + 软件垫片（router/sample_format.h）。
 *
 * 调用方负责 CoInitializeEx（多线程单元）。
 */
#ifndef ROUTER_WASAPI_H
#define ROUTER_WASAPI_H

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cstdint>
#include <string>
#include <vector>

#include "router/sample_format.h"

namespace router {

struct EndpointInfo {
    std::wstring id;
    std::wstring friendlyName;
    std::wstring deviceDesc;
};

/* 枚举某数据流的端点（按枚举器顺序）；stateMask = DEVICE_STATE_* 位组合。 */
std::vector<EndpointInfo> EnumEndpoints(EDataFlow flow, DWORD stateMask);

/* 枚举某数据流的**活动**端点（DEVICE_STATE_ACTIVE）。失败返回空。 */
std::vector<EndpointInfo> EnumActiveEndpoints(EDataFlow flow);

/* 最近一次枚举失败的 HRESULT（0 = 无错误）；诊断用。 */
HRESULT EnumLastHr() noexcept;

/* 独立探针（与 MMDevice 不同 API 路径）：winmm 报告的波形输出设备数。
 * 0 = 本会话/本机无可用音频输出设备（环境级），用于区分「环境缺设备」与「枚举代码缺陷」。 */
int WinmmWaveOutDeviceCount() noexcept;

/* 默认端点（eConsole 角色）；失败返回空 id。 */
EndpointInfo GetDefaultEndpoint(EDataFlow flow);

/* 在端点集中按名找（不区分大小写）；needle 为空返回 nullptr。 */
const EndpointInfo* FindEndpointByName(const std::vector<EndpointInfo>& eps,
                                       const std::wstring& needle);

/* 虚拟声卡捕获端点匹配串（冻结：VB-CABLE 的捕获端名含 "CABLE Output"）。 */
extern const wchar_t* const kVirtualCableCaptureNeedle;

/* 描述一个连接格式（日志用，ASCII）。 */
std::string DescribeFormat(const WAVEFORMATEX* wf);

/* ── 捕获客户端 ───────────────────────────────────────────────────────── */
class WasapiCapture {
public:
    WasapiCapture() noexcept = default;
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    /* deviceId 空 = 默认捕获端点。bufferMs = 引擎缓冲时长（附加延迟预算）。
     * loopback = true 时对**渲染端点**做回环捕获（R4 用；只捕获不回放）。 */
    bool Open(const std::wstring& deviceId, double bufferMs, bool loopback) noexcept;

    bool Start() noexcept;
    void Stop() noexcept;

    /* 释放全部 COM/句柄资源（幂等）。Open() 内部先调用它，故同一个对象可**重复 Open**
     * （端点热插拔重连必需：旧 IAudioClient 在端点失效后不可复用，必须重建）。 */
    void ReleaseAll() noexcept;

    HANDLE Event() const noexcept { return m_event; }

    /* 待取包数（0 = 无数据）。 */
    bool GetNextPacketSize(UINT32* frames) noexcept;

    /* 音频线程：取一包（返回 false = 无数据/错误）。 */
    bool GetPacket(BYTE** data, UINT32* frames, DWORD* flags) noexcept;
    void ReleasePacket(UINT32 frames) noexcept;

    int SampleRate() const noexcept { return m_sampleRate; }
    int Channels() const noexcept { return m_channels; }
    SampleFormat Format() const noexcept { return m_format; }
    UINT32 BufferFrames() const noexcept { return m_bufferFrames; }
    const std::string& NegotiatedFormat() const noexcept { return m_fmtText; }
    const std::wstring& LastError() const noexcept { return m_error; }

private:
    IAudioClient*       m_client = nullptr;
    IAudioCaptureClient* m_capture = nullptr;
    HANDLE              m_event = nullptr;
    SampleFormat        m_format = SampleFormat::Unknown;
    int                 m_sampleRate = 0;
    int                 m_channels = 0;
    UINT32              m_bufferFrames = 0;
    std::vector<BYTE>   m_fmtBytes;   /* 连接格式副本（WAVEFORMATEX 起） */
    std::string         m_fmtText;
    std::wstring        m_error;
};

/* ── 渲染客户端 ───────────────────────────────────────────────────────── */
class WasapiRender {
public:
    WasapiRender() noexcept = default;
    ~WasapiRender();

    WasapiRender(const WasapiRender&) = delete;
    WasapiRender& operator=(const WasapiRender&) = delete;

    /* deviceId 空 = 默认渲染端点。wantRate/wantChannels = 0 表示沿用混音格式的采样率/通道。
     * 尽量协商出 float32 共享模式连接格式（必要时用 AUTOCONVERTPCM 让引擎转换）。 */
    bool Open(const std::wstring& deviceId, double bufferMs, int wantRate,
              int wantChannels) noexcept;

    bool Start() noexcept;
    void Stop() noexcept;

    /* 释放全部 COM/句柄资源（幂等）。Open() 内部先调用它，故同一个对象可**重复 Open**。 */
    void ReleaseAll() noexcept;

    HANDLE Event() const noexcept { return m_event; }

    UINT32 BufferFrames() const noexcept { return m_bufferFrames; }
    bool GetPadding(UINT32* padding) noexcept;
    bool GetBuffer(UINT32 frames, BYTE** data) noexcept;
    void ReleaseBuffer(UINT32 frames, DWORD flags) noexcept;

    int SampleRate() const noexcept { return m_sampleRate; }
    int Channels() const noexcept { return m_channels; }
    SampleFormat Format() const noexcept { return m_format; }
    const std::string& NegotiatedFormat() const noexcept { return m_fmtText; }
    const std::wstring& LastError() const noexcept { return m_error; }

private:
    IAudioClient*       m_client = nullptr;
    IAudioRenderClient* m_render = nullptr;
    HANDLE              m_event = nullptr;
    SampleFormat        m_format = SampleFormat::Unknown;
    int                 m_sampleRate = 0;
    int                 m_channels = 0;
    UINT32              m_bufferFrames = 0;
    std::vector<BYTE>   m_fmtBytes;
    std::string         m_fmtText;
    std::wstring        m_error;
};

}  /* namespace router */

#endif /* ROUTER_WASAPI_H */
