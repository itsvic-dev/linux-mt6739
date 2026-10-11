/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef PVR_DDK19_H
#define PVR_DDK19_H

#include <drm/pvr_drm.h>
#include <linux/types.h>

#define PVR_DDK19_CCB_CMD_HEADER_SIZE 56
#define PVR_DDK19_JOB_CMD_MAX_SIZE 1024

struct dentry;
struct pvr_cccb;
struct pvr_device;
struct pvr_fw_object;
struct pvr_job;
struct pvr_vm_context;
struct pvr_power_sequence_ops;

extern const struct pvr_power_sequence_ops pvr_power_sequence_ops_ddk19;
extern const struct drm_pvr_heap pvr_ddk19_heaps[DRM_PVR_HEAP_COUNT];

char *pvr_ddk19_firmware_filename(struct pvr_device *pvr_dev);
void pvr_ddk19_init_device_info(struct pvr_device *pvr_dev);
int pvr_ddk19_fw_init(struct pvr_device *pvr_dev);
void pvr_ddk19_fw_fini(struct pvr_device *pvr_dev);
int pvr_ddk19_fw_disable(struct pvr_device *pvr_dev, bool hard_reset, bool rpm_suspend);
int pvr_ddk19_fw_enable(struct pvr_device *pvr_dev, bool rpm_resume);
bool pvr_ddk19_is_idle(struct pvr_device *pvr_dev);
void pvr_ddk19_process_irq(struct pvr_device *pvr_dev);
int pvr_ddk19_sync(struct pvr_device *pvr_dev);
int pvr_ddk19_mmu_flush(struct pvr_device *pvr_dev, u32 flags);
void pvr_ddk19_debugfs_init(struct pvr_device *pvr_dev, struct dentry *root);
int pvr_ddk19_cleanup(struct pvr_device *pvr_dev, u32 type, struct pvr_fw_object *fw_obj,
		      u32 offset);
int pvr_ddk19_kick(struct pvr_device *pvr_dev, u32 dm, u32 ctx_fw_addr, u32 write_offset);
void pvr_ddk19_cccb_write(struct pvr_cccb *cccb, u32 cmd_type, u32 cmd_size, void *cmd_data,
			  u32 ext_job_ref, u32 int_job_ref);
u32 pvr_ddk19_fw_ctx_size(enum drm_pvr_ctx_type type);
u32 pvr_ddk19_static_state_offset(enum drm_pvr_ctx_type type);
u32 pvr_ddk19_ctx_offset(enum drm_pvr_job_type type);
u32 pvr_ddk19_ctx_state_size(enum drm_pvr_job_type type);
u32 pvr_ddk19_rf_cmd_offset(enum drm_pvr_job_type type);
void pvr_ddk19_init_common_context(void *dst, u32 ccbctl_fw_addr, u32 ccb_fw_addr,
				   u32 fw_mem_ctx_fw_addr, u32 ctx_state_fw_addr,
				   u32 rf_cmd_fw_addr, u32 priority, u32 ctx_id, u32 dm);
void pvr_ddk19_free_list_init(void *dst, u64 dev_addr, u32 max_pages, u32 current_pages,
			      u32 grow_pages, u32 ready_pages, u32 id);
u32 pvr_ddk19_job_cmd_len(struct pvr_job *job);
u32 pvr_ddk19_job_cmd(struct pvr_job *job, void *out);
int pvr_ddk19_vm_context_init(struct pvr_device *pvr_dev, struct pvr_vm_context *vm_ctx);
bool pvr_ddk19_va_reserved(struct pvr_device *pvr_dev, u64 device_addr, u64 size);

#endif /* PVR_DDK19_H */
