/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef PVR_DDK19_H
#define PVR_DDK19_H

#include <linux/types.h>

struct pvr_device;
struct pvr_power_sequence_ops;

extern const struct pvr_power_sequence_ops pvr_power_sequence_ops_ddk19;

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

#endif /* PVR_DDK19_H */
