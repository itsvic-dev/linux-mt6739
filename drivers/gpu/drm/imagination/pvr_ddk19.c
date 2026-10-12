// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include "pvr_cccb.h"
#include "pvr_context.h"
#include "pvr_ddk19.h"
#include "pvr_ddk19_fwif.h"
#include "pvr_device.h"
#include "pvr_fw.h"
#include "pvr_fw_info.h"
#include "pvr_fw_mips.h"
#include "pvr_fw_startstop.h"
#include "pvr_gem.h"
#include "pvr_hwrt.h"
#include "pvr_job.h"
#include "pvr_mmu.h"
#include "pvr_power.h"
#include "pvr_rogue_cr_defs.h"
#include "pvr_rogue_fwif.h"
#include "pvr_rogue_fwif_client.h"
#include "pvr_rogue_heap_config.h"
#include "pvr_rogue_mips.h"
#include "pvr_vm.h"
#include "pvr_vm_mips.h"

#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <linux/delay.h>
#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#define DDK19_CORE_CLOCK_SPEED 570000000
#define DDK19_ACTIVE_PM_LATENCY_MS 50
#define DDK19_BOOT_TIMEOUT_US 1000000

enum {
	DDK19_SYNC_KCCB,
	DDK19_SYNC_MMU,
	DDK19_SYNC_CLEANUP,
	DDK19_SYNC_COUNT,
};

struct pvr_ddk19 {
	void __iomem *mfg;

	struct mutex kccb_lock;
	struct ddk19_fwif_ccb_ctl *kccb_ctl;
	u8 *kccb;
	struct ddk19_fwif_ccb_ctl *fwccb_ctl;
	u8 *fwccb;
	u32 *power_sync;
	struct mutex sync_lock;
	u32 *sync;
	u32 sync_fw_addr;
	u16 mmu_sync_val;
	struct pvr_fw_object *sync_obj;

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
	u8 *hwrinfobuf;
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
	int err;

	ddk19 = drmm_kzalloc(drm_dev, sizeof(*ddk19), GFP_KERNEL);
	if (!ddk19)
		return -ENOMEM;

	ddk19->mfg = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(ddk19->mfg))
		return PTR_ERR(ddk19->mfg);

	err = drmm_mutex_init(drm_dev, &ddk19->kccb_lock);
	if (!err)
		err = drmm_mutex_init(drm_dev, &ddk19->sync_lock);
	if (err)
		return err;

	pvr_dev->ddk19 = ddk19;

	return pvr_power_sequence_ops_manual.init(pvr_dev);
}

/* HW APM switches MFG1 through the SPM firmware, which mainline does not load. */
static bool hw_apm;
module_param(hw_apm, bool, 0644);

static int pvr_ddk19_power_on(struct pvr_device *pvr_dev)
{
	int err = pvr_power_sequence_ops_manual.power_on(pvr_dev);

	if (err || !hw_apm)
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
	f->has_common_store_size_in_dwords = true;
	f->common_store_size_in_dwords = 1024 * 4 * 4;
	f->has_compute = true;
	f->has_gpu_virtualisation = true;
	f->has_gs_rta_support = true;
	f->has_isp_samples_per_pixel = true;
	f->isp_samples_per_pixel = 1;
	f->has_max_partitions = true;
	f->max_partitions = 10;
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
	f->has_tile_size_x = true;
	f->tile_size_x = 16;
	f->has_tile_size_y = true;
	f->tile_size_y = 16;
	f->has_usc_min_output_registers_per_pix = true;
	f->usc_min_output_registers_per_pix = 2;
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

	pvr_ddk19_free(&ddk19->sync_obj);
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
			      struct ddk19_fwif_ccb_ctl **ctl_out,
			      struct pvr_fw_object **ccb_obj, u32 *ccb_fw_addr, u8 **ccb_out)
{
	struct ddk19_fwif_ccb_ctl *ctl;
	u8 *ccb;

	ctl = pvr_ddk19_alloc(pvr_dev, sizeof(*ctl), ctl_obj, ctl_fw_addr);
	if (IS_ERR(ctl))
		return PTR_ERR(ctl);

	ccb = pvr_ddk19_alloc(pvr_dev, cmd_size << num_log2, ccb_obj, ccb_fw_addr);
	if (IS_ERR(ccb))
		return PTR_ERR(ccb);

	ctl->wrap_mask = (1 << num_log2) - 1;
	ctl->cmd_size = cmd_size;
	*ctl_out = ctl;
	*ccb_out = ccb;

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

	ddk19->power_sync = pvr_ddk19_alloc(pvr_dev, sizeof(u32), &ddk19->power_sync_obj,
					    &power_sync_fw_addr);
	if (IS_ERR(ddk19->power_sync)) {
		ptr = ddk19->power_sync;
		goto err_alloc;
	}

	ddk19->sync = pvr_ddk19_alloc(pvr_dev, DDK19_SYNC_COUNT * sizeof(u32), &ddk19->sync_obj,
				      &ddk19->sync_fw_addr);
	if (IS_ERR(ddk19->sync)) {
		ptr = ddk19->sync;
		goto err_alloc;
	}

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
				 &ddk19->kccb_ctl_obj, &init->kccb_ctl, &ddk19->kccb_ctl,
				 &ddk19->kccb_obj, &init->kccb, &ddk19->kccb);
	if (!err)
		err = pvr_ddk19_ccb_init(pvr_dev, DDK19_FWCCB_NUMCMDS_LOG2, DDK19_FWCCB_CMD_SIZE,
					 &ddk19->fwccb_ctl_obj, &init->fwccb_ctl, &ddk19->fwccb_ctl,
					 &ddk19->fwccb_obj, &init->fwccb, &ddk19->fwccb);
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
	ddk19->hwrinfobuf = ptr;

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

static int pvr_ddk19_poll(u32 *ptr, u32 value, u32 timeout_us)
{
	u32 v;

	return read_poll_timeout(READ_ONCE, v, v == value, 100, timeout_us, false, *ptr);
}

