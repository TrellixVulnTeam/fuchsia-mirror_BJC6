// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>

#include <ddk/metadata/power.h>
#include <soc/aml-common/aml-power.h>
#include <soc/aml-t931/t931-gpio.h>
#include <soc/aml-t931/t931-power.h>
#include <soc/aml-t931/t931-pwm.h>

#include "sherlock.h"
#include "src/devices/board/drivers/sherlock/luis-0p8-ee-buck-bind.h"
#include "src/devices/board/drivers/sherlock/luis-cpu-a-buck-bind.h"
#include "src/devices/board/drivers/sherlock/luis-power-domain-bind.h"
#include "src/devices/board/drivers/sherlock/luis-power-impl-bind.h"
#include "src/devices/lib/fidl-metadata/i2c.h"

namespace sherlock {
using i2c_channel_t = fidl_metadata::i2c::Channel;
namespace {

constexpr uint32_t kPwmDFn = 3;

constexpr aml_voltage_table_t kT931VoltageTable[] = {
    {1'022'000, 0}, {1'011'000, 3}, {1'001'000, 6}, {991'000, 10}, {981'000, 13}, {971'000, 16},
    {961'000, 20},  {951'000, 23},  {941'000, 26},  {931'000, 30}, {921'000, 33}, {911'000, 36},
    {901'000, 40},  {891'000, 43},  {881'000, 46},  {871'000, 50}, {861'000, 53}, {851'000, 56},
    {841'000, 60},  {831'000, 63},  {821'000, 67},  {811'000, 70}, {801'000, 73}, {791'000, 76},
    {781'000, 80},  {771'000, 83},  {761'000, 86},  {751'000, 90}, {741'000, 93}, {731'000, 96},
    {721'000, 100},
};

constexpr voltage_pwm_period_ns_t kT931PwmPeriodNs = 1250;

const pbus_metadata_t power_impl_metadata[] = {
    {
        .type = DEVICE_METADATA_AML_VOLTAGE_TABLE,
        .data_buffer = reinterpret_cast<const uint8_t*>(&kT931VoltageTable),
        .data_size = sizeof(kT931VoltageTable),
    },
    {
        .type = DEVICE_METADATA_AML_PWM_PERIOD_NS,
        .data_buffer = reinterpret_cast<const uint8_t*>(&kT931PwmPeriodNs),
        .data_size = sizeof(kT931PwmPeriodNs),
    },
};

zx_device_prop_t power_domain_arm_core_props[] = {
    {BIND_POWER_DOMAIN_COMPOSITE, 0, PDEV_DID_POWER_DOMAIN_COMPOSITE},
};

constexpr power_domain_t big_domain[] = {
    {static_cast<uint32_t>(T931PowerDomains::kArmCoreBig)},
};

constexpr device_metadata_t power_domain_big_core[] = {
    {
        .type = DEVICE_METADATA_POWER_DOMAINS,
        .data = &big_domain,
        .length = sizeof(big_domain),
    },
};

constexpr composite_device_desc_t power_domain_big_core_desc = {
    .props = power_domain_arm_core_props,
    .props_count = countof(power_domain_arm_core_props),
    .fragments = luis_power_domain_fragments,
    .fragments_count = countof(luis_power_domain_fragments),
    .primary_fragment = "power",
    .spawn_colocated = true,
    .metadata_list = power_domain_big_core,
    .metadata_count = countof(power_domain_big_core),
};

constexpr power_domain_t little_domain[] = {
    {static_cast<uint32_t>(T931PowerDomains::kArmCoreLittle)},
};

constexpr device_metadata_t power_domain_little_core[] = {
    {
        .type = DEVICE_METADATA_POWER_DOMAINS,
        .data = &little_domain,
        .length = sizeof(little_domain),
    },
};

constexpr composite_device_desc_t power_domain_little_core_desc = {
    .props = power_domain_arm_core_props,
    .props_count = countof(power_domain_arm_core_props),
    .fragments = luis_power_domain_fragments,
    .fragments_count = countof(luis_power_domain_fragments),
    .primary_fragment = "power",
    .spawn_colocated = true,
    .metadata_list = power_domain_little_core,
    .metadata_count = countof(power_domain_little_core),
};

}  // namespace

static const pbus_dev_t power_dev = []() {
  pbus_dev_t dev = {};
  dev.name = "aml-power-impl-composite";
  dev.vid = PDEV_VID_GOOGLE;
  dev.pid = PDEV_PID_LUIS;
  dev.did = PDEV_DID_AMLOGIC_POWER;
  dev.metadata_list = power_impl_metadata;
  dev.metadata_count = countof(power_impl_metadata);
  return dev;
}();

zx_status_t Sherlock::LuisPowerPublishBuck(const char* name, uint32_t bus_id, uint16_t address,
                                           const device_fragment_t* fragments,
                                           size_t fragments_count) {
  const zx_device_prop_t props[] = {
      {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_SILERGY},
      {BIND_PLATFORM_DEV_PID, 0, PDEV_PID_SILERGY_SYBUCK},
  };

  const i2c_channel_t i2c_channels[] = {{
      .bus_id = bus_id,
      .address = address,
  }};

  auto i2c_status = fidl_metadata::i2c::I2CChannelsToFidl(i2c_channels);
  if (i2c_status.is_error()) {
    zxlogf(ERROR, "%s: failed to fidl encode i2c channels: %d", __func__, i2c_status.error_value());
    return i2c_status.error_value();
  }

  auto& data = i2c_status.value();

  const device_metadata_t metadata[] = {
      {
          .type = DEVICE_METADATA_I2C_CHANNELS,
          .data = data.data(),
          .length = data.size(),
      },
  };

  const composite_device_desc_t comp_desc = {
      .props = props,
      .props_count = countof(props),
      .fragments = fragments,
      .fragments_count = fragments_count,
      .primary_fragment = "i2c",
      .spawn_colocated = true,
      .metadata_list = metadata,
      .metadata_count = countof(metadata),
  };

  return DdkAddComposite(name, &comp_desc);
}

zx_status_t Sherlock::LuisPowerInit() {
  zx_status_t st;

  // Configure the GPIO to be Output & set it to alternate
  // function 3 which puts in PWM_D mode. A53 cluster (Small)
  gpio_impl_.SetAltFunction(T931_GPIOE(1), kPwmDFn);

  st = gpio_impl_.ConfigOut(T931_GPIOE(1), 0);
  if (st != ZX_OK) {
    zxlogf(ERROR, "%s: ConfigOut failed: %d", __func__, st);
    return st;
  }

  st = LuisPowerPublishBuck("0p8_ee_buck", SHERLOCK_I2C_A0_0, 0x60, ee_buck_fragments,
                            countof(ee_buck_fragments));
  if (st != ZX_OK) {
    zxlogf(ERROR, "Failed to publish sy8827 0P8_EE_BUCK device, st = %d", st);
    return st;
  }

  st = LuisPowerPublishBuck("cpu_a_buck", SHERLOCK_I2C_3, 0x60, cpu_a_buck_fragments,
                            countof(cpu_a_buck_fragments));
  if (st != ZX_OK) {
    zxlogf(ERROR, "Failed to publish sy8827 CPU_A_BUCK device, st = %d", st);
    return st;
  }

  st = pbus_.AddComposite(&power_dev, reinterpret_cast<uint64_t>(luis_power_impl_fragments),
                          countof(luis_power_impl_fragments), "pdev");
  if (st != ZX_OK) {
    zxlogf(ERROR, "%s: AddComposite for powerimpl failed, st = %d", __FUNCTION__, st);
    return st;
  }

  st = DdkAddComposite("composite-pd-big-core", &power_domain_big_core_desc);
  if (st != ZX_OK) {
    zxlogf(ERROR, "%s: CompositeDeviceAdd for power domain Big Arm Core failed, st = %d",
           __FUNCTION__, st);
    return st;
  }

  st = DdkAddComposite("composite-pd-little-core", &power_domain_little_core_desc);
  if (st != ZX_OK) {
    zxlogf(ERROR, "%s: CompositeDeviceAdd for power domain Little Arm Core failed, st = %d",
           __FUNCTION__, st);
    return st;
  }

  return ZX_OK;
}

}  // namespace sherlock
