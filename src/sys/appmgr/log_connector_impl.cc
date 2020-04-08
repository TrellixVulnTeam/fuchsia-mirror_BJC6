// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/appmgr/log_connector_impl.h"

#include "src/lib/fxl/logging.h"

namespace component {

LogConnectorImpl::LogConnectorImpl(fxl::WeakPtr<LogConnectorImpl> parent, std::string realm_label)
    : parent_(std::move(parent)),
      realm_label_(realm_label),
      consumer_request_(consumer_.NewRequest()),
      weak_factory_(this){};

LogConnectorImpl::LogConnectorImpl(std::string realm_label)
    : LogConnectorImpl(nullptr /* parent */, realm_label) {}

fbl::RefPtr<LogConnectorImpl> LogConnectorImpl::NewChild(std::string child_realm_label) {
  auto child = new LogConnectorImpl(weak_factory_.GetWeakPtr(), child_realm_label);
  return AdoptRef(child);
}

void LogConnectorImpl::TakeLogConnectionListener(TakeLogConnectionListenerCallback callback) {
  FXL_LOG(INFO) << "taking log connector for " << realm_label_;
  callback(std::move(consumer_request_));
}

void LogConnectorImpl::AddConnectorClient(
    fidl::InterfaceRequest<fuchsia::sys::internal::LogConnector> request) {
  bindings_.AddBinding(this, std::move(request));
}

void LogConnectorImpl::AddLogConnection(
    std::string component_url, std::string instance_id,
    fidl::InterfaceRequest<fuchsia::logger::LogSink> connection) {
  // find the nearest initialized LogConnector, assumes that >=1 one is instantiated before this
  std::vector<std::string> realm_path;
  auto current = this;
  while (current->parent_ && current->consumer_request_.is_valid()) {
    realm_path.push_back(current->realm_label_);
    current = &*current->parent_;
  }
  std::reverse(realm_path.begin(), realm_path.end());

  fuchsia::sys::internal::SourceIdentity identity;
  identity.set_instance_id(instance_id);
  identity.set_realm_path(realm_path);
  identity.set_component_url(std::move(component_url));
  identity.set_component_name(realm_label_);

  current->consumer_->OnNewConnection({
      .log_request = std::move(connection),
      .source_identity = std::move(identity),
  });
}
}  // namespace component
