#include "drc_classfactory.h"

#include <new>

#include "drc_apo.h"
#include "drc_apo_log.h"
#include "drc_globals.h"

namespace drc {

DrcClassFactory::DrcClassFactory() noexcept = default;
DrcClassFactory::~DrcClassFactory() = default;

HRESULT DrcClassFactory::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr) {
        return E_POINTER;
    }
    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
        *ppvObject = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG DrcClassFactory::AddRef() {
    const LONG v = m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
    return static_cast<ULONG>(v);
}

ULONG DrcClassFactory::Release() {
    const LONG v = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (v == 0) {
        delete this;
        return 0ul;
    }
    return static_cast<ULONG>(v);
}

HRESULT DrcClassFactory::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) {
    char g[40] = {};
    if (ppvObject == nullptr) {
        ApoLog("ClassFactory::CreateInstance riid=%s ppvObject=null -> E_POINTER",
               GuidToAnsi(riid, g, sizeof(g)));
        return E_POINTER;
    }
    *ppvObject = nullptr;

    /* 聚合路径（引擎实际使用：pUnkOuter != null）：COM 规则 = riid 必须 IID_IUnknown，
     * 返回 nondelegating IUnknown，生命周期由 controlling unknown 控制。 */
    if (pUnkOuter != nullptr) {
        if (riid != __uuidof(IUnknown)) {
            ApoLog("ClassFactory::CreateInstance(aggregate) riid=%s != IID_IUnknown -> E_NOINTERFACE",
                   GuidToAnsi(riid, g, sizeof(g)));
            return E_NOINTERFACE;
        }
        DrcApo* apo = new (std::nothrow) DrcApo(pUnkOuter);
        if (apo == nullptr || apo->m_pND == nullptr) {
            delete apo;
            ApoLog("ClassFactory::CreateInstance(aggregate) riid=%s -> E_OUTOFMEMORY",
                   GuidToAnsi(riid, g, sizeof(g)));
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = apo->NonDelegatingQueryInterface(riid, ppvObject);
        apo->NonDelegatingRelease();
        ApoLog("ClassFactory::CreateInstance(aggregate) riid=%s -> 0x%08lX",
               GuidToAnsi(riid, g, sizeof(g)), static_cast<unsigned long>(hr));
        return hr;
    }

    DrcApo* apo = new (std::nothrow) DrcApo();
    if (apo == nullptr) {
        ApoLog("ClassFactory::CreateInstance riid=%s -> E_OUTOFMEMORY",
               GuidToAnsi(riid, g, sizeof(g)));
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = apo->QueryInterface(riid, ppvObject);
    apo->Release();
    ApoLog("ClassFactory::CreateInstance riid=%s -> 0x%08lX",
           GuidToAnsi(riid, g, sizeof(g)), static_cast<unsigned long>(hr));
    return hr;
}

HRESULT DrcClassFactory::LockServer(BOOL fLock) {
    if (fLock != FALSE) {
        InterlockedIncrement(&g_cLock);
    } else {
        InterlockedDecrement(&g_cLock);
    }
    return S_OK;
}

}  /* namespace drc */
