// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/bluetooth/core/bt-host/gap/interrogator.h"

#include <zircon/assert.h>

#include "src/connectivity/bluetooth/core/bt-host/gap/peer.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/transport.h"

namespace bt::gap {

Interrogator::Interrogation::Interrogation(PeerId peer_id, hci_spec::ConnectionHandle handle,
                                           ResultCallback result_cb)
    : peer_id_(peer_id),
      handle_(handle),
      result_cb_(std::move(result_cb)),
      weak_ptr_factory_(this) {
  ZX_ASSERT(result_cb_);
}

Interrogator::Interrogation::~Interrogation() {
  // Interrogation may have already completed if there was an error.
  // Otherwise, complete with success because all commands completed.
  Complete(hci::Status());
  ZX_ASSERT_MSG(!result_cb_, "interrogation result callback never called");
}

void Interrogator::Interrogation::Complete(hci::Status status) {
  // Each interrogation step may fail but only invoke the status callback once, for the earliest
  // success/failure encountered.
  if (!result_cb_) {
    return;
  }

  bt_log(DEBUG, "gap", "interrogation completed (status: %s, peer: %s)", bt_str(status),
         bt_str(peer_id_));

  result_cb_(status);
}

Interrogator::Interrogator(PeerCache* cache, fxl::WeakPtr<hci::Transport> hci)
    : hci_(std::move(hci)), cache_(cache), weak_ptr_factory_(this) {
  ZX_ASSERT(hci_);
  ZX_ASSERT(cache_);
}

Interrogator::~Interrogator() {
  while (!pending_.empty()) {
    auto peer_id = pending_.begin()->first;
    // Result callback will erase this pending interrogation.
    Cancel(peer_id);
  }
}

void Interrogator::Start(PeerId peer_id, hci_spec::ConnectionHandle handle,
                         ResultCallback result_cb) {
  ZX_ASSERT(result_cb);

  auto self = weak_ptr_factory_.GetWeakPtr();
  auto result_cb_wrapped = [self, peer_id, cb = std::move(result_cb)](hci::Status status) mutable {
    if (!self) {
      bt_log(DEBUG, "gap",
             "Interrogator already destroyed in interrogation result callback (peer id: %s)",
             bt_str(peer_id));
      return;
    }
    auto node = self->pending_.extract(peer_id);
    cb(status);
  };

  auto interrogation_ref =
      fbl::AdoptRef(new Interrogation(peer_id, handle, std::move(result_cb_wrapped)));

  auto [it, inserted] = pending_.try_emplace(peer_id, interrogation_ref->GetWeakPtr());
  ZX_ASSERT_MSG(inserted, "interrogating peer %s twice at once", bt_str(peer_id));

  bt_log(TRACE, "gap", "started interrogating peer %s", bt_str(peer_id));

  SendCommands(std::move(interrogation_ref));
}

void Interrogator::Cancel(PeerId peer_id) {
  auto it = pending_.find(peer_id);
  if (it == pending_.end()) {
    return;
  }

  // Result callback will remove Interrogation from |pending_|.
  it->second->Complete(hci::Status(HostError::kCanceled));
}

void Interrogator::ReadRemoteVersionInformation(InterrogationRefPtr interrogation) {
  auto packet = hci::CommandPacket::New(hci_spec::kReadRemoteVersionInfo,
                                        sizeof(hci_spec::ReadRemoteVersionInfoCommandParams));
  packet->mutable_payload<hci_spec::ReadRemoteVersionInfoCommandParams>()->connection_handle =
      htole16(interrogation->handle());

  auto cmd_cb = [self = weak_ptr_factory_.GetWeakPtr(), interrogation](
                    auto id, const hci::EventPacket& event) {
    if (!self) {
      return;
    }

    if (!interrogation->active()) {
      return;
    }

    if (hci_is_error(event, WARN, "gap", "read remote version info failed")) {
      interrogation->Complete(event.ToStatus());
      return;
    }

    if (event.event_code() == hci_spec::kCommandStatusEventCode) {
      return;
    }

    ZX_ASSERT(event.event_code() == hci_spec::kReadRemoteVersionInfoCompleteEventCode);

    bt_log(TRACE, "gap", "read remote version info completed (peer id: %s)",
           bt_str(interrogation->peer_id()));

    const auto params = event.params<hci_spec::ReadRemoteVersionInfoCompleteEventParams>();

    Peer* peer = self->peer_cache()->FindById(interrogation->peer_id());
    if (!peer) {
      interrogation->Complete(hci::Status(HostError::kFailed));
      return;
    }
    peer->set_version(params.lmp_version, params.manufacturer_name, params.lmp_subversion);
  };

  bt_log(TRACE, "gap", "asking for version info (peer id: %s)", bt_str(interrogation->peer_id()));
  hci()->command_channel()->SendCommand(std::move(packet), std::move(cmd_cb),
                                        hci_spec::kReadRemoteVersionInfoCompleteEventCode);
}

}  // namespace bt::gap
