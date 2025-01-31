// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fidl/ordinals.h"

#include <optional>

#include "fidl/flat_ast.h"

#define BORINGSSL_NO_CXX
#include <openssl/sha.h>

namespace fidl {
namespace ordinals {

std::string GetSelector(const flat::AttributeList* attributes, SourceSpan name) {
  flat::MaybeAttribute maybe_selector_attr = attributes->GetAttribute("selector");
  if (maybe_selector_attr.has_value()) {
    auto selector_constant = maybe_selector_attr.value().get().GetArg("value");
    if (selector_constant.has_value() &&
        selector_constant.value().get().value->Value().kind == flat::ConstantValue::Kind::kString) {
      auto selector_string_constant = static_cast<const flat::StringConstantValue&>(
          selector_constant.value().get().value->Value());
      return selector_string_constant.MakeContents();
    }
  }
  return std::string(name.data().data(), name.data().size());
}

namespace {

uint64_t CalcOrdinal(const std::string_view& full_name) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(full_name.data()), full_name.size(), digest);
  // The following dance ensures that we treat the bytes as a little-endian
  // int64 regardless of host byte order.
  // clang-format off
  uint64_t ordinal =
      static_cast<uint64_t>(digest[0]) |
      static_cast<uint64_t>(digest[1]) << 8 |
      static_cast<uint64_t>(digest[2]) << 16 |
      static_cast<uint64_t>(digest[3]) << 24 |
      static_cast<uint64_t>(digest[4]) << 32 |
      static_cast<uint64_t>(digest[5]) << 40 |
      static_cast<uint64_t>(digest[6]) << 48 |
      static_cast<uint64_t>(digest[7]) << 56;
  // clang-format on

  return ordinal & 0x7fffffffffffffff;
}

}  // namespace

raw::Ordinal64 GetGeneratedOrdinal64(const std::vector<std::string_view>& library_name,
                                     const std::string_view& protocol_name,
                                     const std::string_view& selector_name,
                                     const raw::SourceElement& source_element) {
  if (selector_name.find("/") != selector_name.npos)
    return raw::Ordinal64(source_element, CalcOrdinal(selector_name));

  // TODO(pascallouis): Move this closer (code wise) to NameFlatName, ideally
  // sharing code.
  std::string full_name;
  bool once = false;
  for (std::string_view id : library_name) {
    if (once) {
      full_name.push_back('.');
    } else {
      once = true;
    }
    full_name.append(id.data(), id.size());
  }
  full_name.append("/");
  full_name.append(protocol_name.data(), protocol_name.size());
  full_name.append(".");
  full_name.append(selector_name);

  return raw::Ordinal64(source_element, CalcOrdinal(full_name));
}

}  // namespace ordinals
}  // namespace fidl
