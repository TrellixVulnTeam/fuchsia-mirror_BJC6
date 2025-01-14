/*
 * Copyright 2019 The Fuchsia Authors.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

// TODO(29700): Consolidate to one ieee80211.h

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_PLATFORM_IEEE80211_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_PLATFORM_IEEE80211_H_

#include <netinet/if_ether.h>
#include <stddef.h>
#include <stdint.h>

#include <ddk/hw/wlan/wlaninfo/c/banjo.h>
#include <wlan/protocol/ieee80211.h>

#if defined(__cplusplus)
extern "C" {
#endif  // defined(__cplusplus)

// The below constants are not defined in the 802.11-2016 Std.
#define IEEE80211_MAX_CHAINS 4
#define IEEE80211_MAX_RTS_THRESHOLD 2353
#define IEEE80211_MAC_PACKET_HEADROOM_SIZE 8

// Used as the default value in the data structure to indicate the queue is not set yet.
#define IEEE80211_INVAL_HW_QUEUE 0xff

// Convert the TID sequence number into the SSN (start sequence number) in the BAR (Block Ack
// Request).
#define IEEE80211_SCTL_SEQ_MASK 0xfff
#define IEEE80211_SCTL_SEQ_OFFSET 4
#define IEEE80211_SEQ_TO_SN(seq) (((seq) >> IEEE80211_SCTL_SEQ_OFFSET) & IEEE80211_SCTL_SEQ_MASK)

// The order of access categories is not clearly specified in 802.11-2016 Std.
// Therefore it cannot be moved into ieee80211 banjo file.
enum ieee80211_ac_numbers {
  IEEE80211_AC_VO = 0,
  IEEE80211_AC_VI = 1,
  IEEE80211_AC_BE = 2,
  IEEE80211_AC_BK = 3,
  IEEE80211_AC_MAX = 4,
};

enum ieee80211_frame_release_type {
  IEEE80211_FRAME_RELEASE_PSPOLL,
  IEEE80211_FRAME_RELEASE_UAPSD,
};

// IEEE Std 802.11-2016, 9.4.2.56.3, Table 9-163
enum ieee80211_max_ampdu_length_exp {
  IEEE80211_HT_MAX_AMPDU_8K = 0,
  IEEE80211_HT_MAX_AMPDU_16K = 1,
  IEEE80211_HT_MAX_AMPDU_32K = 2,
  IEEE80211_HT_MAX_AMPDU_64K = 3
};

enum ieee80211_roc_type {
  IEEE80211_ROC_TYPE_NORMAL = 0,
  IEEE80211_ROC_TYPE_MGMT_TX,
};

enum ieee80211_rssi_event_data {
  RSSI_EVENT_HIGH,
  RSSI_EVENT_LOW,
};

enum ieee80211_smps_mode {
  IEEE80211_SMPS_AUTOMATIC,
  IEEE80211_SMPS_OFF,
  IEEE80211_SMPS_STATIC,
  IEEE80211_SMPS_DYNAMIC,
  IEEE80211_SMPS_NUM_MODES,
};

// NEEDS_PORTING: Below structures are only referenced in function prototype.
//                Doesn't need a dummy byte.
struct cfg80211_gtk_rekey_data;
struct cfg80211_nan_conf;
struct cfg80211_nan_func;
struct cfg80211_scan_request;
struct cfg80211_sched_scan_request;
struct cfg80211_wowlan;
struct ieee80211_key_conf;
struct ieee80211_sta_ht_cap;
struct ieee80211_rx_status;
struct ieee80211_scan_ies;
struct ieee80211_tdls_ch_sw_params;

// NEEDS_PORTING: Below structures are used in code but not ported yet.
// A dummy byte is required to suppress the C++ warning message for empty
// struct.
struct cfg80211_chan_def {
  char dummy;
};

struct ieee80211_hdr {
  char dummy;
};

struct ieee80211_ops {
  char dummy;
};

struct ieee80211_p2p_noa_desc {
  char dummy;
};

// Channel info. Attributes of a channel.
struct ieee80211_channel {
  wlan_info_band_t band;
  uint32_t center_freq;  // unit: MHz.
  uint16_t ch_num;       // channel number (starts from 1).
  uint32_t flags;
  int max_power;
};

struct ieee80211_supported_band {
  wlan_info_band_t band;
  struct ieee80211_channel* channels;
  int n_channels;
  uint16_t* bitrates;
  int n_bitrates;
};

struct ieee80211_tx_queue_params {
  uint16_t txop;
  uint16_t cw_min;
  uint16_t cw_max;
  uint8_t aifs;
};

struct ieee80211_tx_rate {
  char dummy;
};

struct ieee80211_txq;
struct ieee80211_sta {
  void* drv_priv;
  struct ieee80211_txq* txq[IEEE80211_TIDS_MAX + 1];
};

struct ieee80211_tx_info {
  void* driver_data[8];
};

struct ieee80211_txq {
  void* drv_priv;
};

// TODO(43559): completely remove this structure from code.
struct ieee80211_vif {
  uint8_t dummy;
};

// Struct for transferring an IEEE 802.11 MAC-framed packet around the driver.
struct ieee80211_mac_packet {
  // The common portion of the MAC header.
  const struct ieee80211_frame_header* common_header;

  // The size of the entire MAC header (starting at common_header), including variable fields.
  size_t header_size;

  // Statically allocated headroom space between the MAC header and frame body, for adding
  // additional headers to the packet.
  uint8_t headroom[IEEE80211_MAC_PACKET_HEADROOM_SIZE];

  // Size of the headroom used.
  size_t headroom_used_size;

  // MAC frame body.
  const uint8_t* body;

  // MAC frame body size.
  size_t body_size;
};

size_t ieee80211_get_header_len(const struct ieee80211_frame_header* fh);

struct ieee80211_hw* ieee80211_alloc_hw(size_t priv_data_len, const struct ieee80211_ops* ops);

bool ieee80211_is_valid_chan(uint8_t primary);

uint16_t ieee80211_get_center_freq(uint8_t channel_num);

#if defined(__cplusplus)
}  // extern "C"
#endif  // defined(__cplusplus)

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_PLATFORM_IEEE80211_H_
