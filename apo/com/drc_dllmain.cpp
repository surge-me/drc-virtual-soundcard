/*
 * drc_dllmain.cpp — DLL 入口、COM 导出、COM 自注册
 *
 * 导出符号（见 DrcApo.def）：DllGetClassObject / DllCanUnloadNow / DllRegisterServer / DllUnregisterServer
 *
 * 注册（DllRegisterServer）只写标准 COM in-proc server 键：
 *   HKCR\CLSID\{CLSID_DrcApo}                     (默认) = 显示名
 *   HKCR\CLSID\{CLSID_DrcApo}\InprocServer32      (默认) = DLL 路径 ; ThreadingModel = Both
 * APO 与音频设备的绑定（FxProperties LFX/GFX 键值）**不在本卡范围**（属 TASK-003 installer）。
 * 「注册手法是否被 Win11/特定设备采用」= unverified（见 devlog 未验证项），本卡只保证壳逻辑 + 编译产物。
 */
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <wchar.h>

#include <new>

#include "drc_apo_guids.h"
#include "drc_apo_log.h"
#include "drc_classfactory.h"
#include "drc_globals.h"

HINSTANCE g_hModule = nullptr;
LONG      g_cObj = 0;
LONG      g_cLock = 0;

namespace drc {

void LogDegrade(const wchar_t* what, unsigned long err) noexcept {
    wchar_t buf[512] = {};
    swprintf_s(buf, L"[DrcApo] DEGRADED: %s (err=%lu)\r\n",
               (what != nullptr ? what : L"(null)"), err);
    /* 仅非音频线程调用；audiodg 附加调试器时可见（音频线程禁阻塞调用） */
    OutputDebugStringW(buf);
}

}  /* namespace drc */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID /*lpvReserved*/) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        /* 诊断（TASK-002B）：证明「引擎到底有没有 LoadLibrary 我们」 */
        drc::ApoLogModule("ATTACH", hinstDLL);
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        /* 诊断：若引擎 Initialize 失败后 FreeLibrary，这里会留下 DETACH 证据 */
        drc::ApoLogModule("DETACH", hinstDLL);
    }
    return TRUE;
}

/* ── COM 导出 ────────────────────────────────────────────────────────── */

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppvObject) {
    char gClsid[40] = {};
    char gRiid[40]  = {};
    if (ppvObject == nullptr) {
        drc::ApoLog("DllGetClassObject ppvObject=null -> E_POINTER");
        return E_POINTER;
    }
    *ppvObject = nullptr;
    if (IsEqualCLSID(rclsid, CLSID_DrcApo) == FALSE) {
        drc::ApoLog("DllGetClassObject clsid=%s -> CLASS_E_CLASSNOTAVAILABLE",
                    drc::GuidToAnsi(rclsid, gClsid, sizeof(gClsid)));
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    drc::DrcClassFactory* factory = new (std::nothrow) drc::DrcClassFactory();
    if (factory == nullptr) {
        drc::ApoLog("DllGetClassObject clsid=%s -> E_OUTOFMEMORY",
                    drc::GuidToAnsi(rclsid, gClsid, sizeof(gClsid)));
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = factory->QueryInterface(riid, ppvObject);
    factory->Release();
    drc::ApoLog("DllGetClassObject clsid=%s riid=%s -> 0x%08lX",
                drc::GuidToAnsi(rclsid, gClsid, sizeof(gClsid)),
                drc::GuidToAnsi(riid, gRiid, sizeof(gRiid)),
                static_cast<unsigned long>(hr));
    return hr;
}

STDAPI DllCanUnloadNow() {
    const HRESULT hr = (g_cObj == 0 && g_cLock == 0) ? S_OK : S_FALSE;
    drc::ApoLog("DllCanUnloadNow g_cObj=%ld g_cLock=%ld -> 0x%08lX",
                static_cast<long>(g_cObj), static_cast<long>(g_cLock),
                static_cast<unsigned long>(hr));
    return hr;
}

/* ── 自注册 ──────────────────────────────────────────────────────────── */

namespace {

LSTATUS SetSzValue(HKEY hKey, const wchar_t* valueName, const wchar_t* value) {
    return RegSetValueExW(hKey, valueName, 0u, REG_SZ,
                          reinterpret_cast<const BYTE*>(value),
                          static_cast<DWORD>((wcslen(value) + 1u) * sizeof(wchar_t)));
}

}  /* namespace */

STDAPI DllRegisterServer() {
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(g_hModule, modulePath, MAX_PATH) == 0u) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    wchar_t clsidKey[128] = {};
    swprintf_s(clsidKey, L"CLSID\\%s", DRC_APO_CLSID_STRING);

    HKEY hKey = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_CLASSES_ROOT, clsidKey, 0u, nullptr,
                                 REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }
    st = SetSzValue(hKey, nullptr, DRC_APO_FRIENDLY_NAME);
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }

    wchar_t inprocKey[192] = {};
    swprintf_s(inprocKey, L"%s\\InprocServer32", clsidKey);
    st = RegCreateKeyExW(HKEY_CLASSES_ROOT, inprocKey, 0u, nullptr,
                         REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }
    st = SetSzValue(hKey, nullptr, modulePath);
    if (st == ERROR_SUCCESS) {
        st = SetSzValue(hKey, L"ThreadingModel", L"Both");
    }
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }
    return S_OK;
}

STDAPI DllUnregisterServer() {
    wchar_t clsidKey[128] = {};
    swprintf_s(clsidKey, L"CLSID\\%s", DRC_APO_CLSID_STRING);

    const LSTATUS st = RegDeleteTreeW(HKEY_CLASSES_ROOT, clsidKey);
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
        return HRESULT_FROM_WIN32(st);
    }
    return S_OK;
}
