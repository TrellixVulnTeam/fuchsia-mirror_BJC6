// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_IFACE_DEVICE_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_IFACE_DEVICE_H_

#include <fuchsia/wlan/common/c/banjo.h>
#include <lib/ddk/device.h>
#include <zircon/types.h>

#include <mutex>

#include <ddk/hw/wlan/wlaninfo/c/banjo.h>
#include <wlan/protocol/mac.h>

namespace wlan {
namespace testing {

class IfaceDevice {
 public:
  IfaceDevice(zx_device_t* device, wlan_info_mac_role_t role);

  zx_device_t* zxdev() { return zxdev_; }

  zx_status_t Bind();
  void Unbind();
  void Release();

  zx_status_t Query(uint32_t options, wlanmac_info_t* info);
  void Stop();
  zx_status_t Start(const wlanmac_ifc_protocol_t* ifc, zx_handle_t* out_mlme_channel);
  zx_status_t SetChannel(uint32_t options, const wlan_channel_t* channel);

 private:
  zx_device_t* zxdev_;
  zx_device_t* parent_;

  std::mutex lock_;
  wlanmac_ifc_protocol_t ifc_ = {};

  wlan_info_mac_role_t role_;
};

}  // namespace testing
}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_IFACE_DEVICE_H_
