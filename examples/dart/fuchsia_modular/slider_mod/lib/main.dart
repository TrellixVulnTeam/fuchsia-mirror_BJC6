// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'package:flutter/material.dart';
import 'package:fuchsia_logger/logger.dart';

import 'src/widgets/app.dart';

void main() {
  setupLogger(name: 'slider_mod');
  runApp(MaterialApp(home: App()));
}
