// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include "pvr_ddk19.h"
#include "pvr_ddk19_fwif.h"
#include "pvr_device.h"
#include "pvr_fw.h"
#include "pvr_fw_info.h"
#include "pvr_fw_mips.h"
#include "pvr_fw_startstop.h"
#include "pvr_gem.h"
#include "pvr_power.h"
#include "pvr_rogue_cr_defs.h"
#include "pvr_rogue_heap_config.h"
#include "pvr_rogue_mips.h"
#include "pvr_vm_mips.h"

#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define DDK19_CORE_CLOCK_SPEED 570000000
#define DDK19_ACTIVE_PM_LATENCY_MS 50
#define DDK19_BOOT_TIMEOUT_US 1000000

struct pvr_ddk19 {
	void __iomem *mfg;

	struct pvr_fw_object *init_obj;
	struct ddk19_fwif_init *init;
	struct pvr_fw_object *os_config_obj;
	struct pvr_fw_object *power_sync_obj;
	struct pvr_fw_object *kccb_ctl_obj;
	struct pvr_fw_object *kccb_obj;
	struct pvr_fw_object *fwccb_ctl_obj;
	struct pvr_fw_object *fwccb_obj;
	struct pvr_fw_object *tracebuf_obj;
	struct ddk19_fwif_tracebuf *tracebuf;
	struct pvr_fw_object *trace_obj;
	u32 *trace;
	struct pvr_fw_object *hwrinfobuf_obj;
	struct pvr_fw_object *gpu_util_obj;
	struct pvr_fw_object *runtime_cfg_obj;
	struct pvr_fw_object *hwperf_ctl_obj;

	u32 *fault_page;
	dma_addr_t fault_page_dma;
};

static const struct pvr_fw_layout_entry ddk19_layout[] = {
	{ MIPS_CODE, FW_CODE, DDK19_MIPSFW_CODE_VIRTUAL_BASE,
	  DDK19_MIPSFW_CODE_NUMPAGES * SZ_4K, DDK19_MIPSFW_CODE_NUMPAGES * SZ_4K, 0 },
	{ MIPS_EXCEPTIONS_CODE, FW_CODE, ROGUE_MIPSFW_EXCEPTIONS_VIRTUAL_BASE,
	  SZ_4K, SZ_4K, DDK19_MIPSFW_CODE_NUMPAGES * SZ_4K },
	{ MIPS_BOOT_CODE, FW_CODE, ROGUE_MIPSFW_BOOT_NMI_CODE_VIRTUAL_BASE,
	  SZ_4K, SZ_4K, (DDK19_MIPSFW_CODE_NUMPAGES + 1) * SZ_4K },
	{ MIPS_PRIVATE_DATA, FW_DATA, DDK19_MIPSFW_DATA_VIRTUAL_BASE,
	  DDK19_MIPSFW_DATA_NUMPAGES * SZ_4K, DDK19_MIPSFW_DATA_NUMPAGES * SZ_4K, 0 },
	{ MIPS_BOOT_DATA, FW_DATA, ROGUE_MIPSFW_BOOT_NMI_DATA_VIRTUAL_BASE,
	  SZ_4K, SZ_4K, DDK19_MIPSFW_DATA_NUMPAGES * SZ_4K },
	{ MIPS_STACK, FW_DATA, DDK19_MIPSFW_STACK_VIRTUAL_BASE,
	  SZ_4K, SZ_4K, (DDK19_MIPSFW_DATA_NUMPAGES + 1) * SZ_4K },
};

static const struct pvr_fw_info_header ddk19_header = {
	.layout_entry_num = ARRAY_SIZE(ddk19_layout),
	.layout_entry_size = sizeof(struct pvr_fw_layout_entry),
};

static const u32 ddk19_hw_apm[][2] = {
	{ 0x504, 0x01a80000 }, { 0x508, 0x00080010 }, { 0x50c, 0x00100008 },
	{ 0x510, 0x00b800c8 }, { 0x514, 0x00b000c0 }, { 0x518, 0x00c000c8 },
	{ 0x51c, 0x00b000b8 }, { 0x520, 0x00d000d0 }, { 0x524, 0x00d000d0 },
	{ 0x528, 0x00d00000 }, { 0x024, 0x9004001b }, { 0x024, 0x8004001b },
};

