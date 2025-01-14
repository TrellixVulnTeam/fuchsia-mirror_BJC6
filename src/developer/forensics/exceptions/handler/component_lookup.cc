// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/exceptions/handler/component_lookup.h"

#include <fuchsia/sys/internal/cpp/fidl.h>
#include <fuchsia/sys2/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>

#include "src/developer/forensics/utils/errors.h"
#include "src/developer/forensics/utils/fidl/oneshot_ptr.h"
#include "src/developer/forensics/utils/fit/promise.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/strings/join_strings.h"

namespace forensics {
namespace exceptions {
namespace handler {
namespace {

::fpromise::promise<ComponentInfo> GetV1Info(async_dispatcher_t* dispatcher,
                                             std::shared_ptr<sys::ServiceDirectory> services,
                                             fit::Timeout timeout, zx_koid_t thread_koid) {
  namespace sys = fuchsia::sys::internal;
  auto introspect_ptr =
      std::make_unique<fidl::OneShotPtr<sys::CrashIntrospect, ComponentInfo>>(dispatcher, services);
  (*introspect_ptr)
      ->FindComponentByThreadKoid(
          thread_koid, [introspect = introspect_ptr.get()](
                           sys::CrashIntrospect_FindComponentByThreadKoid_Result result) {
            if (introspect->IsAlreadyDone()) {
              return;
            }

            if (result.is_response()) {
              const sys::SourceIdentity& info = result.response().component_info;

              std::string url;
              if (info.has_component_url()) {
                url = info.component_url();
              }

              std::string realm_path;
              if (info.has_realm_path()) {
                realm_path = "/" + fxl::JoinStrings(info.realm_path(), "/");
              }

              std::string moniker;
              if (info.has_realm_path() && info.has_component_name()) {
                std::vector<std::string> moniker_parts = info.realm_path();
                moniker_parts.push_back(info.component_name());
                moniker = fxl::JoinStrings(moniker_parts, "/");
              }

              introspect->CompleteOk(ComponentInfo{
                  .url = url,
                  .realm_path = realm_path,
                  .moniker = moniker,
              });
            } else {
              // ZX_ERR_NOT_FOUND most likely means a thread from a process outside a component,
              // which is not an error.
              if (result.err() != ZX_ERR_NOT_FOUND) {
                FX_PLOGS(WARNING, result.err()) << "Failed v1 FindComponentBeThreadKoid ";
              }

              introspect->CompleteError(Error::kDefault);
            }
          });

  return introspect_ptr->WaitForDone(std::move(timeout))
      .or_else([_ = std::move(introspect_ptr)](const Error& error) { return ::fpromise::error(); });
}

::fpromise::promise<ComponentInfo> GetV2Info(async_dispatcher_t* dispatcher,
                                             std::shared_ptr<sys::ServiceDirectory> services,
                                             fit::Timeout timeout, zx_koid_t thread_koid) {
  namespace sys = fuchsia::sys2;
  auto introspect_ptr =
      std::make_unique<fidl::OneShotPtr<sys::CrashIntrospect, ComponentInfo>>(dispatcher, services);
  (*introspect_ptr)
      ->FindComponentByThreadKoid(
          thread_koid, [introspect = introspect_ptr.get()](
                           sys::CrashIntrospect_FindComponentByThreadKoid_Result result) {
            if (introspect->IsAlreadyDone()) {
              return;
            }

            if (result.is_response()) {
              const sys::ComponentCrashInfo& info = result.response().info;
              std::string moniker = (info.has_moniker()) ? info.moniker() : "";
              if (!moniker.empty() && moniker[0] == '/') {
                moniker = moniker.substr(1);
              }
              introspect->CompleteOk(ComponentInfo{
                  .url = (info.has_url()) ? info.url() : "",
                  .realm_path = "",
                  .moniker = moniker,
              });
            } else {
              // RESOURCE_NOT_FOUND most likely means a thread from a process outside a component,
              // which is not an error.
              if (result.err() != fuchsia::component::Error::RESOURCE_NOT_FOUND) {
                FX_LOGS(WARNING) << "Failed v2 FindComponentByThreadKoid, error: "
                                 << static_cast<int>(result.err());
              }

              introspect->CompleteError(Error::kDefault);
            }
          });

  return introspect_ptr->WaitForDone(std::move(timeout))
      .or_else([_ = std::move(introspect_ptr)](const Error& error) { return ::fpromise::error(); });
}

}  // namespace

::fpromise::promise<ComponentInfo> GetComponentInfo(async_dispatcher_t* dispatcher,
                                                    std::shared_ptr<sys::ServiceDirectory> services,
                                                    const zx::duration timeout,
                                                    zx_koid_t thread_koid) {
  auto get_v1_info = GetV1Info(dispatcher, services, fit::Timeout(timeout), thread_koid);
  auto get_v2_info = GetV2Info(dispatcher, services, fit::Timeout(timeout), thread_koid);
  return ::fpromise::join_promises(std::move(get_v1_info), std::move(get_v2_info))
      .and_then([](std::tuple<::fit::result<ComponentInfo>, ::fit::result<ComponentInfo>>& results)
                    -> ::fpromise::result<ComponentInfo> {
        auto& v1_result = std::get<0>(results);
        auto& v2_result = std::get<1>(results);

        if (v1_result.is_error() && v2_result.is_error()) {
          return ::fit::error();
        }

        ComponentInfo info = (v1_result.is_ok()) ? v1_result.take_value() : v2_result.take_value();
        return ::fit::ok(std::move(info));
      });
}

}  // namespace handler
}  // namespace exceptions
}  // namespace forensics
