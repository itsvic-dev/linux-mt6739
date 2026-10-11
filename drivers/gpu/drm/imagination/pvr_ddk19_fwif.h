/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright (c) Imagination Technologies Ltd. */

#ifndef PVR_DDK19_FWIF_H
#define PVR_DDK19_FWIF_H

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/sizes.h>
#include <linux/stddef.h>
#include <linux/types.h>

/* Firmware interface of the DDK 1.9 (m1.9ED4893595) MIPS firmware, as laid out by that firmware. */

#define DDK19_MIPSFW_CODE_NUMPAGES 42
#define DDK19_MIPSFW_DATA_NUMPAGES 17
#define DDK19_MIPSFW_CODE_VIRTUAL_BASE 0xC0000000
#define DDK19_MIPSFW_DATA_VIRTUAL_BASE \
	(DDK19_MIPSFW_CODE_VIRTUAL_BASE + (DDK19_MIPSFW_CODE_NUMPAGES + 2) * SZ_4K)
#define DDK19_MIPSFW_STACK_VIRTUAL_BASE 0xCF600000
#define DDK19_MIPSFW_HEAP_LOG2_SIZE 25
#define DDK19_MIPSFW_PT_SIZE SZ_32K

#define DDK19_MIPSFW_BOOTLDR_REGS_BASE 0
#define DDK19_MIPSFW_BOOTLDR_PT_BASE 1
#define DDK19_MIPSFW_BOOTLDR_STACK 2
#define DDK19_MIPSFW_BOOTLDR_FWINIT 4

#define DDK19_KCCB_NUMCMDS_LOG2 7
#define DDK19_KCCB_CMD_SIZE 144
#define DDK19_FWCCB_NUMCMDS_LOG2 5
#define DDK19_FWCCB_CMD_SIZE 32

#define DDK19_LOG_TYPE_TRACE 0x00000001
#define DDK19_LOG_TYPE_GROUP_MAIN 0x00000002
#define DDK19_LOG_TYPE_GROUP_POW 0x00000200
#define DDK19_LOG_TYPE_GROUP_HWR 0x00000400
#define DDK19_TRACE_BUFFER_SIZE (400 * 30)

#define DDK19_INICFG_HWR_EN BIT(8)

#define DDK19_COMPCHECKS_LAYOUT_VERSION 2
#define DDK19_COMPCHECKS_BVNC_V_LEN_MAX 7

#define DDK19_GPU_UTIL_STATE_IDLE 1

#define DDK19_HWPERF_CTL_SIZE 4608
#define DDK19_HWRINFOBUF_SIZE 1688
#define DDK19_GPU_UTIL_FWCB_SIZE 10288
#define DDK19_GPU_UTIL_FWCB_LAST_WORD 10248

#define DDK19_PDSCODEDATA_BRN_65273_HEAP_BASE 0xA800000000ull
#define DDK19_USCCODE_BRN_65273_HEAP_BASE 0xBA00000000ull
#define DDK19_DOPPLER_HEAP_BASE 0xE800000000ull
#define DDK19_DOPPLER_OVERFLOW_HEAP_BASE 0xE900000000ull
#define DDK19_TDM_TPU_YUV_COEFFS_HEAP_BASE 0xEA00080000ull
#define DDK19_BIF_TILING_HEAP_BASE 0xD000000000ull
#define DDK19_BIF_TILING_HEAP_SIZE 0x0200000000ull

struct ddk19_fwif_ccb_ctl {
	u32 write_offset;
	u32 read_offset;
	u32 wrap_mask;
	u32 cmd_size;
};

struct ddk19_fwif_os_config {
	u32 config_flags;
	u32 power_sync;
	u32 config_flags_ext;
};

struct ddk19_fwif_compchecks_bvnc {
	u32 layout_version;
	u32 v_len_max;
	u64 bnc;
	char v[DDK19_COMPCHECKS_BVNC_V_LEN_MAX + 1];
};