static int pvr_ddk19_power_init(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct platform_device *pdev = to_platform_device(drm_dev->dev);
	struct pvr_ddk19 *ddk19;

	ddk19 = drmm_kzalloc(drm_dev, sizeof(*ddk19), GFP_KERNEL);
	if (!ddk19)
		return -ENOMEM;

	ddk19->mfg = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(ddk19->mfg))
		return PTR_ERR(ddk19->mfg);

	pvr_dev->ddk19 = ddk19;

	return pvr_power_sequence_ops_manual.init(pvr_dev);
}

static int pvr_ddk19_power_on(struct pvr_device *pvr_dev)
{
	int err = pvr_power_sequence_ops_manual.power_on(pvr_dev);

	if (err)
		return err;

	for (int i = 0; i < ARRAY_SIZE(ddk19_hw_apm); i++)
		writel(ddk19_hw_apm[i][1], pvr_dev->ddk19->mfg + ddk19_hw_apm[i][0]);

	return 0;
}

static int pvr_ddk19_power_off(struct pvr_device *pvr_dev)
{
	return pvr_power_sequence_ops_manual.power_off(pvr_dev);
}

const struct pvr_power_sequence_ops pvr_power_sequence_ops_ddk19 = {
	.init = pvr_ddk19_power_init,
	.power_on = pvr_ddk19_power_on,
	.power_off = pvr_ddk19_power_off,
};

char *pvr_ddk19_firmware_filename(struct pvr_device *pvr_dev)
{
	struct pvr_gpu_id *gpu_id = &pvr_dev->gpu_id;

	return kasprintf(GFP_KERNEL, "powervr/rogue_%d.%d.%d.%d_ddk19.fw",
			 gpu_id->b, gpu_id->v, gpu_id->n, gpu_id->c);
}

void pvr_ddk19_init_device_info(struct pvr_device *pvr_dev)
{
	struct pvr_device_features *f = &pvr_dev->features;

	pvr_dev->fw_dev.header = &ddk19_header;
	pvr_dev->fw_dev.layout_entries = ddk19_layout;

	f->has_axi_acelite = true;
	f->has_compute = true;
	f->has_gpu_virtualisation = true;
	f->has_gs_rta_support = true;
	f->has_mips = true;
	f->has_num_clusters = true;
	f->num_clusters = 1;
	f->has_num_isp_ipp_pipes = true;
	f->num_isp_ipp_pipes = 2;
	f->has_num_raster_pipes = true;
	f->num_raster_pipes = 1;
	f->has_pbe2_in_xe = true;
	f->has_pbvnc_coreid_reg = true;
	f->has_perfbus = true;
	f->has_phys_bus_width = true;
	f->phys_bus_width = 32;
	f->has_roguexe = true;
	f->has_simple_internal_parameter_format = true;
	f->has_simple_parameter_format_version = true;
	f->simple_parameter_format_version = 1;
	f->has_slc_banks = true;
	f->slc_banks = 1;
	f->has_slc_cache_line_size_bits = true;
	f->slc_cache_line_size_bits = 512;
	f->has_slc_size_in_kilobytes = true;
	f->slc_size_in_kilobytes = 8;
	f->has_sys_bus_secure_reset = true;
	f->has_virtual_address_space_bits = true;
	f->virtual_address_space_bits = 40;

	pvr_dev->quirks.has_brn63553 = true;
	pvr_dev->enhancements.has_ern42606 = true;
	pvr_dev->enhancements.has_ern57596 = true;
}

