// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback_data/system_log_recorder/system_log_recorder.h"

#include <lib/syslog/cpp/macros.h>

#include "src/lib/diagnostics/stream/cpp/log_message.h"
#include "src/lib/files/path.h"

namespace forensics {
namespace feedback_data {
namespace system_log_recorder {

SystemLogRecorder::SystemLogRecorder(async_dispatcher_t* archive_dispatcher,
                                     async_dispatcher_t* write_dispatcher,
                                     std::shared_ptr<sys::ServiceDirectory> services,
                                     WriteParameters write_parameters,
                                     std::unique_ptr<Encoder> encoder)
    : write_dispatcher_(write_dispatcher),
      write_period_(write_parameters.period),
      logs_dir_(write_parameters.logs_dir),
      store_(write_parameters.total_log_size_bytes / write_parameters.max_num_files,
             write_parameters.max_write_size_bytes, std::move(encoder)),
      archive_accessor_(archive_dispatcher, services, fuchsia::diagnostics::DataType::LOGS,
                        fuchsia::diagnostics::StreamMode::SNAPSHOT_THEN_SUBSCRIBE),
      writer_(logs_dir_, write_parameters.max_num_files, &store_) {}

void SystemLogRecorder::Start() {
  archive_accessor_.Collect([this](fuchsia::diagnostics::FormattedContent chunk) {
    auto log_messages = diagnostics::stream::ConvertFormattedContentToLogMessages(std::move(chunk));
    if (log_messages.is_error()) {
      store_.Add(::fit::error(log_messages.take_error()));
      return;
    }

    for (auto& log_message : log_messages.value()) {
      store_.Add(std::move(log_message));
    }
  });

  periodic_write_task_.Post(write_dispatcher_);
}

void SystemLogRecorder::StopAndDeleteLogs() {
  // Stop collecting logs.
  archive_accessor_.StopCollect();
  periodic_write_task_.Cancel();

  // Consume the data currently in the store to flush it.
  bool end_of_block;
  store_.Consume(&end_of_block);

  // Delete the persisted logs.
  files::DeletePath(logs_dir_, /*recursive=*/true);

  FX_LOGS(INFO) << "Stopped log recording and flushed persisted logs";
}

void SystemLogRecorder::PeriodicWriteTask() {
  writer_.Write();
  periodic_write_task_.PostDelayed(write_dispatcher_, write_period_);
}

}  // namespace system_log_recorder
}  // namespace feedback_data
}  // namespace forensics
