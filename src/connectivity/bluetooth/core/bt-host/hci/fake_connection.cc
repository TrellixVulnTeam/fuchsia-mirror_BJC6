// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fake_connection.h"

namespace bt::hci::testing {

FakeConnection::FakeConnection(hci_spec::ConnectionHandle handle, bt::LinkType ll_type, Role role,
                               const DeviceAddress& local_address,
                               const DeviceAddress& peer_address)
    : Connection(handle, ll_type, role, local_address, peer_address),
      conn_state_(State::kConnected),
      weak_ptr_factory_(this) {}

void FakeConnection::TriggerEncryptionChangeCallback(Status status, bool enabled) {
  ZX_DEBUG_ASSERT(encryption_change_callback());
  encryption_change_callback()(status, enabled);
}

fxl::WeakPtr<Connection> FakeConnection::WeakPtr() { return weak_ptr_factory_.GetWeakPtr(); }

void FakeConnection::Disconnect(hci_spec::StatusCode reason) {
  // TODO(armansito): implement
  conn_state_ = State::kWaitingForDisconnectionComplete;
}

bool FakeConnection::StartEncryption() {
  start_encryption_count_++;
  return true;
}

}  // namespace bt::hci::testing