static int pvr_ddk19_mips_init(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct pvr_fw_mips_data *mips_data;

	pvr_fw_heap_info_init(pvr_dev, DDK19_MIPSFW_HEAP_LOG2_SIZE, 0);

	mips_data = drmm_kzalloc(drm_dev, sizeof(*mips_data), GFP_KERNEL);
	if (!mips_data)
		return -ENOMEM;

	/* The firmware takes one physical address for its whole page table. */
	mips_data->pt = dma_alloc_coherent(drm_dev->dev, DDK19_MIPSFW_PT_SIZE,
					   &mips_data->pt_dma_addr[0], GFP_KERNEL);
	if (!mips_data->pt)
		return -ENOMEM;

	mips_data->pfn_mask = ROGUE_MIPSFW_ENTRYLO_PFN_MASK;
	mips_data->cache_policy = ROGUE_MIPSFW_CACHED_POLICY;
	pvr_dev->fw_dev.processor_data.mips_data = mips_data;

	return 0;
}

static void pvr_ddk19_mips_fini(struct pvr_device *pvr_dev)
{
	struct pvr_fw_mips_data *mips_data = pvr_dev->fw_dev.processor_data.mips_data;

	dma_free_coherent(from_pvr_device(pvr_dev)->dev, DDK19_MIPSFW_PT_SIZE, mips_data->pt,
			  mips_data->pt_dma_addr[0]);
	pvr_dev->fw_dev.processor_data.mips_data = NULL;
}

static int pvr_ddk19_mips_fw_process(struct pvr_device *pvr_dev, const u8 *fw, u8 *fw_code_ptr,
				     u8 *fw_data_ptr, u8 *fw_core_code_ptr, u8 *fw_core_data_ptr,
				     u32 core_code_alloc_size)
{
	struct pvr_fw_device *fw_dev = &pvr_dev->fw_dev;
	struct pvr_fw_mips_data *mips_data = fw_dev->processor_data.mips_data;
	const struct pvr_fw_layout_entry *boot_code, *boot_data, *exceptions, *stack;
	dma_addr_t stack_dma_addr;
	u64 *boot;
	int err;

	err = pvr_fw_process_elf_command_stream(pvr_dev, fw, fw_code_ptr, fw_data_ptr,
						fw_core_code_ptr, fw_core_data_ptr);
	if (err)
		return err;

	boot_code = pvr_fw_find_layout_entry(pvr_dev, MIPS_BOOT_CODE);
	boot_data = pvr_fw_find_layout_entry(pvr_dev, MIPS_BOOT_DATA);
	exceptions = pvr_fw_find_layout_entry(pvr_dev, MIPS_EXCEPTIONS_CODE);
	stack = pvr_fw_find_layout_entry(pvr_dev, MIPS_STACK);

	err = pvr_gem_get_dma_addr(fw_dev->mem.code_obj->gem, boot_code->alloc_offset,
				   &mips_data->boot_code_dma_addr);
	if (!err)
		err = pvr_gem_get_dma_addr(fw_dev->mem.data_obj->gem, boot_data->alloc_offset,
					   &mips_data->boot_data_dma_addr);
	if (!err)
		err = pvr_gem_get_dma_addr(fw_dev->mem.code_obj->gem, exceptions->alloc_offset,
					   &mips_data->exception_code_dma_addr);
	if (!err)
		err = pvr_fw_object_get_dma_addr(fw_dev->mem.data_obj, stack->alloc_offset,
						 &stack_dma_addr);
	if (err)
		return err;

	boot = (u64 *)(fw_data_ptr + boot_data->alloc_offset);
	boot[DDK19_MIPSFW_BOOTLDR_REGS_BASE] = pvr_dev->regs_resource->start;
	boot[DDK19_MIPSFW_BOOTLDR_PT_BASE] = mips_data->pt_dma_addr[0];
	boot[DDK19_MIPSFW_BOOTLDR_STACK] = stack_dma_addr;

	return 0;
}

