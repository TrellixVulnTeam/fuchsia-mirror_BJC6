// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/handle.h>
#include <lib/zx/vmo.h>
#include <lib/zxio/cpp/inception.h>
#include <lib/zxio/null.h>
#include <lib/zxio/zxio.h>
#include <zircon/syscalls.h>

namespace {

// A zxio_handle_holder is a zxio object instance that holds on to a handle and
// allows it to be closed or released via zxio_close() / zxio_release(). It is
// useful for wrapping objects that zxio does not understand.
struct zxio_handle_holder {
  zxio_t io;
  zx::handle handle;
};

static_assert(sizeof(zxio_handle_holder) <= sizeof(zxio_storage_t),
              "zxio_handle_holder must fit inside zxio_storage_t.");

zxio_handle_holder& zxio_get_handle_holder(zxio_t* io) {
  return *reinterpret_cast<zxio_handle_holder*>(io);
}

static constexpr zxio_ops_t zxio_handle_holder_ops = []() {
  zxio_ops_t ops = zxio_default_ops;
  ops.close = [](zxio_t* io) {
    zxio_get_handle_holder(io).~zxio_handle_holder();
    return ZX_OK;
  };

  ops.release = [](zxio_t* io, zx_handle_t* out_handle) {
    zx_handle_t handle = zxio_get_handle_holder(io).handle.release();
    if (handle == ZX_HANDLE_INVALID) {
      return ZX_ERR_BAD_HANDLE;
    }
    *out_handle = handle;
    return ZX_OK;
  };
  return ops;
}();

void zxio_handle_holder_init(zxio_storage_t* storage, zx::handle handle) {
  auto holder = new (storage) zxio_handle_holder{
      .handle = std::move(handle),
  };
  zxio_init(&holder->io, &zxio_handle_holder_ops);
}

}  // namespace

zx_status_t zxio_create_with_info(zx_handle_t raw_handle, const zx_info_handle_basic_t* handle_info,
                                  zxio_storage_t* storage) {
  zx::handle handle(raw_handle);
  if (!handle.is_valid() || storage == nullptr || handle_info == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  switch (handle_info->type) {
    case ZX_OBJ_TYPE_LOG: {
      zxio_debuglog_init(storage, zx::debuglog(std::move(handle)));
      return ZX_OK;
    }
    case ZX_OBJ_TYPE_VMO: {
      zx::vmo vmo(std::move(handle));
      zx::stream stream;
      uint32_t options = 0u;
      if (handle_info->rights & ZX_RIGHT_READ) {
        options |= ZX_STREAM_MODE_READ;
      }
      if (handle_info->rights & ZX_RIGHT_WRITE) {
        options |= ZX_STREAM_MODE_WRITE;
      }
      // We pass 0 for the initial seek value because the |handle| we're given does not remember
      // the seek value we had previously.
      zx_status_t status = zx::stream::create(options, vmo, 0u, &stream);
      if (status != ZX_OK) {
        zxio_null_init(&storage->io);
        return status;
      }
      return zxio_vmo_init(storage, std::move(vmo), std::move(stream));
    }
    case ZX_OBJ_TYPE_SOCKET: {
      zx::socket socket(std::move(handle));
      zx_info_socket_t info;
      zx_status_t status = socket.get_info(ZX_INFO_SOCKET, &info, sizeof(info), nullptr, nullptr);
      if (status != ZX_OK) {
        return status;
      }
      return zxio_pipe_init(storage, std::move(socket), info);
    }
    default: {
      zxio_handle_holder_init(storage, std::move(handle));
      return ZX_ERR_NOT_SUPPORTED;
    }
  }
}

zx_status_t zxio_create(zx_handle_t raw_handle, zxio_storage_t* storage) {
  zx::handle handle(raw_handle);
  if (!handle.is_valid() || storage == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  zx_info_handle_basic_t info = {};
  zx_status_t status = handle.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  if (status != ZX_OK) {
    zxio_null_init(&storage->io);
    return status;
  }
  return zxio_create_with_info(handle.release(), &info, storage);
}