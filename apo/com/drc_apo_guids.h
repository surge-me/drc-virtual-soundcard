/*
 * drc_apo_guids.h — APO COM 身份（CLSID / 显示名）冻结点
 *
 * CLSID 由 TASK-002（exec-apo）于 2026-10-15 生成并冻结，**唯一来源**：
 *   {02E42793-63F4-49A3-B547-95B3250D57E4}
 * 已写入 docs/devlogs/2026-10-15-apo-com.md；后续 installer 卡（TASK-003）必须引用同一值，
 * 禁止在别处再生成（防双处漂移）。
 */
#ifndef DRC_APO_GUIDS_H
#define DRC_APO_GUIDS_H

#include <guiddef.h>

/* CLSID 字符串形式（注册表键名用），与下面 CLSID_DrcApo 必须一致 */
#define DRC_APO_CLSID_STRING     L"{02E42793-63F4-49A3-B547-95B3250D57E4}"
#define DRC_APO_FRIENDLY_NAME    L"DRC Dynamic Range Compander APO"

/* CLSID_DrcApo == {02E42793-63F4-49A3-B547-95B3250D57E4} */
inline constexpr GUID CLSID_DrcApo = {
    0x02e42793, 0x63f4, 0x49a3,
    { 0xb5, 0x47, 0x95, 0xb3, 0x25, 0x0d, 0x57, 0xe4 }
};

#endif /* DRC_APO_GUIDS_H */