static int pvr_ddk19_mips_wrapper_init(struct pvr_device *pvr_dev)
{
	struct pvr_fw_mips_data *mips_data = pvr_dev->fw_dev.processor_data.mips_data;
	const u64 remap_settings = ROGUE_MIPSFW_BOOT_REMAP_LOG2_SEGMENT_SIZE;
	u32 slc_misc;

	pvr_cr_write32(pvr_dev, ROGUE_CR_SLC_CTRL_BYPASS,
		       ROGUE_CR_SLC_CTRL_BYPASS_REQ_IPF_OBJ_EN |
		       ROGUE_CR_SLC_CTRL_BYPASS_REQ_IPF_CPF_EN |
		       ROGUE_CR_SLC_CTRL_BYPASS_REQ_TPU_EN);
	slc_misc = pvr_cr_read32(pvr_dev, ROGUE_CR_SLC_CTRL_MISC);
	pvr_cr_write32(pvr_dev, ROGUE_CR_SLC_CTRL_MISC,
		       slc_misc | ROGUE_CR_SLC_CTRL_MISC_ENABLE_PSG_HAZARD_CHECK_EN);

	/* With a 32-bit bus the wrapper takes the physical register bank address. */
	pvr_cr_write64(pvr_dev, ROGUE_CR_MIPS_WRAPPER_CONFIG,
		       (pvr_dev->regs_resource->start >>
			ROGUE_MIPSFW_WRAPPER_CONFIG_REGBANK_ADDR_ALIGN) |
		       ROGUE_CR_MIPS_WRAPPER_CONFIG_BOOT_ISA_MODE_MICROMIPS);

	pvr_cr_write64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP1_CONFIG1,
		       ROGUE_MIPSFW_BOOT_REMAP_PHYS_ADDR_IN |
		       ROGUE_CR_MIPS_ADDR_REMAP1_CONFIG1_MODE_ENABLE_EN);
	pvr_cr_write64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP1_CONFIG2,
		       (mips_data->boot_code_dma_addr &
			~ROGUE_CR_MIPS_ADDR_REMAP1_CONFIG2_ADDR_OUT_CLRMSK) | remap_settings);

	pvr_cr_write64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP5_CONFIG1,
		       ROGUE_CR_MIPS_ADDR_REMAP5_CONFIG1_MODE_ENABLE_EN);
	pvr_cr_write64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP5_CONFIG2,
		       (mips_data->boot_code_dma_addr &
			~ROGUE_CR_MIPS_ADDR_REMAP5_CONFIG2_ADDR_OUT_CLRMSK) | remap_settings);

	pvr_cr_write64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP2_CONFIG1,
		       ROGUE_MIPSFW_DATA_REMAP_PHYS_ADDR_IN |
		       ROGUE_CR_MIPS_ADDR_REMAP2_CONFIG1_MODE_ENABLE_EN);
	pvr_cr_write64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP2_CONFIG2,
		       (mips_data->boot_data_dma_addr &
			~ROGUE_CR_MIPS_ADDR_REMAP2_CONFIG2_ADDR_OUT_CLRMSK) | remap_settings);

	pvr_cr_write64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP3_CONFIG1,
		       ROGUE_MIPSFW_CODE_REMAP_PHYS_ADDR_IN |
		       ROGUE_CR_MIPS_ADDR_REMAP3_CONFIG1_MODE_ENABLE_EN);
	pvr_cr_write64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP3_CONFIG2,
		       (mips_data->exception_code_dma_addr &
			~ROGUE_CR_MIPS_ADDR_REMAP3_CONFIG2_ADDR_OUT_CLRMSK) | remap_settings);

	pvr_cr_write64(pvr_dev, ROGUE_CR_MTS_GARTEN_WRAPPER_CONFIG,
		       ROGUE_CR_MTS_GARTEN_WRAPPER_CONFIG_IDLE_CTRL_META);
	pvr_cr_write32(pvr_dev, ROGUE_CR_MIPS_DEBUG_CONFIG, 0);

	return 0;
}

static u32 pvr_ddk19_mips_get_fw_addr_with_offset(struct pvr_fw_object *fw_obj, u32 offset)
{
	struct pvr_device *pvr_dev = to_pvr_device(gem_from_pvr_gem(fw_obj->gem)->dev);

	return ((fw_obj->fw_addr_offset + offset) & pvr_dev->fw_dev.fw_heap_info.offset_mask) |
	       DDK19_MIPSFW_CODE_VIRTUAL_BASE;
}

static bool pvr_ddk19_mips_irq_pending(struct pvr_device *pvr_dev)
{
	return pvr_cr_read32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS) &
	       ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS_EVENT_EN;
}

