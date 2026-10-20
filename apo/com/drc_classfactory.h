/*
 * drc_classfactory.h — DrcApo 的 COM 类工厂（DllGetClassObject 用）
 */
#ifndef DRC_CLASSFACTORY_H
#define DRC_CLASSFACTORY_H

#include <windows.h>
#include <unknwn.h>

#include <atomic>

namespace drc {

class DrcClassFactory final : public IClassFactory
{
public:
    DrcClassFactory() noexcept;
    ~DrcClassFactory();

    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    STDMETHOD(CreateInstance)(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override;
    STDMETHOD(LockServer)(BOOL fLock) override;

private:
    std::atomic<LONG> m_ref{ 1 };
};

}  /* namespace drc */

#endif /* DRC_CLASSFACTORY_H */
