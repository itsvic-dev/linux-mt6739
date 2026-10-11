# SPDX-License-Identifier: GPL-2.0

ccflags-y += -DLINUX -DMT6628 -D_HIF_SDIO=1 -DDBG=0
ccflags-y += -DCFG_SRAM_SIZE_OPTION=2
ccflags-y += -DCFG_SUPPORT_WIFI_FW_LOG_UI=0
ccflags-y += -DCFG_SUPPORT_AGPS_ASSIST=0
ccflags-y += -DCFG_SUPPORT_TSF_USING_BOOTTIME=1
ccflags-y += -DCFG_P2P_LEGACY_COEX_REVISE=0
ccflags-y += -DARP_MONITER_ENABLE=1
ccflags-y += -DWMT_IDC_SUPPORT=0
ccflags-y += -DCFG_SUPPORT_WAPI=0
ccflags-y += -DCFG_SUPPORT_MCC=0
ccflags-y += -DCFG_SUPPORT_XLOG=0
ccflags-y += -DCFG_SUPPORT_AEE=0
ccflags-y += -DCFG_SUPPORT_HOTSPOT_2_0=0
ccflags-y += -DCFG_HS20_DEBUG=0
ccflags-y += -DCFG_ENABLE_GTK_FRAME_FILTER=0
ccflags-y += -DCFG_SUPPORT_MET_PROFILING=0
ccflags-y += -DCFG_TC1_FEATURE=0
ccflags-y += -DCFG_TC10_FEATURE=0

ccflags-y += -I$(src)/os -I$(src)/os/linux/include -I$(src)/os/linux/hif/ahb/include
ccflags-y += -I$(src)/include -I$(src)/include/nic -I$(src)/include/mgmt

obj-$(CONFIG_MT6739_WLAN) += mt6739-wlan.o

mt6739-wlan-y := \
	common/dump.o \
	common/wlan_lib.o \
	common/wlan_oid.o \
	common/debug.o \
	nic/nic.o \
	nic/nic_tx.o \
	nic/nic_rx.o \
	nic/nic_pwr_mgt.o \
	nic/cmd_buf.o \
	nic/que_mgt.o \
	nic/nic_cmd_event.o \
	os/linux/gl_init.o \
	os/linux/gl_kal.o \
	os/linux/gl_wext.o \
	os/linux/gl_rst.o \
	os/linux/gl_cfg80211.o \
	os/linux/platform.o \
	os/linux/gl_proc.o \
	os/linux/hif/ahb/arm.o \
	os/linux/hif/ahb/ahb.o \
	os/linux/hif/ahb/mt6739/ahb_pdma.o \
	os/linux/plat/mt6739/plat_priv.o \
	os/linux/shim/wmt_shim.o \
	mgmt/ais_fsm.o \
	mgmt/aaa_fsm.o \
	mgmt/assoc.o \
	mgmt/auth.o \
	mgmt/bss.o \
	mgmt/cnm.o \
	mgmt/cnm_timer.o \
	mgmt/cnm_mem.o \
	mgmt/hem_mbox.o \
	mgmt/mib.o \
	mgmt/privacy.o \
	mgmt/rate.o \
	mgmt/rlm.o \
	mgmt/rlm_domain.o \
	mgmt/rlm_obss.o \
	mgmt/rlm_protection.o \
	mgmt/rsn.o \
	mgmt/saa_fsm.o \
	mgmt/scan.o \
	mgmt/scan_fsm.o \
	mgmt/sec_fsm.o \
	mgmt/swcr.o \
	mgmt/roaming_fsm.o \
	mgmt/hs20.o \
	mgmt/wnm.o \
	mgmt/qosmap.o \
	mgmt/tdls.o \
	mgmt/tdls_com.o \
	mgmt/stats.o \
	mgmt/wmm.o