static void pvr_ddk19_mips_irq_clear(struct pvr_device *pvr_dev)
{
	pvr_cr_write32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_IRQ_CLEAR,
		       ROGUE_CR_MIPS_WRAPPER_IRQ_CLEAR_EVENT_EN);
}

static const struct pvr_fw_defs pvr_fw_defs_ddk19 = {
	.init = pvr_ddk19_mips_init,
	.fini = pvr_ddk19_mips_fini,
	.fw_process = pvr_ddk19_mips_fw_process,
	.vm_map = pvr_vm_mips_map,
	.vm_unmap = pvr_vm_mips_unmap,
	.get_fw_addr_with_offset = pvr_ddk19_mips_get_fw_addr_with_offset,
	.wrapper_init = pvr_ddk19_mips_wrapper_init,
	.irq_pending = pvr_ddk19_mips_irq_pending,
	.irq_clear = pvr_ddk19_mips_irq_clear,
	.has_fixed_data_addr = true,
};

static void *pvr_ddk19_alloc(struct pvr_device *pvr_dev, size_t size,
			     struct pvr_fw_object **obj, u32 *fw_addr)
{
	void *ptr = pvr_fw_object_create_and_map(pvr_dev, size, PVR_BO_FW_FLAGS_DEVICE_UNCACHED,
						 NULL, NULL, obj);

	if (IS_ERR(ptr)) {
		*obj = NULL;
		return ptr;
	}

	memset(ptr, 0, size);
	if (fw_addr)
		pvr_fw_object_get_fw_addr(*obj, fw_addr);

	return ptr;
}

static void pvr_ddk19_free(struct pvr_fw_object **obj)
{
	if (*obj)
		pvr_fw_object_unmap_and_destroy(*obj);
	*obj = NULL;
}

static void pvr_ddk19_destroy_structures(struct pvr_device *pvr_dev)
{
	struct pvr_ddk19 *ddk19 = pvr_dev->ddk19;

	pvr_ddk19_free(&ddk19->hwperf_ctl_obj);
	pvr_ddk19_free(&ddk19->runtime_cfg_obj);
	pvr_ddk19_free(&ddk19->gpu_util_obj);
	pvr_ddk19_free(&ddk19->hwrinfobuf_obj);
	pvr_ddk19_free(&ddk19->trace_obj);
	pvr_ddk19_free(&ddk19->tracebuf_obj);
	pvr_ddk19_free(&ddk19->fwccb_obj);
	pvr_ddk19_free(&ddk19->fwccb_ctl_obj);
	pvr_ddk19_free(&ddk19->kccb_obj);
	pvr_ddk19_free(&ddk19->kccb_ctl_obj);
	pvr_ddk19_free(&ddk19->power_sync_obj);
	pvr_ddk19_free(&ddk19->os_config_obj);
	pvr_ddk19_free(&ddk19->init_obj);

	if (ddk19->fault_page)
		dma_free_coherent(from_pvr_device(pvr_dev)->dev, SZ_4K, ddk19->fault_page,
				  ddk19->fault_page_dma);
	ddk19->fault_page = NULL;
}

static int pvr_ddk19_ccb_init(struct pvr_device *pvr_dev, u32 num_log2, u32 cmd_size,
			      struct pvr_fw_object **ctl_obj, u32 *ctl_fw_addr,
			      struct pvr_fw_object **ccb_obj, u32 *ccb_fw_addr)
{
	struct ddk19_fwif_ccb_ctl *ctl;
	void *ccb;

	ctl = pvr_ddk19_alloc(pvr_dev, sizeof(*ctl), ctl_obj, ctl_fw_addr);
	if (IS_ERR(ctl))
		return PTR_ERR(ctl);

	ccb = pvr_ddk19_alloc(pvr_dev, cmd_size << num_log2, ccb_obj, ccb_fw_addr);
	if (IS_ERR(ccb))
		return PTR_ERR(ccb);

	ctl->wrap_mask = (1 << num_log2) - 1;
	ctl->cmd_size = cmd_size;

	return 0;
}