static int pvr_ddk19_wait_for_boot(struct pvr_device *pvr_dev)
{
	struct pvr_ddk19 *ddk19 = pvr_dev->ddk19;
	int err = pvr_ddk19_poll(&ddk19->init->firmware_started, 1, DDK19_BOOT_TIMEOUT_US);

	if (err)
		drm_err(from_pvr_device(pvr_dev), "ddk19: firmware did not start\n");

	return err;
}

static int pvr_ddk19_kccb_send(struct pvr_device *pvr_dev, struct ddk19_fwif_kccb_cmd *cmd)
{
	struct pvr_ddk19 *ddk19 = pvr_dev->ddk19;
	struct ddk19_fwif_ccb_ctl *ctl = ddk19->kccb_ctl;
	u32 write_offset, next, read_offset;
	int err;

	guard(mutex)(&ddk19->kccb_lock);

	write_offset = READ_ONCE(ctl->write_offset);
	next = (write_offset + 1) & ctl->wrap_mask;
	err = read_poll_timeout(READ_ONCE, read_offset, read_offset != next, 100, 100000, false,
				ctl->read_offset);
	if (err)
		return err;

	memcpy(ddk19->kccb + write_offset * DDK19_KCCB_CMD_SIZE, cmd, sizeof(*cmd));
	wmb();
	WRITE_ONCE(ctl->write_offset, next);

	pvr_fw_mts_schedule(pvr_dev, (DDK19_DM_GP & ~ROGUE_CR_MTS_SCHEDULE_DM_CLRMSK) |
				     ROGUE_CR_MTS_SCHEDULE_TASK_COUNTED);

	return 0;
}

static bool pvr_ddk19_kccb_is_idle(struct pvr_device *pvr_dev)
{
	struct ddk19_fwif_ccb_ctl *ctl = pvr_dev->ddk19->kccb_ctl;

	return READ_ONCE(ctl->read_offset) == READ_ONCE(ctl->write_offset);
}

static int pvr_ddk19_sync_powered(struct pvr_device *pvr_dev)
{
	struct pvr_ddk19 *ddk19 = pvr_dev->ddk19;
	struct ddk19_fwif_kccb_cmd cmd = {
		.cmd_type = DDK19_KCCB_CMD_SYNC,
		.data.sync = {
			.sync_fw_addr = ddk19->sync_fw_addr + DDK19_SYNC_KCCB * sizeof(u32),
			.update_val = 1,
		},
	};
	int err;

	guard(mutex)(&ddk19->sync_lock);

	WRITE_ONCE(ddk19->sync[DDK19_SYNC_KCCB], 0);

	err = pvr_ddk19_kccb_send(pvr_dev, &cmd);
	if (err)
		return err;

	return pvr_ddk19_poll(&ddk19->sync[DDK19_SYNC_KCCB], 1, 1000000);
}

static int pvr_ddk19_mmu_flush_powered(struct pvr_device *pvr_dev, u32 flags)
{
	struct pvr_ddk19 *ddk19 = pvr_dev->ddk19;
	struct ddk19_fwif_kccb_cmd cmd = {
		.cmd_type = DDK19_KCCB_CMD_MMUCACHE,
		.data.mmu_cache = {
			.flags = flags,
			.sync_fw_addr = ddk19->sync_fw_addr + DDK19_SYNC_MMU * sizeof(u32),
		},
	};
	int err;

	guard(mutex)(&ddk19->sync_lock);

	cmd.data.mmu_cache.sync_update_val = ++ddk19->mmu_sync_val;

	err = pvr_ddk19_kccb_send(pvr_dev, &cmd);
	if (err)
		return err;

	return pvr_ddk19_poll(&ddk19->sync[DDK19_SYNC_MMU], cmd.data.mmu_cache.sync_update_val,
			      1000000);
}

static int pvr_ddk19_cleanup_dm(struct pvr_device *pvr_dev, u32 type, u32 fw_addr, u32 dm)
{
	struct pvr_ddk19 *ddk19 = pvr_dev->ddk19;
	struct ddk19_fwif_kccb_cmd cmd = {
		.cmd_type = DDK19_KCCB_CMD_CLEANUP,
		.dm = dm,
		.data.cleanup = {
			.cleanup_type = type,
			.fw_addr = fw_addr,
			.sync_fw_addr = ddk19->sync_fw_addr + DDK19_SYNC_CLEANUP * sizeof(u32),
		},
	};
	u32 *sync = &ddk19->sync[DDK19_SYNC_CLEANUP];
	u32 v;
	int err;

	guard(mutex)(&ddk19->sync_lock);

	WRITE_ONCE(*sync, 0);

	err = pvr_ddk19_kccb_send(pvr_dev, &cmd);
	if (err)
		return err;

	err = read_poll_timeout(READ_ONCE, v, v & DDK19_CLEANUP_RUN, 100, 1000000, false, *sync);
	if (err)
		return err;

	return (v & DDK19_CLEANUP_BUSY) ? -EBUSY : 0;
}

static int pvr_ddk19_cleanup_powered(struct pvr_device *pvr_dev, u32 type,
				     struct pvr_fw_object *fw_obj, u32 offset)
{
	u32 dm = DDK19_DM_GP;
	u32 fw_addr;
	int err;

	pvr_fw_object_get_fw_addr_offset(fw_obj, offset, &fw_addr);

	if (type == ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT) {
		u8 *map = pvr_fw_object_vmap(fw_obj);

		if (IS_ERR(map))
			return PTR_ERR(map);
		dm = ((struct ddk19_fwif_fwcommoncontext *)(map + offset))->dm;
		pvr_fw_object_vunmap(fw_obj);
	} else if (type == ROGUE_FWIF_CLEANUP_HWRTDATA) {
		err = pvr_ddk19_cleanup_dm(pvr_dev, type, fw_addr, DDK19_DM_TA);
		if (err)
			return err;
		dm = DDK19_DM_3D;
	}

	return pvr_ddk19_cleanup_dm(pvr_dev, type, fw_addr, dm);
}

int pvr_ddk19_sync(struct pvr_device *pvr_dev)
{
	int err = pvr_power_get(pvr_dev);

	if (err)
		return err;
	err = pvr_ddk19_sync_powered(pvr_dev);
	pvr_power_put(pvr_dev);

	return err;
}

