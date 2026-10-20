/*
 * drc_apo.h — APO COM 对象（微软 APO 接口族最小实现集）
 *
 * 接口族（以 Windows SDK 头 audioenginebaseapo.h 为准，逐签名照抄）：
 *   IAudioProcessingObject              （Initialize / GetLatency / GetRegistrationProperties /
 *                                        IsInputFormatSupported / IsOutputFormatSupported /
 *                                        GetInputChannelCount）
 *   IAudioProcessingObjectConfiguration （LockForProcess / UnlockForProcess）
 *   IAudioProcessingObjectRT            （APOProcess / CalcInputFrames / CalcOutputFrames）
 *   IAudioSystemEffects                 （标记接口，无方法）
 *
 * 音频线程 = APOProcess。该路径内**禁止**堆分配 / 锁 / 阻塞 / 注册表 / IO；
 * 一切资源（共享内存映射、dsp 状态）在 LockForProcess（或更早的 Initialize）前就绪。
 */
#ifndef DRC_APO_H
#define DRC_APO_H

#include <windows.h>
#include <mmreg.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>   /* IAudioSystemEffects3 / IAudioProcessingObjectNotifications（Win11 引擎 QI 必需） */

#include <atomic>
#include <cstdint>
#include <vector>

#include "drc_apo_guids.h"
#include "drc_dsp_adapter.h"
#include "drc_ipc_shared.h"

namespace drc {

/* 连接样本格式（本 APO 接受集）。dsp 热路径固定 interleaved float32；
 * 非 float32 连接在 LockForProcess 建预分配转换垫片（音频线程零分配）。
 * 依据：微软 APO 格式协商契约——引擎逐个提议格式，APO 逐项接受/拒；
 * 返回 APOERR_FORMAT_NOT_SUPPORTED = 本 APO 对该类型无任何可用格式，引擎放弃本 APO。 */
enum class SampleFormat {
    Unknown = 0,
    Float32,
    Int16,
    Int24,   /* 3 字节紧凑 PCM 24-bit(WAVE_FORMAT_PCM 24 或 EXTENSIBLE 容器 24) */
    Int32
};
/* 注:24-in-32 左对齐(EXTENSIBLE 容器 32/有效 24)按 Int32 处理——左对齐值的
 * [-1,1) 归一化口径同为 /2^31,转换逻辑完全等价,无需独立枚举。 */

class DrcApo final
    : public IAudioProcessingObjectRT
    , public IAudioProcessingObjectConfiguration
    , public IAudioProcessingObject
    , public IAudioSystemEffects3
    , public IAudioProcessingObjectNotifications
{
public:
    /* pUnkOuter != null ⇒ 聚合模式：所有接口的 IUnknown 方法委托给控制 unknown。
     * 引擎（audiodg）以聚合方式创建 APO（实测 pUnkOuter!=null，否则 CLASS_E_NOAGGREGATION 拒载）。 */
    explicit DrcApo(IUnknown* pUnkOuter = nullptr) noexcept;
    ~DrcApo();

    /* delegating IUnknown（全部接口路径经此） */
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    /* nondelegating IUnknown（类工厂聚合创建时使用；也是非聚合时的实际实现） */
    HRESULT NonDelegatingQueryInterface(REFIID riid, void** ppvObject);
    ULONG NonDelegatingAddRef();
    ULONG NonDelegatingRelease();

    /* IAudioProcessingObject —— 注意：Reset 是该接口的第一个方法（SDK 头 audioenginebaseapo.h） */
    STDMETHOD(Reset)() override;
    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE* pbyData) override;
    STDMETHOD(GetLatency)(HNSTIME* pTime) override;
    STDMETHOD(GetRegistrationProperties)(APO_REG_PROPERTIES** ppRegProps) override;
    STDMETHOD(IsInputFormatSupported)(IAudioMediaType* pOppositeFormat,
                                      IAudioMediaType* pRequestedInputFormat,
                                      IAudioMediaType** ppSupportedInputFormat) override;
    STDMETHOD(IsOutputFormatSupported)(IAudioMediaType* pOppositeFormat,
                                       IAudioMediaType* pRequestedOutputFormat,
                                       IAudioMediaType** ppSupportedOutputFormat) override;
    STDMETHOD(GetInputChannelCount)(UINT32* pu32ChannelCount) override;