static int pvr_ddk19_create_structures(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct pvr_ddk19 *ddk19 = pvr_dev->ddk19;
	struct ddk19_fwif_os_config *os_config;
	struct ddk19_fwif_runtime_cfg *runtime_cfg;
	struct ddk19_fwif_init *init;
	u32 power_sync_fw_addr;
	void *ptr;
	int err;

	init = pvr_ddk19_alloc(pvr_dev, sizeof(*init), &ddk19->init_obj, NULL);
	if (IS_ERR(init))
		return PTR_ERR(init);
	ddk19->init = init;

	ptr = pvr_ddk19_alloc(pvr_dev, sizeof(u32), &ddk19->power_sync_obj, &power_sync_fw_addr);
	if (IS_ERR(ptr))
		goto err_alloc;

	os_config = pvr_ddk19_alloc(pvr_dev, sizeof(*os_config), &ddk19->os_config_obj,
				    &init->os_config);
	if (IS_ERR(os_config)) {
		ptr = os_config;
		goto err_alloc;
	}
	os_config->config_flags = DDK19_INICFG_HWR_EN;
	os_config->power_sync = power_sync_fw_addr;

	ddk19->fault_page = dma_alloc_coherent(drm_dev->dev, SZ_4K, &ddk19->fault_page_dma,
					       GFP_KERNEL);
	if (!ddk19->fault_page) {
		ptr = ERR_PTR(-ENOMEM);
		goto err_alloc;
	}
	memset32(ddk19->fault_page, 0xdeadbeef, SZ_4K / sizeof(u32));
	init->fault_phys_addr = ddk19->fault_page_dma;

	err = pvr_ddk19_ccb_init(pvr_dev, DDK19_KCCB_NUMCMDS_LOG2, DDK19_KCCB_CMD_SIZE,
				 &ddk19->kccb_ctl_obj, &init->kccb_ctl,
				 &ddk19->kccb_obj, &init->kccb);
	if (!err)
		err = pvr_ddk19_ccb_init(pvr_dev, DDK19_FWCCB_NUMCMDS_LOG2, DDK19_FWCCB_CMD_SIZE,
					 &ddk19->fwccb_ctl_obj, &init->fwccb_ctl,
					 &ddk19->fwccb_obj, &init->fwccb);
	if (err) {
		ptr = ERR_PTR(err);
		goto err_alloc;
	}

	ddk19->tracebuf = pvr_ddk19_alloc(pvr_dev, sizeof(*ddk19->tracebuf),
					  &ddk19->tracebuf_obj, &init->tracebuf_ctl);
	if (IS_ERR(ddk19->tracebuf)) {
		ptr = ddk19->tracebuf;
		goto err_alloc;
	}

	ddk19->trace = pvr_ddk19_alloc(pvr_dev, DDK19_TRACE_BUFFER_SIZE * sizeof(u32),
				       &ddk19->trace_obj,
				       &ddk19->tracebuf->trace_buf[0].trace_buffer_fw_addr);
	if (IS_ERR(ddk19->trace)) {
		ptr = ddk19->trace;
		goto err_alloc;
	}
	ddk19->tracebuf->log_type = DDK19_LOG_TYPE_TRACE | DDK19_LOG_TYPE_GROUP_MAIN |
				    DDK19_LOG_TYPE_GROUP_POW | DDK19_LOG_TYPE_GROUP_HWR;

	ptr = pvr_ddk19_alloc(pvr_dev, DDK19_HWRINFOBUF_SIZE, &ddk19->hwrinfobuf_obj,
			      &init->hwrinfobuf_ctl);
	if (IS_ERR(ptr))
		goto err_alloc;

	ptr = pvr_ddk19_alloc(pvr_dev, DDK19_GPU_UTIL_FWCB_SIZE, &ddk19->gpu_util_obj,
			      &init->gpu_util_fwcb_ctl);
	if (IS_ERR(ptr))
		goto err_alloc;
	*(u64 *)(ptr + DDK19_GPU_UTIL_FWCB_LAST_WORD) =
		(ktime_get_ns() & ~3ull) | DDK19_GPU_UTIL_STATE_IDLE;

	runtime_cfg = pvr_ddk19_alloc(pvr_dev, sizeof(*runtime_cfg), &ddk19->runtime_cfg_obj,
				      &init->runtime_cfg);
	if (IS_ERR(runtime_cfg)) {
		ptr = runtime_cfg;
		goto err_alloc;
	}
	runtime_cfg->active_pm_latency_ms = DDK19_ACTIVE_PM_LATENCY_MS;
	runtime_cfg->active_pm_latency_persistent = 1;
	runtime_cfg->core_clock_speed = DDK19_CORE_CLOCK_SPEED;
	runtime_cfg->default_dusts_num_init = 1;

	ptr = pvr_ddk19_alloc(pvr_dev, DDK19_HWPERF_CTL_SIZE, &ddk19->hwperf_ctl_obj,
			      &init->hwperf_ctl);
	if (IS_ERR(ptr))
		goto err_alloc;

	init->pds_exec_base = DDK19_PDSCODEDATA_BRN_65273_HEAP_BASE;
	init->usc_exec_base = DDK19_USCCODE_BRN_65273_HEAP_BASE;
	init->dpx_control_stream_base = DDK19_DOPPLER_HEAP_BASE;
	init->result_dump_base = DDK19_DOPPLER_OVERFLOW_HEAP_BASE;
	init->rtu_heap_base = DDK19_DOPPLER_HEAP_BASE;
	init->tdm_tpu_yuv_coeffs_heap_base = DDK19_TDM_TPU_YUV_COEFFS_HEAP_BASE;
	for (int i = 0; i < ARRAY_SIZE(init->bif_tiling_cfg); i++) {
		init->bif_tiling_cfg[i].base = DDK19_BIF_TILING_HEAP_BASE +
					       i * DDK19_BIF_TILING_HEAP_SIZE;
		init->bif_tiling_cfg[i].len = DDK19_BIF_TILING_HEAP_SIZE;
	}
	init->initial_core_clock_speed = DDK19_CORE_CLOCK_SPEED;
	init->active_pm_latency_ms = DDK19_ACTIVE_PM_LATENCY_MS;
	init->marker_val = 1;
	init->compchecks.hw_bvnc.layout_version = DDK19_COMPCHECKS_LAYOUT_VERSION;
	init->compchecks.hw_bvnc.v_len_max = DDK19_COMPCHECKS_BVNC_V_LEN_MAX;
	init->compchecks.fw_bvnc.layout_version = DDK19_COMPCHECKS_LAYOUT_VERSION;
	init->compchecks.fw_bvnc.v_len_max = DDK19_COMPCHECKS_BVNC_V_LEN_MAX;

	return 0;

err_alloc:
	pvr_ddk19_destroy_structures(pvr_dev);
	return PTR_ERR(ptr);
}