int pvr_ddk19_mmu_flush(struct pvr_device *pvr_dev, u32 flags)
{
	int err = pvr_power_get(pvr_dev);

	if (err)
		return err;
	err = pvr_ddk19_mmu_flush_powered(pvr_dev, flags);
	pvr_power_put(pvr_dev);

	return err;
}

int pvr_ddk19_cleanup(struct pvr_device *pvr_dev, u32 type, struct pvr_fw_object *fw_obj,
		      u32 offset)
{
	int err = pvr_power_get(pvr_dev);

	if (err)
		return err;
	err = pvr_ddk19_cleanup_powered(pvr_dev, type, fw_obj, offset);
	pvr_power_put(pvr_dev);

	return err;
}

int pvr_ddk19_kick(struct pvr_device *pvr_dev, u32 dm, u32 ctx_fw_addr, u32 write_offset,
		   struct pvr_hwrt_data *hwrt)
{
	struct ddk19_fwif_kccb_cmd cmd = {
		.cmd_type = DDK19_KCCB_CMD_KICK,
		.dm = dm,
		.data.kick = {
			.context_fw_addr = ctx_fw_addr,
			.client_woff_update = write_offset,
		},
	};

	if (hwrt && (dm == DDK19_DM_TA || dm == DDK19_DM_3D)) {
		u32 offset = dm == DDK19_DM_TA ?
			     offsetof(struct ddk19_fwif_hwrtdata, ta_cleanup_ctl) :
			     offsetof(struct ddk19_fwif_hwrtdata, cleanup_ctl_3d);

		pvr_fw_object_get_fw_addr_offset(hwrt->fw_obj, offset,
						 &cmd.data.kick.cleanup_ctl_fw_addr[0]);
		cmd.data.kick.num_cleanup_ctl = 1;
	}

	wmb();

	return pvr_ddk19_kccb_send(pvr_dev, &cmd);
}

static u32 pvr_ddk19_ccb_cmd_type(u32 type)
{
	switch (type) {
	case ROGUE_FWIF_CCB_CMD_TYPE_GEOM:
		return DDK19_CCB_CMD_TYPE_TA;
	case ROGUE_FWIF_CCB_CMD_TYPE_FRAG:
		return DDK19_CCB_CMD_TYPE_3D;
	case ROGUE_FWIF_CCB_CMD_TYPE_FRAG_PR:
		return DDK19_CCB_CMD_TYPE_3D_PR;
	case ROGUE_FWIF_CCB_CMD_TYPE_CDM:
		return DDK19_CCB_CMD_TYPE_CDM;
	case ROGUE_FWIF_CCB_CMD_TYPE_TQ_3D:
		return DDK19_CCB_CMD_TYPE_TQ_3D;
	case ROGUE_FWIF_CCB_CMD_TYPE_TQ_TDM:
		return DDK19_CCB_CMD_TYPE_TQ_TDM;
	case ROGUE_FWIF_CCB_CMD_TYPE_NULL:
		return DDK19_CCB_CMD_TYPE_NULL;
	case ROGUE_FWIF_CCB_CMD_TYPE_FENCE:
		return DDK19_CCB_CMD_TYPE_FENCE;
	case ROGUE_FWIF_CCB_CMD_TYPE_UPDATE:
		return DDK19_CCB_CMD_TYPE_UPDATE;
	case ROGUE_FWIF_CCB_CMD_TYPE_RMW_UPDATE:
		return DDK19_CCB_CMD_TYPE_RMW_UPDATE;
	case ROGUE_FWIF_CCB_CMD_TYPE_FENCE_PR:
		return DDK19_CCB_CMD_TYPE_FENCE_PR;
	case ROGUE_FWIF_CCB_CMD_TYPE_PRIORITY:
		return DDK19_CCB_CMD_TYPE_PRIORITY;
	case ROGUE_FWIF_CCB_CMD_TYPE_PADDING:
		return DDK19_CCB_CMD_TYPE_PADDING;
	}

	WARN(1, "ddk19: no CCB command type for 0x%x\n", type);
	return DDK19_CCB_CMD_TYPE_PADDING;
}

void pvr_ddk19_cccb_write(struct pvr_cccb *cccb, u32 cmd_type, u32 cmd_size, void *cmd_data,
			  u32 ext_job_ref, u32 int_job_ref)
{
	const u32 hdr_size = sizeof(struct ddk19_fwif_ccb_cmd_header);
	struct ddk19_fwif_ccb_cmd_header hdr = {
		.cmd_type = pvr_ddk19_ccb_cmd_type(cmd_type),
		.cmd_size = ALIGN(cmd_size, 8),
		.ext_job_ref = ext_job_ref,
		.int_job_ref = int_job_ref,
	};
	u32 size = hdr_size + ALIGN(cmd_size, 8);
	u32 remaining = cccb->size - cccb->write_offset;
	u32 required = size;
	u32 space;

	if (remaining < size + hdr_size)
		required += remaining;

	space = (READ_ONCE(cccb->ctrl->read_offset) - cccb->write_offset + cccb->size - 1) &
		(cccb->size - 1);
	if (WARN_ON(space < required))
		return;

	if (required != size) {
		struct ddk19_fwif_ccb_cmd_header pad = {
			.cmd_type = DDK19_CCB_CMD_TYPE_PADDING,
			.cmd_size = remaining - hdr_size,
		};

		memcpy(&cccb->cccb[cccb->write_offset], &pad, sizeof(pad));
		cccb->write_offset = 0;
	}

	memcpy(&cccb->cccb[cccb->write_offset], &hdr, sizeof(hdr));
	memcpy(&cccb->cccb[cccb->write_offset + hdr_size], cmd_data, cmd_size);
	cccb->write_offset = (cccb->write_offset + size) & cccb->wrap_mask;
}

u32 pvr_ddk19_static_state_offset(enum drm_pvr_ctx_type type)
{
	switch (type) {
	case DRM_PVR_CTX_TYPE_RENDER:
		return 2 * sizeof(struct ddk19_fwif_fwcommoncontext) + 8;
	default:
		return sizeof(struct ddk19_fwif_fwcommoncontext);
	}
}

