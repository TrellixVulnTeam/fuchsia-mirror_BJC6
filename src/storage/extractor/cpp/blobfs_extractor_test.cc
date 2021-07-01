// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <lib/fdio/spawn.h>
#include <lib/zx/process.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zircon/device/block.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>

#include <fbl/unique_fd.h>

#include "src/storage/blobfs/format.h"
#include "src/storage/blobfs/test/integration/blobfs_fixtures.h"
#include "src/storage/extractor/c/extractor.h"
#include "src/storage/extractor/cpp/extractor.h"
#include "src/storage/fs_test/blobfs_test.h"
#include "src/storage/fs_test/fs_test.h"
#include "src/storage/fs_test/fs_test_fixture.h"

namespace extractor {
namespace {

using BlobfsExtractionTest = fs_test::FilesystemTest;

TEST_P(BlobfsExtractionTest, TestSuperblock) {
  ASSERT_EQ(fs().Unmount().status_value(), ZX_OK);

  fbl::unique_fd input_fd(open(fs().DevicePath().value().c_str(), O_RDONLY));
  ASSERT_TRUE(input_fd);

  char out_path[] = "/tmp/blobfs-extraction.XXXXXX";
  fbl::unique_fd output_fd(mkostemp(out_path, O_RDWR | O_CREAT | O_EXCL));
  ASSERT_TRUE(output_fd);
  ExtractorOptions options = ExtractorOptions{.force_dump_pii = false,
                                              .add_checksum = false,
                                              .alignment = blobfs::kBlobfsBlockSize,
                                              .compress = false};
  auto extractor_status = Extractor::Create(input_fd.duplicate(), options, output_fd.duplicate());
  ASSERT_EQ(extractor_status.status_value(), ZX_OK);
  auto extractor = std::move(extractor_status.value());
  auto status = BlobfsExtract(input_fd.duplicate(), *extractor);
  ASSERT_TRUE(status.is_ok());
  ASSERT_TRUE(extractor->Write().is_ok());

  blobfs::Superblock info;
  ASSERT_EQ(blobfs::kBlobfsBlockSize,
            pread(input_fd.get(), &info, blobfs::kBlobfsBlockSize, blobfs::kSuperblockOffset));
  ASSERT_EQ(info.magic0, blobfs::kBlobfsMagic0);
  ASSERT_EQ(info.magic1, blobfs::kBlobfsMagic1);

  struct stat stats;
  ASSERT_EQ(fstat(output_fd.get(), &stats), 0);
  // One block for header and one for extent cluster
  constexpr uint64_t kExtractedImageBlockCount = 2;
  ssize_t expected = (kExtractedImageBlockCount)*blobfs::kBlobfsBlockSize;

  char read_buffer[blobfs::kBlobfsBlockSize];
  ASSERT_EQ(pread(output_fd.get(), read_buffer, sizeof(read_buffer), expected),
            static_cast<ssize_t>(sizeof(read_buffer)));
  ASSERT_EQ(memcmp(&info, read_buffer, sizeof(read_buffer)), 0);
}

INSTANTIATE_TEST_SUITE_P(/*no prefix*/, BlobfsExtractionTest,
                         testing::Values(blobfs::BlobfsDefaultTestParam()),
                         testing::PrintToStringParamName());
}  // namespace

}  // namespace extractor