static int pvr_ddk19_set_boot_fwinit(struct pvr_device *pvr_dev)
{
	struct pvr_fw_mem *fw_mem = &pvr_dev->fw_dev.mem;
	const struct pvr_fw_layout_entry *boot_data;
	u64 fwinit = pvr_fw_obj_get_gpu_addr(pvr_dev->ddk19->init_obj);
	u32 offset;
	u8 *data;

	boot_data = pvr_fw_find_layout_entry(pvr_dev, MIPS_BOOT_DATA);
	offset = boot_data->alloc_offset + DDK19_MIPSFW_BOOTLDR_FWINIT * sizeof(u64);

	data = pvr_fw_object_vmap(fw_mem->data_obj);
	if (IS_ERR(data))
		return PTR_ERR(data);

	*(u64 *)(data + offset) = fwinit;
	*(u64 *)(fw_mem->data + offset) = fwinit;
	pvr_fw_object_vunmap(fw_mem->data_obj);

	return 0;
}

static void pvr_ddk19_dump_state(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct pvr_ddk19 *ddk19 = pvr_dev->ddk19;
	struct ddk19_fwif_tracebuf *tb = ddk19->tracebuf;
	struct ddk19_fwif_assertbuf *ab = &tb->trace_buf[0].assert_buf;
	u32 ptr = READ_ONCE(tb->trace_buf[0].trace_pointer);

	drm_info(drm_dev, "ddk19: pow_state %u trace_pointer %u kccb_cmds %u irqs %u\n",
		 READ_ONCE(tb->pow_state), ptr, READ_ONCE(tb->kccb_cmds_executed),
		 READ_ONCE(tb->interrupt_count[0]));
	drm_info(drm_dev, "ddk19: MIPS_WRAPPER_IRQ_STATUS 0x%08x SOFT_RESET 0x%016llx\n",
		 pvr_cr_read32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS),
		 pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET));
	if (ab->path[0] || ab->info[0])
		drm_err(drm_dev, "ddk19: FW assert %.200s:%u: %.200s\n", ab->path,
			ab->line_num, ab->info);
	if (ptr)
		print_hex_dump(KERN_INFO, "ddk19 trace: ", DUMP_PREFIX_OFFSET, 32, 4,
			       ddk19->trace, min_t(u32, ptr, 256) * sizeof(u32), false);
}

