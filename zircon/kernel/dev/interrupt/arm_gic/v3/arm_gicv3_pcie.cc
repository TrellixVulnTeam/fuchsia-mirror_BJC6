// Copyright 2018 The Fuchsia Authors
// Copyright (c) 2012-2015 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#if WITH_KERNEL_PCIE
#include <inttypes.h>
#include <lib/lazy_init/lazy_init.h>
#include <trace.h>
#include <zircon/boot/driver-config.h>
#include <zircon/types.h>

#include <dev/pcie_bus_driver.h>
#include <dev/pcie_platform.h>
#include <dev/pcie_root.h>
#include <lk/init.h>
#include <pdev/driver.h>
#include <pdev/interrupt.h>

static lazy_init::LazyInit<NoMsiPciePlatformInterface, lazy_init::CheckType::None,
                           lazy_init::Destructor::Disabled>
    g_platform_pcie_support;

static void arm_gicv3_pcie_init(const void* driver_data, uint32_t length) {
  ASSERT(length >= sizeof(dcfg_arm_gicv3_driver_t));
  __UNUSED const dcfg_arm_gicv3_driver_t* driver =
      reinterpret_cast<const dcfg_arm_gicv3_driver_t*>(driver_data);

  // When GICv3 MSI support is added, add a handler to register the deny
  // regions. Add all MMIO regions which contain GIC registers to the
  // system-wide deny list using root_resource_filter_add_deny_region.

  // Initialize the PCI platform, claiming no MSI support
  g_platform_pcie_support.Initialize();

  zx_status_t res = PcieBusDriver::InitializeDriver(g_platform_pcie_support.Get());
  if (res != ZX_OK) {
    TRACEF(
        "Failed to initialize PCI bus driver (res %d). "
        "PCI will be non-functional.\n",
        res);
  }
}

LK_PDEV_INIT(arm_gicv3_pcie_init, KDRV_ARM_GIC_V3, arm_gicv3_pcie_init, LK_INIT_LEVEL_PLATFORM)

#endif  // if WITH_KERNEL_PCIE