    /* IAudioProcessingObjectConfiguration */
    STDMETHOD(LockForProcess)(UINT32 u32NumInputConnections,
                              APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                              UINT32 u32NumOutputConnections,
                              APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHOD(UnlockForProcess)() override;

    /* IAudioProcessingObjectRT —— 音频线程 */
    STDMETHOD_(void, APOProcess)(UINT32 u32NumInputConnections,
                                 APO_CONNECTION_PROPERTY** ppInputConnections,
                                 UINT32 u32NumOutputConnections,
                                 APO_CONNECTION_PROPERTY** ppOutputConnections) override;
    STDMETHOD_(UINT32, CalcInputFrames)(UINT32 u32OutputFrameCount) override;
    STDMETHOD_(UINT32, CalcOutputFrames)(UINT32 u32InputFrameCount) override;

    /* IAudioSystemEffects2/3（Win11 引擎在聚合后 QI {C58B31CD-…}，缺则弃用 APO） */
    STDMETHOD(GetEffectsList)(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE Event) override;
    STDMETHOD(GetControllableSystemEffectsList)(AUDIO_SYSTEMEFFECT** effects,
                                                UINT* numEffects, HANDLE event) override;
    STDMETHOD(SetAudioSystemEffectState)(GUID effectId,
                                         AUDIO_SYSTEMEFFECT_STATE state) override;

    /* IAudioProcessingObjectNotifications（Win11 24H2 引擎 QI；空注册 = 不需要通知） */
    STDMETHOD(GetApoNotificationRegistrationInfo)(
        APO_NOTIFICATION_DESCRIPTOR** apoNotifications, DWORD* count) override;
    void STDMETHODCALLTYPE HandleNotification(APO_NOTIFICATION* apoNotification) override;

private:
    class NDUnknown;                  /* nondelegating IUnknown 嵌套实现 */
    friend class NDUnknown;
    friend class DrcClassFactory;     /* 聚合创建路径检查 m_pND */

    /* ---- child APO（原厂音效串联，EQAPO 同款）----
     * 安装器把原厂 SFX/MFX CLSID 存 HKLM\SOFTWARE\DrcApo\ChildApos\{设备GUID}；
     * Initialize（非音频线程）读出并 CoCreate，APOProcess 先跑子 APO 再跑 DRC
     * ⇒ 外部引擎只见本 APO（避开 SFX/MFX 与 LFX/GFX 集合级互斥），实际顺序
     *   = 原厂 SFX → 原厂 MFX → DRC（原厂在先，符合需求）。 */
    struct ChildApo {
        IAudioProcessingObject*              apo = nullptr;
        IAudioProcessingObjectRT*            rt  = nullptr;
        IAudioProcessingObjectConfiguration* cfg = nullptr;
    };
    static constexpr int kMaxChildApos = 2;   /* SFX + MFX */

    /* 非音频线程：读 ChildApos 注册值并 CoCreate 子 APO（cbDataSize/pbyData 原样转发）。
     * 任何一步失败 ⇒ 丢弃该子 APO 并继续（原厂缺席不挡 DRC，与 EQAPO 一致返回 S_OK）。 */
    void LoadChildren(UINT32 cbDataSize, BYTE* pbyData) noexcept;
    void ReleaseChildren() noexcept;

    /* 非音频线程：刷新参数快照（读 IPC，缺省用 SPEC §3 默认值） */
    void RefreshParams() noexcept;
    /* 非音频线程：把采样率/通道/延迟写入 meter 头一次 */
    void PublishMeterHeader() noexcept;
    /* 音频线程：静音块心跳回写（防 GUI 心跳超时误判未接管） */
    void WriteMeterHeartbeat() noexcept;
    /* 音频线程：按本块数据更新 meter（峰值 / 增益衰减 / framesProcessed） */
    void UpdateMeter(const float* pin, const float* pout, UINT32 frames) noexcept;
    /* 音频线程：把 IPC 参数快照映射为 dsp 运行期参数 */
    static DspRuntimeParams MakeRuntimeParams(const DrcParams& p) noexcept;

    std::atomic<LONG> m_ref{ 1 };
    std::atomic<bool> m_firstProcessLogged{ false };  /* 诊断：首次 APOProcess 一次性日志 */
    std::atomic<bool> m_childScratchWarned{ false };  /* 诊断：子链垫片不足只警告一次 */
    IUnknown* m_pUnkOuter = nullptr;  /* controlling unknown（聚合 = outer，否则 = ND 自身） */
    NDUnknown* m_pND = nullptr;       /* ctor new / dtor delete */
    bool   m_locked = false;
    int    m_channels = 0;
    int    m_sampleRate = 0;
    UINT32 m_maxFrameCount = 0;
    UINT64 m_framesProcessed = 0;

    SharedMemory m_shm;
    DspChain     m_dsp;
    DrcParams    m_lastParams{};     /* 非音频线程更新，音频线程只读 */

    /* 连接格式（LockForProcess 记录）+ 非 float32 连接的转换垫片（预分配，音频线程只读写） */
    SampleFormat       m_inFormat  = SampleFormat::Unknown;
    SampleFormat       m_outFormat = SampleFormat::Unknown;
    std::vector<float> m_scratchIn;      /* 输入非 float32 时的 float32 工作缓冲 */
    std::vector<float> m_scratchOut;     /* 输出非 float32 时的 float32 工作缓冲 */

    /* child APO 链状态（Initialize 建 / dtor 与锁定失败时释放；音频线程只调用 rt） */
    ChildApo           m_children[kMaxChildApos]{};
    int                m_childCount = 0;
    UINT32             m_childBlockAlign = 0;             /* 连接块对齐字节（子链垫片用） */
    std::vector<BYTE>  m_childScratch;                    /* 子 APO 级间缓冲（预分配，音频线程零分配） */
};

/* 样本格式分类：PCM int16/int32、IEEE float32（含 WAVE_FORMAT_EXTENSIBLE 子格式）；
 * 其余（含 float64/24bit/压缩格式）= Unknown。 */
SampleFormat ClassifyWaveFormat(const WAVEFORMATEX* wf) noexcept;

/* 从 IAudioMediaType 取 WAVEFORMATEX 视图。**借用指针**：归 media type 所有，调用方不得释放。
 * SDK 头签名 = virtual const WAVEFORMATEX *GetAudioFormat(void)（audiomediatype.h）。 */
const WAVEFORMATEX* WaveFormatOf(IAudioMediaType* fmt) noexcept;

}  /* namespace drc */

#endif /* DRC_APO_H */
