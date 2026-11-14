#ifndef _SYS_VENUS_LMD_WIFI_H_
#define _SYS_VENUS_LMD_WIFI_H_
#include <stdint.h>
/* PCIe/CNSS bring-up stages 1..24; DMA only behind the owned SMMU guard. */
void venus_lmd_wifi_init(void);
void venus_lmd_wifi_stop(void);
void venus_lmd_wifi_quiesce(void);
/* Answers for the independent ath11k object (via the vlmd bridge). */
unsigned venus_lmd_wifi_driver_mode(void);
void *venus_lmd_wifi_driver_memory(unsigned long size, unsigned long long *iova);
const uint8_t *venus_lmd_wifi_driver_file(const char *name, uint64_t *size);
void *venus_lmd_wifi_driver_bar(void);
uint32_t venus_lmd_wifi_register_read(uint32_t offset);
void venus_lmd_wifi_register_write(uint32_t offset, uint32_t value);
#endif
