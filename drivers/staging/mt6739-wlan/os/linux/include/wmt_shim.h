/* SPDX-License-Identifier: GPL-2.0 */
/* Stand-ins for the vendor WMT/STP exports; CONSYS bring-up is done outside the driver. */
#ifndef _WMT_SHIM_H
#define _WMT_SHIM_H

#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/vmalloc.h>
#include <linux/firmware.h>
#include <linux/kthread.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/device.h>
#include <linux/pm_wakeup.h>
#include <linux/atomic.h>

typedef signed char INT8;
typedef void VOID, *PVOID, **PPVOID;
typedef char *PINT8, **PPINT8;
typedef short INT16, *PINT16, **PPINT16;
typedef int INT32, *PINT32, **PPINT32;
typedef long LONG, *PLONG, **PPLONG;
typedef long long INT64, *PINT64, **PPINT64;
typedef unsigned char UINT8, *PUINT8, **PPUINT8;
typedef unsigned short UINT16, *PUINT16, **PPUINT16;
typedef unsigned int UINT32, *PUINT32, **PPUINT32;
typedef unsigned long ULONG, *PULONG, **PPULONG;
typedef unsigned long long UINT64, *PUINT64, **PPUINT64;
typedef size_t SIZE_T;

typedef int MTK_WCN_BOOL;
#define MTK_WCN_BOOL_FALSE	((MTK_WCN_BOOL)0)
#define MTK_WCN_BOOL_TRUE	((MTK_WCN_BOOL)1)

typedef enum _ENUM_WMTDRV_TYPE_T {
	WMTDRV_TYPE_BT = 0,
	WMTDRV_TYPE_FM = 1,
	WMTDRV_TYPE_GPS = 2,
	WMTDRV_TYPE_WIFI = 3,
	WMTDRV_TYPE_WMT = 4,
	WMTDRV_TYPE_ANT = 5,
	WMTDRV_TYPE_STP = 6,
	WMTDRV_TYPE_SDIO1 = 7,
	WMTDRV_TYPE_SDIO2 = 8,
	WMTDRV_TYPE_LPBK = 9,
	WMTDRV_TYPE_COREDUMP = 10,
	WMTDRV_TYPE_MAX
} ENUM_WMTDRV_TYPE_T, *P_ENUM_WMTDRV_TYPE_T;

typedef enum _ENUM_WMTHWVER_TYPE_T {
	WMTHWVER_E1 = 0x0,
	WMTHWVER_E2 = 0x1,
	WMTHWVER_E3 = 0x2,
	WMTHWVER_E4 = 0x3,
	WMTHWVER_E5 = 0x4,
	WMTHWVER_E6 = 0x5,
	WMTHWVER_E7 = 0x6,
	WMTHWVER_MAX,
	WMTHWVER_INVALID = 0xff
} ENUM_WMTHWVER_TYPE_T, *P_ENUM_WMTHWVER_TYPE_T;

typedef enum _ENUM_WMTMSG_TYPE_T {
	WMTMSG_TYPE_POWER_ON = 0,
	WMTMSG_TYPE_POWER_OFF = 1,
	WMTMSG_TYPE_RESET = 2,
	WMTMSG_TYPE_STP_RDY = 3,
	WMTMSG_TYPE_HW_FUNC_ON = 4,
	WMTMSG_TYPE_MAX
} ENUM_WMTMSG_TYPE_T, *P_ENUM_WMTMSG_TYPE_T;

typedef enum _ENUM_WMTRSTMSG_TYPE_T {
	WMTRSTMSG_RESET_START = 0x0,
	WMTRSTMSG_RESET_END = 0x1,
	WMTRSTMSG_RESET_END_FAIL = 0x2,
	WMTRSTMSG_RESET_MAX,
	WMTRSTMSG_RESET_INVALID = 0xff
} ENUM_WMTRSTMSG_TYPE_T, *P_ENUM_WMTRSTMSG_TYPE_T;