u32 pvr_ddk19_fw_ctx_size(enum drm_pvr_ctx_type type)
{
	switch (type) {
	case DRM_PVR_CTX_TYPE_RENDER:
		return pvr_ddk19_static_state_offset(type) +
		       sizeof(struct rogue_fwif_static_rendercontext_state);
	case DRM_PVR_CTX_TYPE_COMPUTE:
		return pvr_ddk19_static_state_offset(type) +
		       sizeof(struct rogue_fwif_static_computecontext_state);
	case DRM_PVR_CTX_TYPE_TRANSFER_FRAG:
		return sizeof(struct ddk19_fwif_fwcommoncontext);
	}

	return 0;
}

u32 pvr_ddk19_ctx_offset(enum drm_pvr_job_type type)
{
	return type == DRM_PVR_JOB_TYPE_FRAGMENT ? sizeof(struct ddk19_fwif_fwcommoncontext) : 0;
}

static u32 pvr_ddk19_ctx_state_only_size(enum drm_pvr_job_type type)
{
	switch (type) {
	case DRM_PVR_JOB_TYPE_GEOMETRY:
		return DDK19_TACTX_STATE_SIZE;
	case DRM_PVR_JOB_TYPE_FRAGMENT:
	case DRM_PVR_JOB_TYPE_TRANSFER_FRAG:
		return DDK19_3DCTX_STATE_SIZE;
	default:
		return DDK19_CTX_STATE_SIZE;
	}
}

/* The RF command follows the context state in the same object. */
u32 pvr_ddk19_ctx_state_size(enum drm_pvr_job_type type)
{
	return pvr_ddk19_ctx_state_only_size(type) + DDK19_RF_CMD_SIZE;
}

u32 pvr_ddk19_rf_cmd_offset(enum drm_pvr_job_type type)
{
	return pvr_ddk19_ctx_state_only_size(type);
}

void pvr_ddk19_init_common_context(void *dst, u32 ccbctl_fw_addr, u32 ccb_fw_addr,
				   u32 fw_mem_ctx_fw_addr, u32 ctx_state_fw_addr,
				   u32 rf_cmd_fw_addr, u32 priority, u32 ctx_id, u32 dm)
{
	struct ddk19_fwif_fwcommoncontext *cctx = dst;

	cctx->ccbctl_fw_addr = ccbctl_fw_addr;
	cctx->ccb_fw_addr = ccb_fw_addr;
	cctx->fw_mem_context_fw_addr = fw_mem_ctx_fw_addr;
	cctx->context_state_fw_addr = ctx_state_fw_addr;
	cctx->rf_cmd_fw_addr = rf_cmd_fw_addr;
	cctx->priority = priority;
	cctx->server_common_context_id = ctx_id;
	cctx->pid = task_tgid_nr(current);
	cctx->dm = dm;
}

void pvr_ddk19_free_list_init(void *dst, u64 dev_addr, u32 max_pages, u32 current_pages,
			      u32 grow_pages, u32 ready_pages, u32 id)
{
	struct ddk19_fwif_freelist *fl = dst;

	memset(fl, 0, sizeof(*fl));
	fl->freelist_dev_addr = dev_addr;
	fl->current_dev_addr = dev_addr + (max_pages - current_pages) * sizeof(u32);
	fl->current_stack_top = current_pages - 1;
	fl->max_pages = max_pages;
	fl->grow_pages = grow_pages;
	fl->current_pages = current_pages;
	fl->freelist_id = id;
	fl->ready_pages = ready_pages;
	fl->oom_ready_limit = ready_pages;
}

static void ddk19_render_target_init(void *cpu_ptr, void *priv)
{
	struct ddk19_fwif_render_target *rt = cpu_ptr;

	rt->vheap_table_dev_addr = *(u64 *)priv;
}

int pvr_ddk19_hwrt_common_init(struct pvr_hwrt_dataset *hwrt, u64 vheap_table_dev_addr)
{
	return pvr_fw_object_create(hwrt->pvr_dev, sizeof(struct ddk19_fwif_render_target),
				    PVR_BO_FW_FLAGS_DEVICE_UNCACHED, ddk19_render_target_init,
				    &vheap_table_dev_addr, &hwrt->common_fw_obj);
}

static void ddk19_rta_ctl_init(void *cpu_ptr, void *priv)
{
	struct rogue_fwif_rta_ctl *src = priv;
	struct ddk19_fwif_rta_ctl *rta_ctl = cpu_ptr;

	rta_ctl->valid_render_targets_fw_addr = src->valid_render_targets_fw_addr;
	rta_ctl->num_renders_fw_addr = src->rta_num_partial_renders_fw_addr;
	rta_ctl->max_rts = max_t(u16, src->max_rts, 1);
}

static void ddk19_hwrtdata_init(void *cpu_ptr, void *priv)
{
	struct pvr_hwrt_data *hwrt_data = priv;
	struct pvr_hwrt_dataset *hwrt = hwrt_data->hwrt_dataset;
	struct rogue_fwif_hwrtdata_common *common = &hwrt->common;
	struct ddk19_fwif_hwrtdata *dst = cpu_ptr;

	dst->pm_mlist_dev_addr = hwrt_data->data.pm_mlist_dev_addr;
	for (int i = 0; i < ARRAY_SIZE(dst->freelists_fw_addr); i++)
		dst->freelists_fw_addr[i] = hwrt_data->data.freelists_fw_addr[i];
	pvr_fw_object_get_fw_addr(hwrt->common_fw_obj, &dst->parent_render_target_fw_addr);
	pvr_fw_object_get_fw_addr(hwrt_data->rta_ctl_obj, &dst->rta_ctl_fw_addr);
	dst->ppp_screen = common->screen_pixel_max;
	dst->ppp_multi_sample_ctl = common->multi_sample_ctl;
	dst->tpc_stride = common->tpc_stride;
	dst->tail_ptrs_dev_addr = hwrt_data->data.tail_ptrs_dev_addr;
	dst->tpc_size = common->tpc_size;
	dst->te_screen = common->te_screen;
	dst->mtile_stride = common->mtile_stride;
	dst->teaa = common->teaa;
	dst->te_mtile1 = common->te_mtile1;
	dst->te_mtile2 = common->te_mtile2;
	dst->isp_merge_lower_x = common->isp_merge_lower_x;
	dst->isp_merge_lower_y = common->isp_merge_lower_y;
	dst->isp_merge_upper_x = common->isp_merge_upper_x;
	dst->isp_merge_upper_y = common->isp_merge_upper_y;
	dst->isp_merge_scale_x = common->isp_merge_scale_x;
	dst->isp_merge_scale_y = common->isp_merge_scale_y;
}

