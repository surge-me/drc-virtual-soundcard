/*
 * drc_globals.h — DLL 级全局量 / 降级日志（非音频线程可调）
 */
#ifndef DRC_GLOBALS_H
#define DRC_GLOBALS_H

#include <windows.h>

extern HINSTANCE g_hModule;   /* DllMain 记录 */
extern LONG      g_cObj;      /* 存活 COM 对象数（DllCanUnloadNow 用） */
extern LONG      g_cLock;     /* LockServer 计数 */

namespace drc {

/* 降级可观测通道：非音频线程调用（Initialize / LockForProcess）。
 * 音频线程内**禁止**调用（OutputDebugString 属阻塞式内核调用）。
 * 仅在 audiodg 以调试器附加时可见；同时 devlog 记录降级语义。 */
void LogDegrade(const wchar_t* what, unsigned long err) noexcept;

}  /* namespace drc */

#endif /* DRC_GLOBALS_H */
