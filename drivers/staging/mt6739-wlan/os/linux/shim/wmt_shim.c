// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "mt6739-wlan: " fmt

#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include "wmt_shim.h"

static DEFINE_MUTEX(wmt_shim_lock);
static MTK_WCN_WMT_WLAN_CB_INFO wmt_shim_cb;
static bool wmt_shim_registered;
static bool wmt_shim_probed;

INT32 mtk_wcn_wmt_wlan_reg(P_MTK_WCN_WMT_WLAN_CB_INFO pWmtWlanCbInfo)
{
	mutex_lock(&wmt_shim_lock);
	wmt_shim_cb = *pWmtWlanCbInfo;
	wmt_shim_registered = true;
	mutex_unlock(&wmt_shim_lock);
	return 0;
}

INT32 mtk_wcn_wmt_wlan_unreg(VOID)
{
	mutex_lock(&wmt_shim_lock);
	wmt_shim_registered = false;
	memset(&wmt_shim_cb, 0, sizeof(wmt_shim_cb));
	mutex_unlock(&wmt_shim_lock);
	return 0;
}

int wmt_shim_wlan_probe(void)
{
	int ret = -ENODEV;

	mutex_lock(&wmt_shim_lock);
	if (wmt_shim_probed) {
		ret = 0;
	} else if (wmt_shim_registered && wmt_shim_cb.wlan_probe_cb) {
		ret = wmt_shim_cb.wlan_probe_cb();
		wmt_shim_probed = (ret == 0);
	}
	mutex_unlock(&wmt_shim_lock);
	return ret;
}

int wmt_shim_wlan_remove(void)
{
	int ret = 0;

	mutex_lock(&wmt_shim_lock);
	if (wmt_shim_probed && wmt_shim_cb.wlan_remove_cb) {
		ret = wmt_shim_cb.wlan_remove_cb();
		wmt_shim_probed = false;
	}
	mutex_unlock(&wmt_shim_lock);
	return ret;
}

static int wmt_shim_wlan_on_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret;

	ret = kstrtobool(val, &on);
	if (ret)
		return ret;

	return on ? wmt_shim_wlan_probe() : wmt_shim_wlan_remove();
}

static int wmt_shim_wlan_on_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", wmt_shim_probed);
}

static const struct kernel_param_ops wmt_shim_wlan_on_ops = {
	.set = wmt_shim_wlan_on_set,
	.get = wmt_shim_wlan_on_get,
};
module_param_cb(wlan_on, &wmt_shim_wlan_on_ops, NULL, 0644);
MODULE_PARM_DESC(wlan_on, "Write 1 to probe Wi-Fi after CONSYS bring-up, 0 to remove");

INT32 mtk_wcn_wmt_msgcb_reg(ENUM_WMTDRV_TYPE_T eType, PF_WMT_CB pCb)
{
	return 0;
}

INT32 mtk_wcn_wmt_msgcb_unreg(ENUM_WMTDRV_TYPE_T eType)
{
	return 0;
}

/* CONSYS hw version 0x8B00 maps to E2 in the vendor WMT IC table. */
ENUM_WMTHWVER_TYPE_T mtk_wcn_wmt_hwver_get(VOID)
{
	return WMTHWVER_E2;
}

VOID mtk_wcn_wmt_set_wifi_ver(UINT32 Value)
{
	pr_info("firmware version 0x%x\n", Value);
}

MTK_WCN_BOOL mtk_wcn_wmt_assert(ENUM_WMTDRV_TYPE_T type, UINT32 reason)
{
	pr_err("firmware assert requested (reason 0x%x), not supported\n", reason);
	return MTK_WCN_BOOL_FALSE;
}

MTK_WCN_BOOL mtk_wcn_wmt_assert_timeout(ENUM_WMTDRV_TYPE_T type, UINT32 reason, INT32 timeout)
{
	pr_err("chip reset requested (reason 0x%x), not supported\n", reason);
	return MTK_WCN_BOOL_FALSE;
}

MTK_WCN_BOOL mtk_wcn_wmt_assert_keyword(ENUM_WMTDRV_TYPE_T type, PUINT8 keyword)
{
	pr_err("firmware assert requested: %s\n", keyword);
	return MTK_WCN_BOOL_FALSE;
}

MTK_WCN_BOOL mtk_wcn_set_connsys_power_off_flag(MTK_WCN_BOOL value)
{
	return MTK_WCN_BOOL_TRUE;
}

INT32 mtk_wcn_stp_coredump_start_get(VOID)
{
	return 0;
}

INT32 mtk_wcn_stp_debug_gpio_assert(UINT32 dwIndex, UINT32 dwMethod)
{
	return 0;
}

/* mt6739-consys keeps VCN33_WIFI on while CONSYS is powered. */
INT32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT32 enable)
{
	return 0;
}

PUINT8 mtk_wcn_consys_emi_virt_addr_get(UINT32 ctrl_state_offset)
{
	return NULL;
}

PUINT8 wmt_lib_get_fwinfor_from_emi(UINT8 section, UINT32 offset, PUINT8 buff, UINT32 len)
{
	return NULL;
}

/* NVRAM and config buffers were pushed from userspace through /dev/wmtWifi. */
VOID register_file_buf_handler(file_buf_handler handler, PVOID ctx, UINT8 ucType)
{
}

INT32 wmt_set_jtag_for_mcu(VOID)
{
	return 0;
}

INT32 wmt_set_jtag_for_gps(VOID)
{
	return 0;
}
