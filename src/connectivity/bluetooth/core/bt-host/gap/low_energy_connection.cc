// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "low_energy_connection.h"

#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/async/time.h>

#include "low_energy_connection_manager.h"

namespace bt::gap::internal {

namespace {

constexpr const char* kInspectPeerIdPropertyName = "peer_id";
constexpr const char* kInspectPeerAddressPropertyName = "peer_address";
constexpr const char* kInspectRefsPropertyName = "ref_count";

// Connection parameters to use when the peer's preferred connection parameters are not known.
static const hci_spec::LEPreferredConnectionParameters kDefaultPreferredConnectionParameters(
    hci_spec::defaults::kLEConnectionIntervalMin, hci_spec::defaults::kLEConnectionIntervalMax,
    /*max_latency=*/0, hci_spec::defaults::kLESupervisionTimeout);

}  // namespace

LowEnergyConnection::LowEnergyConnection(PeerId peer_id, std::unique_ptr<hci::Connection> link,
                                         LowEnergyConnectionOptions connection_options,
                                         PeerDisconnectCallback peer_disconnect_cb,
                                         ErrorCallback error_cb,
                                         fxl::WeakPtr<LowEnergyConnectionManager> conn_mgr,
                                         fbl::RefPtr<l2cap::L2cap> l2cap,
                                         fxl::WeakPtr<gatt::GATT> gatt,
                                         fxl::WeakPtr<hci::Transport> transport)
    : peer_id_(peer_id),
      link_(std::move(link)),
      connection_options_(connection_options),
      conn_mgr_(std::move(conn_mgr)),
      l2cap_(std::move(l2cap)),
      gatt_(std::move(gatt)),
      transport_(std::move(transport)),
      peer_disconnect_callback_(std::move(peer_disconnect_cb)),
      error_callback_(std::move(error_cb)),
      refs_(/*convert=*/[](const auto& refs) { return refs.size(); }),
      weak_ptr_factory_(this) {
  ZX_ASSERT(peer_id_.IsValid());
  ZX_ASSERT(link_);
  ZX_ASSERT(conn_mgr_);
  ZX_ASSERT(gatt_);
  ZX_ASSERT(transport_);
  ZX_ASSERT(peer_disconnect_callback_);
  ZX_ASSERT(error_callback_);

  auto peer = conn_mgr_->peer_cache()->FindById(peer_id_);
  ZX_ASSERT(peer);
  peer_ = peer->GetWeakPtr();

  link_->set_peer_disconnect_callback(
      [this](auto, auto reason) { peer_disconnect_callback_(reason); });

  RegisterEventHandlers();
  StartConnectionPauseTimeout();
  InitializeFixedChannels();
}

LowEnergyConnection::~LowEnergyConnection() {
  transport_->command_channel()->RemoveEventHandler(conn_update_cmpl_handler_id_);

  // Unregister this link from the GATT profile and the L2CAP plane. This
  // invalidates all L2CAP channels that are associated with this link.
  gatt_->RemoveConnection(peer_id_);
  l2cap_->RemoveConnection(link_->handle());

  // Notify all active references that the link is gone. This will
  // synchronously notify all refs.
  CloseRefs();
}

std::unique_ptr<bt::gap::LowEnergyConnectionHandle> LowEnergyConnection::AddRef() {
  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> conn_ref(
      new LowEnergyConnectionHandle(peer_id_, handle(), conn_mgr_));
  ZX_ASSERT(conn_ref);

  refs_.Mutable()->insert(conn_ref.get());

  bt_log(DEBUG, "gap-le", "added ref (peer: %s, handle %#.4x, count: %lu)", bt_str(peer_id_),
         handle(), ref_count());

  return conn_ref;
}

void LowEnergyConnection::DropRef(LowEnergyConnectionHandle* ref) {
  ZX_DEBUG_ASSERT(ref);

  size_t res = refs_.Mutable()->erase(ref);
  ZX_ASSERT_MSG(res == 1u, "DropRef called with wrong connection reference");
  bt_log(DEBUG, "gap-le", "dropped ref (peer: %s, handle: %#.4x, count: %lu)", bt_str(peer_id_),
         handle(), ref_count());
}

// Registers this connection with L2CAP and initializes the fixed channel
// protocols.
void LowEnergyConnection::InitializeFixedChannels() {
  auto self = GetWeakPtr();
  // Ensure error_callback_ is only called once if link_error_cb is called multiple times.
  auto link_error_cb = [self]() {
    if (self && self->error_callback_) {
      self->error_callback_();
    }
  };
  auto update_conn_params_cb = [self](auto params) {
    if (self) {
      self->OnNewLEConnectionParams(params);
    }
  };
  auto security_upgrade_cb = [self](auto handle, auto level, auto cb) {
    if (!self) {
      return;
    }

    bt_log(
        INFO, "gap-le",
        "received security upgrade request on L2CAP channel (level: %s, peer: %s, handle: %#.4x)",
        sm::LevelToString(level), bt_str(self->peer_id_), handle);
    ZX_ASSERT(self->handle() == handle);
    self->OnSecurityRequest(level, std::move(cb));
  };
  l2cap::L2cap::LEFixedChannels fixed_channels =
      l2cap_->AddLEConnection(link_->handle(), link_->role(), std::move(link_error_cb),
                              update_conn_params_cb, security_upgrade_cb);

  OnL2capFixedChannelsOpened(std::move(fixed_channels.att), std::move(fixed_channels.smp),
                             connection_options_);
}

// Used to respond to protocol/service requests for increased security.
void LowEnergyConnection::OnSecurityRequest(sm::SecurityLevel level, sm::StatusCallback cb) {
  ZX_ASSERT(sm_);
  sm_->UpgradeSecurity(level, [cb = std::move(cb), peer_id = peer_id_, handle = handle()](
                                  sm::Status status, const auto& sp) {
    bt_log(INFO, "gap-le", "pairing status: %s, properties: %s (peer: %s, handle: %#.4x)",
           bt_str(status), bt_str(sp), bt_str(peer_id), handle);
    cb(status);
  });
}

// Handles a pairing request (i.e. security upgrade) received from "higher levels", likely
// initiated from GAP. This will only be used by pairing requests that are initiated
// in the context of testing. May only be called on an already-established connection.
void LowEnergyConnection::UpgradeSecurity(sm::SecurityLevel level, sm::BondableMode bondable_mode,
                                          sm::StatusCallback cb) {
  ZX_ASSERT(sm_);
  sm_->set_bondable_mode(bondable_mode);
  OnSecurityRequest(level, std::move(cb));
}

// Cancels any on-going pairing procedures and sets up SMP to use the provided
// new I/O capabilities for future pairing procedures.
void LowEnergyConnection::ResetSecurityManager(sm::IOCapability ioc) { sm_->Reset(ioc); }

void LowEnergyConnection::OnInterrogationComplete() {
  ZX_ASSERT(!interrogation_completed_);
  interrogation_completed_ = true;
  MaybeUpdateConnectionParameters();
}

void LowEnergyConnection::AttachInspect(inspect::Node& parent, std::string name) {
  inspect_node_ = parent.CreateChild(name);
  inspect_properties_.peer_id =
      inspect_node_.CreateString(kInspectPeerIdPropertyName, peer_id_.ToString());
  inspect_properties_.peer_address = inspect_node_.CreateString(
      kInspectPeerAddressPropertyName, link_.get() ? link_->peer_address().ToString() : "");
  refs_.AttachInspect(inspect_node_, kInspectRefsPropertyName);
}

void LowEnergyConnection::StartConnectionPauseTimeout() {
  if (link_->role() == hci::Connection::Role::kMaster) {
    StartConnectionPauseCentralTimeout();
  } else {
    StartConnectionPausePeripheralTimeout();
  }
}

void LowEnergyConnection::RegisterEventHandlers() {
  auto self = GetWeakPtr();
  conn_update_cmpl_handler_id_ = transport_->command_channel()->AddLEMetaEventHandler(
      hci_spec::kLEConnectionUpdateCompleteSubeventCode, [self](const auto& event) {
        if (self) {
          self->OnLEConnectionUpdateComplete(event);
          return hci::CommandChannel::EventCallbackResult::kContinue;
        }
        return hci::CommandChannel::EventCallbackResult::kRemove;
      });
}

// Connection parameter updates by the peripheral are not allowed until the central has been idle
// for kLEConnectionPauseCentral and kLEConnectionPausePeripheral has passed since the connection
// was established (Core Spec v5.2, Vol 3, Part C, Sec 9.3.12).
// TODO(fxbug.dev/79491): Wait to update connection parameters until all initialization
// procedures have completed.
void LowEnergyConnection::StartConnectionPausePeripheralTimeout() {
  ZX_ASSERT(!conn_pause_peripheral_timeout_.has_value());
  conn_pause_peripheral_timeout_.emplace([this]() {
    // Destroying this task will invalidate the capture list, so we need to save a self pointer.
    auto self = this;
    conn_pause_peripheral_timeout_.reset();
    self->MaybeUpdateConnectionParameters();
  });
  conn_pause_peripheral_timeout_->PostDelayed(async_get_default_dispatcher(),
                                              kLEConnectionPausePeripheral);
}

// Connection parameter updates by the central are not allowed until the central is idle and the
// peripheral has been idle for kLEConnectionPauseCentral (Core Spec v5.2, Vol 3, Part
// C, Sec 9.3.12).
// TODO(fxbug.dev/79491): Wait to update connection parameters until all initialization
// procedures have completed.
void LowEnergyConnection::StartConnectionPauseCentralTimeout() {
  ZX_ASSERT(!conn_pause_central_timeout_.has_value());
  conn_pause_central_timeout_.emplace([this]() {
    // Destroying this task will invalidate the capture list, so we need to save a self pointer.
    auto self = this;
    conn_pause_central_timeout_.reset();
    self->MaybeUpdateConnectionParameters();
  });
  conn_pause_central_timeout_->PostDelayed(async_get_default_dispatcher(),
                                           kLEConnectionPauseCentral);
}

void LowEnergyConnection::OnL2capFixedChannelsOpened(
    fbl::RefPtr<l2cap::Channel> att, fbl::RefPtr<l2cap::Channel> smp,
    LowEnergyConnectionOptions connection_options) {
  if (!att || !smp) {
    bt_log(INFO, "gap-le", "link was closed before opening fixed channels (peer: %s)",
           bt_str(peer_id_));
    return;
  }

  bt_log(DEBUG, "gap-le", "ATT and SMP fixed channels open (peer: %s)", bt_str(peer_id_));

  // Obtain existing pairing data, if any.
  std::optional<sm::LTK> ltk;
  auto* peer = conn_mgr_->peer_cache()->FindById(peer_id_);
  ZX_DEBUG_ASSERT_MSG(peer, "connected peer must be present in cache!");

  if (peer->le() && peer->le()->bond_data()) {
    // Legacy pairing allows both devices to generate and exchange LTKs. "The master device must
    // have the [...] (LTK, EDIV, and Rand) distributed by the slave device in LE legacy [...] to
    // setup an encrypted session" (V5.0 Vol. 3 Part H 2.4.4.2). For Secure Connections peer_ltk
    // and local_ltk will be equal, so this check is unnecessary but correct.
    ltk = (link()->role() == hci::Connection::Role::kMaster) ? peer->le()->bond_data()->peer_ltk
                                                             : peer->le()->bond_data()->local_ltk;
  }

  // Obtain the local I/O capabilities from the delegate. Default to
  // NoInputNoOutput if no delegate is available.
  auto io_cap = sm::IOCapability::kNoInputNoOutput;
  if (conn_mgr_->pairing_delegate()) {
    io_cap = conn_mgr_->pairing_delegate()->io_capability();
  }
  LeSecurityMode security_mode = conn_mgr_->security_mode();
  sm_ = conn_mgr_->sm_factory_func()(link_->WeakPtr(), std::move(smp), io_cap,
                                     weak_ptr_factory_.GetWeakPtr(),
                                     connection_options.bondable_mode, security_mode);

  // Provide SMP with the correct LTK from a previous pairing with the peer, if it exists. This
  // will start encryption if the local device is the link-layer master.
  if (ltk) {
    bt_log(INFO, "gap-le", "assigning existing LTK (peer: %s, handle: %#.4x)", bt_str(peer_id_),
           handle());
    sm_->AssignLongTermKey(*ltk);
  }

  InitializeGatt(std::move(att), connection_options.service_uuid);
}

void LowEnergyConnection::OnNewLEConnectionParams(
    const hci_spec::LEPreferredConnectionParameters& params) {
  bt_log(INFO, "gap-le", "LE connection parameters received (peer: %s, handle: %#.4x)",
         bt_str(peer_id_), link_->handle());

  ZX_ASSERT(peer_);

  peer_->MutLe().SetPreferredConnectionParameters(params);

  UpdateConnectionParams(params);
}

void LowEnergyConnection::RequestConnectionParameterUpdate(
    const hci_spec::LEPreferredConnectionParameters& params) {
  ZX_ASSERT_MSG(link_->role() == hci::Connection::Role::kSlave,
                "tried to send connection parameter update request as central");

  ZX_ASSERT(peer_);
  // Ensure interrogation has completed.
  ZX_ASSERT(peer_->le()->features().has_value());

  // TODO(fxbug.dev/49714): check local controller support for LL Connection Parameters Request
  // procedure (mask is currently in Adapter le state, consider propagating down)
  bool ll_connection_parameters_req_supported =
      peer_->le()->features()->le_features &
      static_cast<uint64_t>(hci_spec::LESupportedFeature::kConnectionParametersRequestProcedure);

  bt_log(TRACE, "gap-le", "ll connection parameters req procedure supported: %s",
         ll_connection_parameters_req_supported ? "true" : "false");

  if (ll_connection_parameters_req_supported) {
    auto self = weak_ptr_factory_.GetWeakPtr();
    auto status_cb = [self, params](hci::Status status) {
      if (!self) {
        return;
      }

      self->HandleRequestConnectionParameterUpdateCommandStatus(params, status);
    };

    UpdateConnectionParams(params, std::move(status_cb));
  } else {
    L2capRequestConnectionParameterUpdate(params);
  }
}

void LowEnergyConnection::HandleRequestConnectionParameterUpdateCommandStatus(
    hci_spec::LEPreferredConnectionParameters params, hci::Status status) {
  // The next LE Connection Update complete event is for this command iff the command |status|
  // is success.
  if (!status.is_success()) {
    if (status.protocol_error() == hci_spec::StatusCode::kUnsupportedRemoteFeature) {
      // Retry connection parameter update with l2cap if the peer doesn't support LL procedure.
      bt_log(
          INFO, "gap-le",
          "peer does not support HCI LE Connection Update command, trying l2cap request (peer: %s)",
          bt_str(peer_id_));
      L2capRequestConnectionParameterUpdate(params);
    }
    return;
  }

  // Note that this callback is for the Connection Update Complete event, not the Connection Update
  // status event, which is handled by the above code (see v5.2, Vol. 4, Part E 7.7.15 / 7.7.65.3).
  le_conn_update_complete_command_callback_ = [this, params](hci_spec::StatusCode status) {
    // Retry connection parameter update with l2cap if the peer doesn't support LL
    // procedure.
    if (status == hci_spec::StatusCode::kUnsupportedRemoteFeature) {
      bt_log(INFO, "gap-le",
             "peer does not support HCI LE Connection Update command, trying l2cap request "
             "(peer: %s)",
             bt_str(peer_id_));
      L2capRequestConnectionParameterUpdate(params);
    }
  };
}

void LowEnergyConnection::L2capRequestConnectionParameterUpdate(
    const hci_spec::LEPreferredConnectionParameters& params) {
  ZX_ASSERT_MSG(link_->role() == hci::Connection::Role::kSlave,
                "tried to send l2cap connection parameter update request as central");

  bt_log(DEBUG, "gap-le", "sending l2cap connection parameter update request (peer: %s)",
         bt_str(peer_id_));

  auto response_cb = [handle = handle(), peer_id = peer_id_](bool accepted) {
    if (accepted) {
      bt_log(DEBUG, "gap-le",
             "peer accepted l2cap connection parameter update request (peer: %s, handle: %#.4x)",
             bt_str(peer_id), handle);
    } else {
      bt_log(INFO, "gap-le",
             "peer rejected l2cap connection parameter update request (peer: %s, handle: %#.4x)",
             bt_str(peer_id), handle);
    }
  };

  // TODO(fxbug.dev/49717): don't send request until after kLEConnectionParameterTimeout of an
  // l2cap conn parameter update response being received (Core Spec v5.2, Vol 3, Part C,
  // Sec 9.3.9).
  l2cap_->RequestConnectionParameterUpdate(handle(), params, std::move(response_cb));
}

void LowEnergyConnection::UpdateConnectionParams(
    const hci_spec::LEPreferredConnectionParameters& params, StatusCallback status_cb) {
  bt_log(DEBUG, "gap-le", "updating connection parameters (peer: %s)", bt_str(peer_id_));
  auto command = hci::CommandPacket::New(hci_spec::kLEConnectionUpdate,
                                         sizeof(hci_spec::LEConnectionUpdateCommandParams));
  auto event_params = command->mutable_payload<hci_spec::LEConnectionUpdateCommandParams>();

  event_params->connection_handle = htole16(handle());
  event_params->conn_interval_min = htole16(params.min_interval());
  event_params->conn_interval_max = htole16(params.max_interval());
  event_params->conn_latency = htole16(params.max_latency());
  event_params->supervision_timeout = htole16(params.supervision_timeout());
  event_params->minimum_ce_length = 0x0000;
  event_params->maximum_ce_length = 0x0000;

  auto status_cb_wrapper = [handle = handle(), cb = std::move(status_cb)](
                               auto id, const hci::EventPacket& event) mutable {
    ZX_ASSERT(event.event_code() == hci_spec::kCommandStatusEventCode);
    hci_is_error(event, TRACE, "gap-le",
                 "controller rejected connection parameters (handle: %#.4x)", handle);
    if (cb) {
      cb(event.ToStatus());
    }
  };

  transport_->command_channel()->SendCommand(std::move(command), std::move(status_cb_wrapper),
                                             hci_spec::kCommandStatusEventCode);
}

void LowEnergyConnection::OnLEConnectionUpdateComplete(const hci::EventPacket& event) {
  ZX_ASSERT(event.event_code() == hci_spec::kLEMetaEventCode);
  ZX_ASSERT(event.params<hci_spec::LEMetaEventParams>().subevent_code ==
            hci_spec::kLEConnectionUpdateCompleteSubeventCode);

  auto payload = event.le_event_params<hci_spec::LEConnectionUpdateCompleteSubeventParams>();
  ZX_ASSERT(payload);
  hci_spec::ConnectionHandle handle = le16toh(payload->connection_handle);

  // Ignore events for other connections.
  if (handle != link_->handle()) {
    return;
  }

  // This event may be the result of the LE Connection Update command.
  if (le_conn_update_complete_command_callback_) {
    le_conn_update_complete_command_callback_(payload->status);
  }

  if (payload->status != hci_spec::StatusCode::kSuccess) {
    bt_log(WARN, "gap-le",
           "HCI LE Connection Update Complete event with error "
           "(peer: %s, status: %#.2x, handle: %#.4x)",
           bt_str(peer_id_), payload->status, handle);

    return;
  }

  bt_log(INFO, "gap-le", "conn. parameters updated (peer: %s)", bt_str(peer_id_));

  hci_spec::LEConnectionParameters params(le16toh(payload->conn_interval),
                                          le16toh(payload->conn_latency),
                                          le16toh(payload->supervision_timeout));
  link_->set_low_energy_parameters(params);

  ZX_ASSERT(peer_);
  peer_->MutLe().SetConnectionParameters(params);
}

void LowEnergyConnection::MaybeUpdateConnectionParameters() {
  if (connection_parameters_update_requested_ || conn_pause_central_timeout_ ||
      conn_pause_peripheral_timeout_ || !interrogation_completed_) {
    return;
  }

  connection_parameters_update_requested_ = true;

  if (link_->role() == hci::Connection::Role::kMaster) {
    // If the GAP service preferred connection parameters characteristic has not been read by now,
    // just use the default parameters.
    // TODO(fxbug.dev/66031): Wait for preferred connection parameters to be read.
    ZX_ASSERT(peer_);
    auto conn_params = peer_->le()->preferred_connection_parameters().value_or(
        kDefaultPreferredConnectionParameters);
    UpdateConnectionParams(conn_params);
  } else {
    RequestConnectionParameterUpdate(kDefaultPreferredConnectionParameters);
  }
}

void LowEnergyConnection::InitializeGatt(fbl::RefPtr<l2cap::Channel> att_channel,
                                         std::optional<UUID> service_uuid) {
  fbl::RefPtr<att::Bearer> att_bearer = att::Bearer::Create(att_channel);
  if (!att_bearer) {
    // This can happen if the link closes before the Bearer activates the
    // channel.
    bt_log(WARN, "gatt", "failed to initialize ATT bearer");
    // Post task to prevent calling error callback in constructor.
    async::PostTask(async_get_default_dispatcher(),
                    [att_channel] { att_channel->SignalLinkError(); });
    return;
  }
  std::unique_ptr<gatt::Client> gatt_client = gatt::Client::Create(att_bearer);
  gatt_->AddConnection(peer_id_, std::move(att_bearer), std::move(gatt_client));

  std::vector<UUID> service_uuids;
  if (service_uuid) {
    // TODO(fxbug.dev/65592): De-duplicate services.
    service_uuids = {*service_uuid, kGenericAccessService};
  }
  gatt_->DiscoverServices(peer_id_, std::move(service_uuids));

  auto self = weak_ptr_factory_.GetWeakPtr();
  gatt_->ListServices(peer_id_, {kGenericAccessService}, [self](auto status, auto services) {
    if (self) {
      self->OnGattServicesResult(status, std::move(services));
    }
  });
}

void LowEnergyConnection::OnGattServicesResult(att::Status status, gatt::ServiceList services) {
  if (bt_is_error(status, INFO, "gap-le", "error discovering GAP service (peer: %s)",
                  bt_str(peer_id_))) {
    return;
  }

  if (services.empty()) {
    // The GAP service is mandatory for both central and peripheral, so this is unexpected.
    bt_log(INFO, "gap-le", "GAP service not found (peer: %s)", bt_str(peer_id_));
    return;
  }

  auto* peer = conn_mgr_->peer_cache()->FindById(peer_id_);
  ZX_ASSERT_MSG(peer, "connected peer must be present in cache!");

  gap_service_client_.emplace(peer_id_, services.front());

  // TODO(fxbug.dev/65914): Read name and appearance characteristics.
  auto self = weak_ptr_factory_.GetWeakPtr();
  if (!peer->le()->preferred_connection_parameters().has_value()) {
    gap_service_client_->ReadPeripheralPreferredConnectionParameters([self](auto result) {
      if (!self) {
        return;
      }

      if (result.is_error()) {
        bt_log(INFO, "gap-le",
               "error reading peripheral preferred connection parameters (status:  %s, peer: %s)",
               bt_str(result.error()), bt_str(self->peer_id_));
        return;
      }
      auto params = result.value();

      auto* peer = self->conn_mgr_->peer_cache()->FindById(self->peer_id_);
      ZX_ASSERT_MSG(peer, "connected peer must be present in cache!");
      peer->MutLe().SetPreferredConnectionParameters(params);
    });
  }
}

void LowEnergyConnection::CloseRefs() {
  for (auto* ref : *refs_.Mutable()) {
    ref->MarkClosed();
  }

  refs_.Mutable()->clear();
}

void LowEnergyConnection::OnNewPairingData(const sm::PairingData& pairing_data) {
  const std::optional<sm::LTK> ltk =
      pairing_data.peer_ltk ? pairing_data.peer_ltk : pairing_data.local_ltk;
  // Consider the pairing temporary if no link key was received. This
  // means we'll remain encrypted with the STK without creating a bond and
  // reinitiate pairing when we reconnect in the future.
  if (!ltk.has_value()) {
    bt_log(INFO, "gap-le", "temporarily paired with peer (peer: %s)", bt_str(peer_id_));
    return;
  }

  bt_log(INFO, "gap-le", "new %s pairing data: [%s%s%s%s%s%s] (peer: %s)",
         ltk->security().secure_connections() ? "secure connections" : "legacy",
         pairing_data.peer_ltk ? "peer_ltk " : "", pairing_data.local_ltk ? "local_ltk " : "",
         pairing_data.irk ? "irk " : "", pairing_data.cross_transport_key ? "ct_key " : "",
         pairing_data.identity_address
             ? fxl::StringPrintf("(identity: %s) ", bt_str(*pairing_data.identity_address)).c_str()
             : "",
         pairing_data.csrk ? "csrk " : "", bt_str(peer_id_));

  if (conn_mgr_->peer_cache()->StoreLowEnergyBond(peer_id_, pairing_data)) {
    conn_mgr_->peer_cache()->LogLeBondingEvent(true);
  } else {
    conn_mgr_->peer_cache()->LogLeBondingEvent(false);
    bt_log(ERROR, "gap-le", "failed to cache bonding data (id: %s)", bt_str(peer_id_));
  }
}

void LowEnergyConnection::OnPairingComplete(sm::Status status) {
  bt_log(INFO, "gap-le", "pairing complete (status: %s, peer: %s)", bt_str(status),
         bt_str(peer_id_));

  auto delegate = conn_mgr_->pairing_delegate();
  if (delegate) {
    delegate->CompletePairing(peer_id_, status);
  }
}

void LowEnergyConnection::OnAuthenticationFailure(hci::Status status) {
  // TODO(armansito): Clear bonding data from the remote peer cache as any
  // stored link key is not valid.
  bt_log(WARN, "gap-le", "link layer authentication failed (status: %s, peer: %s)", bt_str(status),
         bt_str(peer_id_));
}

void LowEnergyConnection::OnNewSecurityProperties(const sm::SecurityProperties& sec) {
  bt_log(INFO, "gap-le", "new link security properties (properties: %s, peer: %s)", bt_str(sec),
         bt_str(peer_id_));
  // Update the data plane with the correct link security level.
  l2cap_->AssignLinkSecurityProperties(link_->handle(), sec);
}

std::optional<sm::IdentityInfo> LowEnergyConnection::OnIdentityInformationRequest() {
  if (!conn_mgr_->local_address_delegate()->irk()) {
    bt_log(TRACE, "gap-le", "no local identity information to exchange");
    return std::nullopt;
  }

  bt_log(DEBUG, "gap-le", "will distribute local identity information (peer: %s)",
         bt_str(peer_id_));
  sm::IdentityInfo id_info;
  id_info.irk = *conn_mgr_->local_address_delegate()->irk();
  id_info.address = conn_mgr_->local_address_delegate()->identity_address();

  return id_info;
}

void LowEnergyConnection::ConfirmPairing(ConfirmCallback confirm) {
  bt_log(INFO, "gap-le",
         "pairing delegate request for pairing confirmation w/ no passkey (peer: %s)",
         bt_str(peer_id_));

  auto* delegate = conn_mgr_->pairing_delegate();
  if (!delegate) {
    bt_log(ERROR, "gap-le", "rejecting pairing without a PairingDelegate! (peer: %s)",
           bt_str(peer_id_));
    confirm(false);
  } else {
    delegate->ConfirmPairing(peer_id_, std::move(confirm));
  }
}

void LowEnergyConnection::DisplayPasskey(uint32_t passkey, sm::Delegate::DisplayMethod method,
                                         ConfirmCallback confirm) {
  bt_log(INFO, "gap-le", "pairing delegate request (method: %s, peer: %s)",
         sm::util::DisplayMethodToString(method).c_str(), bt_str(peer_id_));

  auto* delegate = conn_mgr_->pairing_delegate();
  if (!delegate) {
    bt_log(ERROR, "gap-le", "rejecting pairing without a PairingDelegate!");
    confirm(false);
  } else {
    delegate->DisplayPasskey(peer_id_, passkey, method, std::move(confirm));
  }
}

void LowEnergyConnection::RequestPasskey(PasskeyResponseCallback respond) {
  bt_log(INFO, "gap-le", "pairing delegate request for passkey entry (peer: %s)", bt_str(peer_id_));

  auto* delegate = conn_mgr_->pairing_delegate();
  if (!delegate) {
    bt_log(ERROR, "gap-le", "rejecting pairing without a PairingDelegate! (peer: %s)",
           bt_str(peer_id_));
    respond(-1);
  } else {
    delegate->RequestPasskey(peer_id_, std::move(respond));
  }
}

}  // namespace bt::gap::internal