int pvr_ddk19_hwrt_data_init(struct pvr_hwrt_data *hwrt_data)
{
	struct pvr_device *pvr_dev = hwrt_data->hwrt_dataset->pvr_dev;
	int err;

	err = pvr_fw_object_create(pvr_dev, sizeof(struct ddk19_fwif_rta_ctl),
				   PVR_BO_FW_FLAGS_DEVICE_UNCACHED, ddk19_rta_ctl_init,
				   &hwrt_data->data.rta_ctl, &hwrt_data->rta_ctl_obj);
	if (err)
		return err;

	err = pvr_fw_object_create(pvr_dev, sizeof(struct ddk19_fwif_hwrtdata),
				   PVR_BO_FW_FLAGS_DEVICE_UNCACHED, ddk19_hwrtdata_init,
				   hwrt_data, &hwrt_data->fw_obj);
	if (err)
		pvr_fw_object_destroy(hwrt_data->rta_ctl_obj);

	return err;
}

u32 pvr_ddk19_job_cmd_len(struct pvr_job *job)
{
	switch (job->type) {
	case DRM_PVR_JOB_TYPE_GEOMETRY:
		return sizeof(struct ddk19_fwif_cmd_ta);
	case DRM_PVR_JOB_TYPE_FRAGMENT:
		return sizeof(struct ddk19_fwif_cmd_3d);
	case DRM_PVR_JOB_TYPE_COMPUTE:
		return sizeof(struct ddk19_fwif_cmd_compute);
	default:
		return job->cmd_len;
	}
}

static u32 ddk19_ta_cmd(struct pvr_job *job, struct ddk19_fwif_cmd_ta *cmd)
{
	const struct rogue_fwif_cmd_geom *in = job->cmd;
	const struct rogue_fwif_static_rendercontext_state *state =
		job->ctx->data + pvr_ddk19_static_state_offset(job->ctx->type);
	const struct rogue_fwif_geom_registers_caswitch *cs = &state->ctxswitch_regs[0];
	const struct rogue_fwif_hwrtdata *data = &job->hwrt->data;

	memset(cmd, 0, sizeof(*cmd));
	cmd->pm_mtile_array = data->macrotile_array_dev_addr;
	cmd->ppp_ctrl = in->regs.ppp_ctrl;
	cmd->ppp_wclamp = DDK19_PPP_WCLAMP;
	cmd->te_psg = in->regs.te_psg;
	cmd->te_psgregion_addr = data->rgn_header_dev_addr;
	cmd->vdm_ctrl_stream_base = in->regs.vdm_ctrl_stream_base;
	cmd->ta_rtc_addr_base = data->rtc_dev_addr;
	cmd->tpu_border_colour_table = in->regs.tpu_border_colour_table;
	cmd->tpu = in->regs.tpu;
	cmd->vdm_context_state_base_addr = cs->geom_reg_vdm_context_state_base_addr;
	cmd->ta_context_state_base_addr = cs->geom_reg_ta_context_state_base_addr;
	for (int i = 0; i < ARRAY_SIZE(cmd->ta_state); i++) {
		cmd->ta_state[i].store_task[0] = cs->geom_state[i].geom_reg_vdm_context_store_task0;
		cmd->ta_state[i].store_task[1] = cs->geom_state[i].geom_reg_vdm_context_store_task1;
		cmd->ta_state[i].store_task[2] = cs->geom_state[i].geom_reg_vdm_context_store_task2;
		cmd->ta_state[i].resume_task[0] = cs->geom_state[i].geom_reg_vdm_context_resume_task0;
		cmd->ta_state[i].resume_task[1] = cs->geom_state[i].geom_reg_vdm_context_resume_task1;
		cmd->ta_state[i].resume_task[2] = cs->geom_state[i].geom_reg_vdm_context_resume_task2;
	}
	cmd->flags = in->flags & ROGUE_GEOM_FLAGS_LASTKICK;
	cmd->hwrt_data_fw_addr = in->cmd_shared.hwrt_data_fw_addr;
	cmd->dummy_rgnhdr_te_psgregion_addr = DDK19_MMU_INIA_BRN_65273_BASE;

	return sizeof(*cmd);
}

