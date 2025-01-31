// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_VFS_CPP_INTERNAL_DIRECTORY_CONNECTION_H_
#define LIB_VFS_CPP_INTERNAL_DIRECTORY_CONNECTION_H_

#include <fuchsia/io/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/vfs/cpp/internal/connection.h>

#include <memory>

namespace vfs {

namespace internal {
class Directory;

// Binds an implementation of |fuchsia.io.Directory| to a
// |vfs::internal::Directory|.
class DirectoryConnection final : public Connection, public fuchsia::io::Directory {
 public:
  // Create a connection to |vn| with the given |flags|.
  DirectoryConnection(uint32_t flags, vfs::internal::Directory* vn);
  ~DirectoryConnection() override;

  // Start listening for |fuchsia.io.Directory| messages on |request|.
  zx_status_t BindInternal(zx::channel request, async_dispatcher_t* dispatcher) override;

  // |fuchsia::io::Directory| Implementation:
  void Clone(uint32_t flags, fidl::InterfaceRequest<fuchsia::io::Node> object) override;
  void Close(CloseCallback callback) override;
  void Close2(Close2Callback callback) override;
  void Describe(DescribeCallback callback) override;
  void Describe2(fuchsia::io::ConnectionInfoQuery query, Describe2Callback callback) override;
  void Sync(SyncCallback callback) override;
  void Sync2(Sync2Callback callback) override;
  void GetAttr(GetAttrCallback callback) override;
  void SetAttr(uint32_t flags, fuchsia::io::NodeAttributes attributes,
               SetAttrCallback callback) override;
  void Open(uint32_t flags, uint32_t mode, std::string path,
            fidl::InterfaceRequest<fuchsia::io::Node> object) override;
  void AddInotifyFilter(std::string path, fuchsia::io2::InotifyWatchMask filters,
                        uint32_t watch_descriptor, zx::socket socket,
                        AddInotifyFilterCallback callback) override {}
  void Unlink(std::string name, fuchsia::io2::UnlinkOptions options,
              UnlinkCallback callback) override;
  void ReadDirents(uint64_t max_bytes, ReadDirentsCallback callback) override;
  void Rewind(RewindCallback callback) override;
  void GetToken(GetTokenCallback callback) override;
  void Rename2(std::string src, zx::event dst_parent_token, std::string dst,
               Rename2Callback callback) override;
  void Link(std::string src, zx::handle dst_parent_token, std::string dst,
            LinkCallback callback) override;
  void Watch(uint32_t mask, uint32_t options, zx::channel watcher, WatchCallback callback) override;
  void NodeGetFlags(NodeGetFlagsCallback callback) override;
  void NodeSetFlags(uint32_t flags, NodeSetFlagsCallback callback) override;

 protected:
  // |Connection| Implementation:
  void SendOnOpenEvent(zx_status_t status) override;

 private:
  vfs::internal::Directory* vn_;
  fidl::Binding<fuchsia::io::Directory> binding_;
};

}  // namespace internal
}  // namespace vfs

#endif  // LIB_VFS_CPP_INTERNAL_DIRECTORY_CONNECTION_H_