struct ddk19_fwif_compchecks {
	struct ddk19_fwif_compchecks_bvnc hw_bvnc;
	struct ddk19_fwif_compchecks_bvnc fw_bvnc;
	u32 fw_processor_version;
	u32 ddk_version;
	u32 ddk_build;
	u32 build_options;
	u32 updated;
	u32 padding;
};

struct ddk19_fwif_sigbuf_ctl {
	u32 buffer;
	u32 left_size_in_regs;
};

struct ddk19_fwif_init {
	u32 os_config;
	u32 padding0;
	u64 fault_phys_addr;
	u64 pds_exec_base;
	u64 usc_exec_base;
	u64 result_dump_base;
	u64 dpx_control_stream_base;
	u64 rtu_heap_base;
	u64 tdm_tpu_yuv_coeffs_heap_base;
	u32 first_ta;
	u32 first_render;
	u32 framework_after_init;
	u32 disable_filter_hwperf_custom_counter;
	u32 filter_flags;
	u32 kccb_ctl;
	u32 kccb;
	u32 fwccb_ctl;
	u32 fwccb;
	u32 padding1;
	struct ddk19_fwif_sigbuf_ctl sigbuf_ctl[7];
	u32 enable_logging;
	u32 breakpoint_temps;
	u32 breakpoint_shareds;
	u32 hwr_debug_dump_limit;
	u32 bif_tiling_mode;
	u32 padding2;
	struct {
		u64 base;
		u64 len;
		u64 x_stride;
	} bif_tiling_cfg[4];
	u32 runtime_cfg;
	u32 tracebuf_ctl;
	u64 hwperf_filter;
	u32 hwrinfobuf_ctl;
	u32 gpu_util_fwcb_ctl;
	u32 reg_cfg;
	u32 hwperf_ctl;
	u32 align_checks;
	u32 initial_core_clock_speed;
	u32 active_pm_latency_ms;
	u32 firmware_started;
	u32 marker_val;
	u32 firmware_started_timestamp;
	u32 jones_disable_mask;
	u32 padding3;
	struct ddk19_fwif_compchecks compchecks;
	struct {
		u64 dev_vaddr;
		u32 fw_addr;
		u32 padding;
	} coremem_data_store;
	u32 firmware_perf;
	u32 padding4;
	u64 slc3_fence_dev_vaddr;
	u32 t1_stack;
	u32 padding5;
	u8 pdvfs_opp_info[136];
	u32 core_clock_rate;
	u32 workest_fwccb_ctl;
	u32 workest_fwccb;
	u32 gpio_validation_mode;
};

struct ddk19_fwif_runtime_cfg {
	u32 active_pm_latency_ms;
	u32 active_pm_latency_persistent;
	u32 core_clock_speed;
	u32 default_dusts_num_init;
	u32 hwperf_buf;
};

struct ddk19_fwif_assertbuf {
	char path[200];
	char info[200];
	u32 line_num;
};

struct ddk19_fwif_tracebuf {
	u32 log_type;
	u32 pow_state;
	struct {
		u32 trace_pointer;
		u32 trace_buffer_fw_addr;
		u64 trace_buffer_host;
		struct ddk19_fwif_assertbuf assert_buf;
		u32 padding;
	} trace_buf[1];
	u32 hwr_dm_locked_up_count[7];
	u32 hwr_dm_overran_count[7];
	u32 hwr_dm_recovered_count[7];
	u32 hwr_dm_false_detect_count[7];
	u32 hwr_counter;
	u32 cr_poll_addr[1];
	u32 cr_poll_mask[1];
	u32 hwr_state_flags;
	u32 hwr_recovery_flags[7];
	u32 hwperf_r_idx;
	u32 hwperf_w_idx;
	u32 hwperf_wrap_count;
	u32 hwperf_size;
	u32 hwperf_drop_count;
	u32 hwperf_ut;
	u32 first_drop_ordinal;
	u32 last_drop_ordinal;
	u32 interrupt_count[1];
	u32 kccb_cmds_executed;
	u64 start_idle_time;
	u32 pow_mon_energy;
	u32 t1_pcx[16];
	u32 t1_pcx_w_off;
	u32 os_state_flags[1];
	u32 mmu_flush_counter;
	u32 interrupt_time[1];
	u32 padding;
};