static u32 ddk19_3d_cmd(struct pvr_job *job, struct ddk19_fwif_cmd_3d *cmd)
{
	const struct rogue_fwif_cmd_frag *in = job->cmd;
	const struct rogue_fwif_frag_regs *regs = &in->regs;
	const struct rogue_fwif_hwrtdata_common *common = &job->hwrt->hwrt_dataset->common;
	const u32 flags = ROGUE_FRAG_FLAGS_DEPTHBUFFER | ROGUE_FRAG_FLAGS_STENCILBUFFER |
			  ROGUE_FRAG_FLAGS_DISABLE_PIXELMERGE | ROGUE_FRAG_FLAGS_SCRATCHBUFFER;

	memset(cmd, 0, sizeof(*cmd));
	cmd->usc_pixel_output_ctrl = regs->usc_pixel_output_ctrl;
	for (int i = 0; i < ARRAY_SIZE(cmd->usc_clear_register); i++)
		cmd->usc_clear_register[i] = regs->usc_clear_register[i];
	cmd->isp_mtile_base = job->hwrt->data.rgn_header_dev_addr;
	cmd->isp_mtile_size = common->isp_mtile_size;
	cmd->isp_multisamplectl = common->multi_sample_ctl;
	cmd->isp_scissor_base = regs->isp_scissor_base;
	cmd->isp_dbias_base = regs->isp_dbias_base;
	cmd->isp_bgobjdepth = regs->isp_bgobjdepth;
	cmd->isp_bgobjvals = regs->isp_bgobjvals;
	cmd->isp_aa = regs->isp_aa;
	cmd->isp_oclqry_base = regs->isp_oclqry_base;
	cmd->isp_zlsctl = regs->isp_zlsctl;
	cmd->isp_zload_base = regs->isp_zload_store_base;
	cmd->isp_zstore_base = regs->isp_zload_store_base;
	cmd->isp_stencil_load_base = regs->isp_stencil_load_store_base;
	cmd->isp_stencil_store_base = regs->isp_stencil_load_store_base;
	cmd->isp_zls_pixels = regs->isp_zls_pixels;
	cmd->isp_rgn = common->rgn_header_size;
	cmd->isp_ctl = regs->isp_ctl;
	cmd->fb_cdc_zls = regs->fb_cdc_zls;
	cmd->event_pixel_pds_code = DDK19_PDS_EOT_OFFSET;
	cmd->event_pixel_pds_data = regs->event_pixel_pds_data;
	cmd->pr_event_pixel_pds_data = regs->event_pixel_pds_data;
	cmd->event_pixel_pds_info = regs->event_pixel_pds_info;
	memcpy(cmd->pbe_word, regs->pbe_word, sizeof(cmd->pbe_word));
	cmd->tpu_border_colour_table = regs->tpu_border_colour_table;
	cmd->tpu = regs->tpu;
	cmd->pds_bgnd[0] = regs->pds_bgnd[0];
	cmd->pds_bgnd[1] = regs->pds_bgnd[1];
	cmd->pds_bgnd[3] = regs->pds_bgnd[2];
	cmd->pds_pr_bgnd[0] = regs->pds_pr_bgnd[0];
	cmd->pds_pr_bgnd[1] = regs->pds_pr_bgnd[1];
	cmd->pds_pr_bgnd[3] = regs->pds_pr_bgnd[2];
	cmd->isp_dummy_stencil_store_base = regs->isp_dummy_stencil_store_base;
	cmd->flags = in->flags & flags;
	cmd->hwrt_data_fw_addr = in->cmd_shared.hwrt_data_fw_addr;
	cmd->zls_stride = in->zls_stride;
	cmd->sls_stride = in->sls_stride;

	return sizeof(*cmd);
}

u32 pvr_ddk19_job_cmd(struct pvr_job *job, void *out)
{
	switch (job->type) {
	case DRM_PVR_JOB_TYPE_GEOMETRY:
		return ddk19_ta_cmd(job, out);
	case DRM_PVR_JOB_TYPE_FRAGMENT:
		return ddk19_3d_cmd(job, out);
	case DRM_PVR_JOB_TYPE_COMPUTE: {
		const struct rogue_fwif_cmd_compute *in = job->cmd;
		const struct rogue_fwif_static_computecontext_state *state =
			job->ctx->data + pvr_ddk19_static_state_offset(job->ctx->type);
		const struct rogue_fwif_cdm_registers_cswitch *cs = &state->ctxswitch_regs;
		struct ddk19_fwif_cmd_compute *cmd = out;

		memset(cmd, 0, sizeof(*cmd));
		cmd->cdm_ctrl_stream_base = in->regs.cdm_ctrl_stream_base;
		cmd->pds_ctrl = DDK19_PDS_CTRL_MAX_NUM_CDM_TASKS;
		cmd->tpu_border_colour_table = in->regs.tpu_border_colour_table;
		cmd->tpu = in->regs.tpu;
		cmd->cdm_context_state_base_addr = in->regs.cdm_context_state_base_addr;
		cmd->cdm_context_pds0 = cs->cdmreg_cdm_context_pds0;
		cmd->cdm_context_pds1 = cs->cdmreg_cdm_context_pds1;
		cmd->cdm_terminate_pds = cs->cdmreg_cdm_terminate_pds;
		cmd->cdm_terminate_pds1 = cs->cdmreg_cdm_terminate_pds1;
		cmd->cdm_resume_pds0 = cs->cdmreg_cdm_resume_pds0;
		cmd->cdm_resume_pds1 = in->regs.cdm_resume_pds1;
		return sizeof(*cmd);
	}
	default:
		memcpy(out, job->cmd, job->cmd_len);
		return job->cmd_len;
	}
}



static int pvr_ddk19_pow_request(struct pvr_device *pvr_dev, u32 pow_type, u32 request,
				 u32 pow_state)
{
	struct pvr_ddk19 *ddk19 = pvr_dev->ddk19;
	struct ddk19_fwif_kccb_cmd cmd = {
		.cmd_type = DDK19_KCCB_CMD_POW,
		.data.pow = { .pow_type = pow_type, .request = request },
	};
	u32 v;
	int err;

	WRITE_ONCE(*ddk19->power_sync, 0);
	err = pvr_ddk19_kccb_send(pvr_dev, &cmd);
	if (err)
		return err;

	err = read_poll_timeout(READ_ONCE, v, v != 0, 100, 1000000, false, *ddk19->power_sync);
	if (err)
		return err;

	return READ_ONCE(ddk19->tracebuf->pow_state) == pow_state ? 0 : -EBUSY;
}

int pvr_ddk19_fw_disable(struct pvr_device *pvr_dev, bool hard_reset, bool rpm_suspend)
{
	int err;

	if (!hard_reset) {
		u32 pow_state = READ_ONCE(pvr_dev->ddk19->tracebuf->pow_state);

		if (pow_state != DDK19_POW_STATE_FORCED_IDLE && pow_state != DDK19_POW_STATE_OFF) {
			err = pvr_ddk19_pow_request(pvr_dev, DDK19_POW_FORCED_IDLE_REQ,
						    DDK19_POWER_FORCE_IDLE,
						    DDK19_POW_STATE_FORCED_IDLE);
			if (err)
				return err;
		}

		err = pvr_ddk19_pow_request(pvr_dev, DDK19_POW_OFF_REQ, 1, DDK19_POW_STATE_OFF);
		if (err)
			return err;
	}

	if (rpm_suspend)
		disable_irq(pvr_dev->irq);

	err = pvr_fw_stop(pvr_dev);
	if (err && rpm_suspend)
		enable_irq(pvr_dev->irq);

	return err;
}