typedef INT32 (*wmt_wlan_probe_cb)(VOID);
typedef INT32 (*wmt_wlan_remove_cb)(VOID);
typedef INT32 (*wmt_wlan_bus_cnt_get_cb)(VOID);
typedef INT32 (*wmt_wlan_bus_cnt_clr_cb)(VOID);
typedef INT32 (*wmt_wlan_emi_mpu_set_protection_cb)(bool);

typedef struct _MTK_WCN_WMT_WLAN_CB_INFO {
	wmt_wlan_probe_cb wlan_probe_cb;
	wmt_wlan_remove_cb wlan_remove_cb;
	wmt_wlan_bus_cnt_get_cb wlan_bus_cnt_get_cb;
	wmt_wlan_bus_cnt_clr_cb wlan_bus_cnt_clr_cb;
	wmt_wlan_emi_mpu_set_protection_cb wlan_emi_mpu_set_protection_cb;
} MTK_WCN_WMT_WLAN_CB_INFO, *P_MTK_WCN_WMT_WLAN_CB_INFO;

typedef VOID (*PF_WMT_CB)(ENUM_WMTDRV_TYPE_T, ENUM_WMTDRV_TYPE_T,
			  ENUM_WMTMSG_TYPE_T, PVOID, UINT32);

/* STP debug GPIO indices, used only with CFG_DBG_GPIO_PINS */
#define IDX_TX_THREAD		0
#define IDX_TX_REQ		1
#define IDX_TX_PORT_WRITE	2
#define DBG_TIE_LOW		0
#define DBG_TIE_HIGH		1

INT32 mtk_wcn_wmt_wlan_reg(P_MTK_WCN_WMT_WLAN_CB_INFO pWmtWlanCbInfo);
INT32 mtk_wcn_wmt_wlan_unreg(VOID);
INT32 mtk_wcn_wmt_msgcb_reg(ENUM_WMTDRV_TYPE_T eType, PF_WMT_CB pCb);
INT32 mtk_wcn_wmt_msgcb_unreg(ENUM_WMTDRV_TYPE_T eType);
ENUM_WMTHWVER_TYPE_T mtk_wcn_wmt_hwver_get(VOID);
VOID mtk_wcn_wmt_set_wifi_ver(UINT32 Value);
MTK_WCN_BOOL mtk_wcn_wmt_assert(ENUM_WMTDRV_TYPE_T type, UINT32 reason);
MTK_WCN_BOOL mtk_wcn_wmt_assert_timeout(ENUM_WMTDRV_TYPE_T type, UINT32 reason, INT32 timeout);
MTK_WCN_BOOL mtk_wcn_wmt_assert_keyword(ENUM_WMTDRV_TYPE_T type, PUINT8 keyword);
MTK_WCN_BOOL mtk_wcn_set_connsys_power_off_flag(MTK_WCN_BOOL value);
INT32 mtk_wcn_stp_coredump_start_get(VOID);
INT32 mtk_wcn_stp_debug_gpio_assert(UINT32 dwIndex, UINT32 dwMethod);
INT32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT32 enable);
PUINT8 mtk_wcn_consys_emi_virt_addr_get(UINT32 ctrl_state_offset);
PUINT8 wmt_lib_get_fwinfor_from_emi(UINT8 section, UINT32 offset, PUINT8 buff, UINT32 len);
typedef UINT8 (*file_buf_handler)(PVOID ctx, const signed char __user *buf, UINT16 length);
VOID register_file_buf_handler(file_buf_handler handler, PVOID ctx, UINT8 ucType);
INT32 wmt_set_jtag_for_mcu(VOID);
INT32 wmt_set_jtag_for_gps(VOID);

int wmt_shim_wlan_probe(void);
int wmt_shim_wlan_remove(void);

#endif /* _WMT_SHIM_H */
