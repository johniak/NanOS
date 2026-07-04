/*
 * linuxkpi/include/linux/mfd/intel_soc_pmic.h — Intel SoC PMIC (Crystal Cove etc.).
 *
 * Inert on bring-up: no such PMIC exists on the Dell. This header also carries the
 * ACPI types into intel_dsi_vbt.c, which uses acpi_handle for the DSI panel's ACPI
 * device (the PMIC is an ACPI-enumerated device, so <linux/acpi.h> is the natural,
 * upstream-faithful carrier here).
 */
#ifndef _LKPI_MFD_INTEL_SOC_PMIC_H
#define _LKPI_MFD_INTEL_SOC_PMIC_H
#include <linux/acpi.h>
#endif