int pvr_ddk19_fw_enable(struct pvr_device *pvr_dev, bool rpm_resume)
{
	int err;

	if (rpm_resume)
		enable_irq(pvr_dev->irq);

	WRITE_ONCE(pvr_dev->ddk19->init->firmware_started, 0);

	err = pvr_fw_start(pvr_dev);
	if (!err) {
		err = pvr_ddk19_wait_for_boot(pvr_dev);
		if (err)
			pvr_fw_stop(pvr_dev);
	}

	if (err && rpm_resume)
		disable_irq(pvr_dev->irq);

	return err;
}

bool pvr_ddk19_is_idle(struct pvr_device *pvr_dev)
{
	return READ_ONCE(pvr_dev->ddk19->tracebuf->pow_state) == DDK19_POW_STATE_IDLE &&
	       pvr_ddk19_kccb_is_idle(pvr_dev);
}

void pvr_ddk19_process_irq(struct pvr_device *pvr_dev)
{
	struct pvr_ddk19 *ddk19 = pvr_dev->ddk19;
	struct ddk19_fwif_ccb_ctl *ctl = ddk19->fwccb_ctl;
	u32 read_offset = READ_ONCE(ctl->read_offset);

	while (read_offset != READ_ONCE(ctl->write_offset)) {
		struct ddk19_fwif_fwccb_cmd *cmd =
			(void *)ddk19->fwccb + read_offset * DDK19_FWCCB_CMD_SIZE;

		dev_info_ratelimited(from_pvr_device(pvr_dev)->dev, "ddk19: FWCCB command %u %*ph\n",
				     READ_ONCE(cmd->cmd_type), (int)sizeof(cmd->data), cmd->data);
		read_offset = (read_offset + 1) & ctl->wrap_mask;
		WRITE_ONCE(ctl->read_offset, read_offset);
	}

	if (pvr_ddk19_is_idle(pvr_dev))
		pm_request_autosuspend(from_pvr_device(pvr_dev)->dev);
}

void pvr_ddk19_fw_fini(struct pvr_device *pvr_dev)
{
	struct pvr_fw_device *fw_dev = &pvr_dev->fw_dev;

	WRITE_ONCE(fw_dev->initialised, false);

	pvr_ddk19_destroy_structures(pvr_dev);
	pvr_fw_cleanup(pvr_dev);
	drm_mm_takedown(&fw_dev->fw_mm);
	fw_dev->defs->fini(pvr_dev);
}

static int pvr_ddk19_state_show(struct seq_file *m, void *data)
{
	struct pvr_device *pvr_dev = m->private;
	struct pvr_ddk19 *ddk19 = pvr_dev->ddk19;
	struct ddk19_fwif_tracebuf *tb = ddk19->tracebuf;
	struct ddk19_fwif_assertbuf *ab = &tb->trace_buf[0].assert_buf;
	u32 ptr = READ_ONCE(tb->trace_buf[0].trace_pointer);

	seq_printf(m, "pow_state %u\nkccb %u/%u\nfwccb %u/%u\nkccb_cmds_executed %u\n",
		   READ_ONCE(tb->pow_state), READ_ONCE(ddk19->kccb_ctl->read_offset),
		   READ_ONCE(ddk19->kccb_ctl->write_offset), READ_ONCE(ddk19->fwccb_ctl->read_offset),
		   READ_ONCE(ddk19->fwccb_ctl->write_offset), READ_ONCE(tb->kccb_cmds_executed));
	seq_printf(m, "interrupts %u\nhwr_counter %u\ntrace_pointer %u\n",
		   READ_ONCE(tb->interrupt_count[0]), READ_ONCE(tb->hwr_counter), ptr);
	if (ab->path[0] || ab->info[0])
		seq_printf(m, "assert %.200s:%u: %.200s\n", ab->path, ab->line_num, ab->info);
	for (u32 i = 0; i < DDK19_HWRINFO_COUNT; i++) {
		const u8 *e = ddk19->hwrinfobuf + i * DDK19_HWRINFO_SIZE;
		const u64 *data = (const u64 *)e;

		if (!*(const u32 *)(e + 52))
			continue;
		seq_printf(m, "hwr %u: type %u dm %u event 0x%08x flags 0x%08x data %016llx %016llx %016llx\n",
			   *(const u32 *)(e + 52), *(const u32 *)(e + 64), *(const u32 *)(e + 68),
			   *(const u32 *)(e + 56), *(const u32 *)(e + 60), data[0], data[1], data[2]);
	}
	for (u32 i = 0; i < DDK19_TRACE_BUFFER_SIZE; i++)
		seq_printf(m, "%08x%c", READ_ONCE(ddk19->trace[i]), (i % 8 == 7) ? '\n' : ' ');

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pvr_ddk19_state);

static int pvr_ddk19_sync_set(void *data, u64 val)
{
	struct pvr_device *pvr_dev = data;
	struct device *dev = from_pvr_device(pvr_dev)->dev;
	int err;

	err = pm_runtime_resume_and_get(dev);
	if (err)
		return err;

	err = pvr_ddk19_sync(pvr_dev);
	pm_runtime_put_autosuspend(dev);

	return err;
}
DEFINE_DEBUGFS_ATTRIBUTE(pvr_ddk19_sync_fops, NULL, pvr_ddk19_sync_set, "%llu\n");

