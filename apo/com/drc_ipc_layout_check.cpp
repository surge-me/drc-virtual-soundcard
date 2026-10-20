/*
 * drc_ipc_layout_check.cpp — IPC 头布局编译期校验锚点
 *
 * 唯一目的：让 apo/include/drc_ipc.h 里的 static_assert（各结构 = 512 字节 + 逐字段 offsetof）
 * 成为构建门禁的一部分——布局若漂移，DrcApo.dll 直接编译失败。
 */
#include "drc_ipc.h"

namespace {

/* 供链接器可见的最小符号（防止「空 TU」优化告警）；不参与运行。 */
constexpr unsigned kDrcIpcLayoutChecked = 1u;

}  /* namespace */

unsigned drc_ipc_layout_checked() noexcept {
    return kDrcIpcLayoutChecked;
}
