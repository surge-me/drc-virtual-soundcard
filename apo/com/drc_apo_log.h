/*
 * drc_apo_log.h — APO 加载/初始化阶段诊断日志（TASK-002B）
 *
 * 目的：e2e 的模块快照无法区分「从未被加载」与「加载后 Initialize/LockForProcess 失败被
 * FreeLibrary」。本日志在**非音频线程**各入口/出口留痕，供 audiodg 实测定位失败点：
 *   DllMain(ATTACH/DETACH) · DllGetClassObject · DllCanUnloadNow · ClassFactory::CreateInstance
 *   · QueryInterface · Initialize · LockForProcess（+ 首次 APOProcess，一次性）。
 *
 * 目标文件：C:\Windows\Temp\DrcApo_log.txt
 *   （可用环境变量 DRC_APO_LOG_PATH 覆写，仅供宿主自测保持日志隔离；audiodg 无此变量 ⇒ 默认路径）
 * 写入：每行一次 WriteFile 原子追加（OVERLAPPED 偏移 0xFFFFFFFF），Interlocked 单写者门。
 * 失败静默：建文件/写失败一律吞掉，不抛、不阻塞、不影响音频路径。
 * 编译开关：DRC_APO_LOG（apo/CMakeLists.txt 默认 =1；=0 时全部展开为空操作）。
 */
#ifndef DRC_APO_LOG_H
#define DRC_APO_LOG_H

#include <windows.h>

#ifndef DRC_APO_LOG
#define DRC_APO_LOG 1
#endif

namespace drc {

/* DllMain 用：记录模块路径/实例（what = "ATTACH"/"DETACH"）。 */
void ApoLogModule(const char* what, HINSTANCE hinst) noexcept;

/* printf 风格；自动加 [时间][pid][tid] 前缀 + CRLF。 */
void ApoLog(const char* fmt, ...) noexcept;

/* GUID -> "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"；返回 buf（buf 至少 40 字节）。 */
const char* GuidToAnsi(const GUID& g, char* buf, int bufLen) noexcept;

}  /* namespace drc */

#endif /* DRC_APO_LOG_H */