static int pvr_ddk19_nmi_show(struct seq_file *m, void *data)
{
	static const char * const names[] = { "ErrorEPC", "Status", "Cause", "BadVAddr", "EPC",
					      "SP", "Debug", "DEPC", "BadInstr" };
	struct pvr_device *pvr_dev = m->private;
	struct pvr_fw_object *data_obj = pvr_dev->fw_dev.mem.data_obj;
	const struct pvr_fw_layout_entry *boot_data;
	u32 *nmi, v;
	u8 *map;
	int err;

	boot_data = pvr_fw_find_layout_entry(pvr_dev, MIPS_BOOT_DATA);
	map = pvr_fw_object_vmap(data_obj);
	if (IS_ERR(map))
		return PTR_ERR(map);
	nmi = (u32 *)(map + boot_data->alloc_offset + ROGUE_MIPSFW_NMI_SHARED_DATA_BASE);

	WRITE_ONCE(nmi[ROGUE_MIPSFW_NMI_SYNC_FLAG_OFFSET], 0);
	pvr_cr_write64(pvr_dev, ROGUE_CR_MIPS_WRAPPER_NMI_ENABLE,
		       ROGUE_CR_MIPS_WRAPPER_NMI_ENABLE_EVENT_EN);
	pvr_cr_write32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_NMI_EVENT,
		       ROGUE_CR_MIPS_WRAPPER_NMI_EVENT_TRIGGER_EN);
	err = read_poll_timeout(pvr_cr_read32, v, v & ROGUE_CR_MIPS_EXCEPTION_STATUS_SI_NMI_TAKEN_EN,
				100, 100000, false, pvr_dev, ROGUE_CR_MIPS_EXCEPTION_STATUS);
	seq_printf(m, "exception_status 0x%08x%s\n", v, err ? " (NMI not taken)" : "");
	if (!err) {
		WRITE_ONCE(nmi[ROGUE_MIPSFW_NMI_SYNC_FLAG_OFFSET], 1);
		err = read_poll_timeout(pvr_cr_read32, v,
					!(v & ROGUE_CR_MIPS_EXCEPTION_STATUS_SI_ERL_EN), 100, 100000,
					false, pvr_dev, ROGUE_CR_MIPS_EXCEPTION_STATUS);
		for (int i = 0; i < ARRAY_SIZE(names); i++)
			seq_printf(m, "%s 0x%08x\n", names[i],
				   READ_ONCE(nmi[ROGUE_MIPSFW_NMI_STATE_OFFSET + i]));
	}
	pvr_cr_write32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_NMI_ENABLE, 0);
	pvr_fw_object_vunmap(data_obj);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pvr_ddk19_nmi);

void pvr_ddk19_debugfs_init(struct pvr_device *pvr_dev, struct dentry *root)
{
	struct dentry *dir = debugfs_create_dir("ddk19", root);

	debugfs_create_file("state", 0400, dir, pvr_dev, &pvr_ddk19_state_fops);
	debugfs_create_file("sync", 0200, dir, pvr_dev, &pvr_ddk19_sync_fops);
	debugfs_create_file("nmi", 0400, dir, pvr_dev, &pvr_ddk19_nmi_fops);
}

/* BRN 65273 needs the code, visibility test and transfer heaps in one 1 GiB PC entry each. */
const struct drm_pvr_heap pvr_ddk19_heaps[DRM_PVR_HEAP_COUNT] = {
	[DRM_PVR_HEAP_GENERAL] = {
		.base = 0x65C0000000ull,
		.size = SZ_2G,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_PDS_CODE_DATA] = {
		.base = DDK19_PDSCODEDATA_BRN_65273_HEAP_BASE,
		.size = SZ_1G,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_USC_CODE] = {
		.base = DDK19_USCCODE_BRN_65273_HEAP_BASE,
		.size = SZ_1G,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_RGNHDR] = {
		.base = 0xDBF0000000ull,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_VIS_TEST] = {
		.base = 0xE400000000ull,
		.size = SZ_1M,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	/* 1.9 starts this heap at 0, which Mesa treats as an invalid address. */
	[DRM_PVR_HEAP_TRANSFER_FRAG] = {
		.base = SZ_2M,
		.size = SZ_1G - SZ_2M,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
};

bool pvr_ddk19_va_reserved(struct pvr_device *pvr_dev, u64 device_addr, u64 size)
{
	static const u64 base[] = { DDK19_MMU_INIA_BRN_65273_BASE, DDK19_MMU_INIB_BRN_65273_BASE };

	if (!pvr_dev->device_data->ddk19)
		return false;

	for (int i = 0; i < ARRAY_SIZE(base); i++) {
		if (device_addr >= base[i] && device_addr + size <= base[i] + SZ_1G)
			return true;
	}

	return false;
}

int pvr_ddk19_vm_context_init(struct pvr_device *pvr_dev, struct pvr_vm_context *vm_ctx)
{
	static const u64 rgn_hdr_addr[] = { DDK19_MMU_INIA_BRN_65273_BASE,
					    DDK19_MMU_INIB_BRN_65273_BASE };
	struct pvr_gem_object *pvr_obj;
	int err = 0;

	/* BRN 65273: the GPU writes a region header at the start of both heaps. */
	pvr_obj = pvr_gem_object_create(pvr_dev, DDK19_BRN_65273_RGN_HDR_SIZE, 0);
	if (IS_ERR(pvr_obj))
		return PTR_ERR(pvr_obj);

	for (int i = 0; i < ARRAY_SIZE(rgn_hdr_addr) && !err; i++)
		err = pvr_vm_map(vm_ctx, pvr_obj, 0, rgn_hdr_addr[i], DDK19_BRN_65273_RGN_HDR_SIZE);

	pvr_gem_object_put(pvr_obj);

	return err;
}

int pvr_ddk19_fw_init(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct pvr_fw_device *fw_dev = &pvr_dev->fw_dev;
	struct ddk19_fwif_compchecks *cc;
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

	err = pvr_ddk19_wait_for_boot(pvr_dev);
	pvr_ddk19_dump_state(pvr_dev);
	if (err) {
		pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET, ROGUE_CR_SOFT_RESET__PBE2_XE__MASKFULL);
		(void)pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET);
		goto err_destroy_structures;
	}

	cc = &pvr_dev->ddk19->init->compchecks;
	drm_info(drm_dev, "ddk19: firmware %u.%u build %u for BVNC 22.%.8s.%llu.%llu started\n",
		 cc->ddk_version >> 16, cc->ddk_version & 0xffff, cc->ddk_build, cc->fw_bvnc.v,
		 (cc->fw_bvnc.bnc >> 16) & 0xffff, cc->fw_bvnc.bnc & 0xffff);

	WRITE_ONCE(fw_dev->initialised, true);

	return 0;

err_destroy_structures:
	pvr_ddk19_destroy_structures(pvr_dev);
err_fw_cleanup:
	pvr_fw_cleanup(pvr_dev);
err_mm_takedown:
	drm_mm_takedown(&fw_dev->fw_mm);
	fw_dev->defs->fini(pvr_dev);

	return err;
}