int pvr_ddk19_fw_init(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct pvr_fw_device *fw_dev = &pvr_dev->fw_dev;
	struct ddk19_fwif_compchecks *cc;
	ktime_t deadline;
	int err;

	fw_dev->defs = &pvr_fw_defs_ddk19;

	err = fw_dev->defs->init(pvr_dev);
	if (err)
		return err;

	drm_mm_init(&fw_dev->fw_mm, ROGUE_FW_HEAP_BASE, fw_dev->fw_heap_info.raw_size);
	fw_dev->fw_mm_base = ROGUE_FW_HEAP_BASE;
	spin_lock_init(&fw_dev->fw_mm_lock);

	INIT_LIST_HEAD(&fw_dev->fw_objs.list);
	err = drmm_mutex_init(drm_dev, &fw_dev->fw_objs.lock);
	if (err)
		goto err_mm_takedown;

	err = pvr_fw_process(pvr_dev);
	if (err)
		goto err_mm_takedown;

	err = pvr_ddk19_create_structures(pvr_dev);
	if (err)
		goto err_fw_cleanup;

	err = pvr_ddk19_set_boot_fwinit(pvr_dev);
	if (err)
		goto err_destroy_structures;

	err = pvr_fw_start(pvr_dev);
	if (err)
		goto err_destroy_structures;

	deadline = ktime_add_us(ktime_get(), DDK19_BOOT_TIMEOUT_US);
	while (!READ_ONCE(pvr_dev->ddk19->init->firmware_started) &&
	       ktime_before(ktime_get(), deadline))
		usleep_range(1000, 2000);

	cc = &pvr_dev->ddk19->init->compchecks;
	if (!READ_ONCE(pvr_dev->ddk19->init->firmware_started)) {
		drm_err(drm_dev, "ddk19: firmware did not start\n");
		err = -ETIMEDOUT;
	} else {
		drm_info(drm_dev, "ddk19: firmware started, timestamp %u\n",
			 pvr_dev->ddk19->init->firmware_started_timestamp);
		drm_info(drm_dev,
			 "ddk19: compchecks updated %u FW BNC 0x%016llx V '%.8s' HW BNC 0x%016llx V '%.8s'\n",
			 cc->updated, cc->fw_bvnc.bnc, cc->fw_bvnc.v, cc->hw_bvnc.bnc,
			 cc->hw_bvnc.v);
		drm_info(drm_dev,
			 "ddk19: DDK version 0x%08x build %u options 0x%08x processor 0x%08x\n",
			 cc->ddk_version, cc->ddk_build, cc->build_options,
			 cc->fw_processor_version);
		err = -ENODEV;
	}
	pvr_ddk19_dump_state(pvr_dev);

	/* Job submission is not there yet, so halt the firmware before freeing its memory. */
	pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET, ROGUE_CR_SOFT_RESET__PBE2_XE__MASKFULL);
	(void)pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET);

err_destroy_structures:
	pvr_ddk19_destroy_structures(pvr_dev);
err_fw_cleanup:
	pvr_fw_cleanup(pvr_dev);
err_mm_takedown:
	drm_mm_takedown(&fw_dev->fw_mm);
	fw_dev->defs->fini(pvr_dev);

	return err;
}