#define DDK19_CHECK(type, member, offset) static_assert(offsetof(type, member) == (offset))

static_assert(sizeof(struct ddk19_fwif_ccb_ctl) == 16);
static_assert(sizeof(struct ddk19_fwif_os_config) == 12);
static_assert(sizeof(struct ddk19_fwif_compchecks_bvnc) == 24);
static_assert(sizeof(struct ddk19_fwif_compchecks) == 72);
DDK19_CHECK(struct ddk19_fwif_compchecks, updated, 64);

static_assert(sizeof(struct ddk19_fwif_init) == 608);
DDK19_CHECK(struct ddk19_fwif_init, fault_phys_addr, 8);
DDK19_CHECK(struct ddk19_fwif_init, tdm_tpu_yuv_coeffs_heap_base, 56);
DDK19_CHECK(struct ddk19_fwif_init, filter_flags, 80);
DDK19_CHECK(struct ddk19_fwif_init, kccb_ctl, 84);
DDK19_CHECK(struct ddk19_fwif_init, fwccb, 96);
DDK19_CHECK(struct ddk19_fwif_init, sigbuf_ctl, 104);
DDK19_CHECK(struct ddk19_fwif_init, enable_logging, 160);
DDK19_CHECK(struct ddk19_fwif_init, bif_tiling_mode, 176);
DDK19_CHECK(struct ddk19_fwif_init, bif_tiling_cfg, 184);
DDK19_CHECK(struct ddk19_fwif_init, runtime_cfg, 280);
DDK19_CHECK(struct ddk19_fwif_init, hwperf_filter, 288);
DDK19_CHECK(struct ddk19_fwif_init, hwperf_ctl, 308);
DDK19_CHECK(struct ddk19_fwif_init, firmware_started, 324);
DDK19_CHECK(struct ddk19_fwif_init, jones_disable_mask, 336);
DDK19_CHECK(struct ddk19_fwif_init, compchecks, 344);
DDK19_CHECK(struct ddk19_fwif_init, coremem_data_store, 416);
DDK19_CHECK(struct ddk19_fwif_init, firmware_perf, 432);
DDK19_CHECK(struct ddk19_fwif_init, slc3_fence_dev_vaddr, 440);
DDK19_CHECK(struct ddk19_fwif_init, t1_stack, 448);
DDK19_CHECK(struct ddk19_fwif_init, pdvfs_opp_info, 456);
DDK19_CHECK(struct ddk19_fwif_init, core_clock_rate, 592);
DDK19_CHECK(struct ddk19_fwif_init, gpio_validation_mode, 604);

static_assert(sizeof(struct ddk19_fwif_runtime_cfg) == 20);
static_assert(sizeof(struct ddk19_fwif_assertbuf) == 404);

static_assert(sizeof(struct ddk19_fwif_tracebuf) == 728);
DDK19_CHECK(struct ddk19_fwif_tracebuf, trace_buf[0].trace_buffer_host, 16);
DDK19_CHECK(struct ddk19_fwif_tracebuf, trace_buf[0].assert_buf, 24);
DDK19_CHECK(struct ddk19_fwif_tracebuf, hwr_dm_locked_up_count, 432);
DDK19_CHECK(struct ddk19_fwif_tracebuf, hwr_counter, 544);
DDK19_CHECK(struct ddk19_fwif_tracebuf, hwperf_r_idx, 588);
DDK19_CHECK(struct ddk19_fwif_tracebuf, hwperf_size, 600);
DDK19_CHECK(struct ddk19_fwif_tracebuf, kccb_cmds_executed, 624);
DDK19_CHECK(struct ddk19_fwif_tracebuf, start_idle_time, 632);
DDK19_CHECK(struct ddk19_fwif_tracebuf, pow_mon_energy, 640);
DDK19_CHECK(struct ddk19_fwif_tracebuf, t1_pcx_w_off, 708);
DDK19_CHECK(struct ddk19_fwif_tracebuf, interrupt_time, 720);

#endif /* PVR_DDK19_FWIF_H */
