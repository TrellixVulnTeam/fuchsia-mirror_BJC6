// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'package:fxtest/fxtest.dart';
import 'package:fxutils/fxutils.dart';

class FxRunException implements Exception {
  final String fxCmd;
  final int exitCode;
  FxRunException(this.fxCmd, [this.exitCode = failureExitCode]);
  @override
  String toString() => 'FxRunException: Failed to run `$fxCmd` :: '
      'Exit Code: $exitCode';
}

class FailFastException implements Exception {
  @override
  String toString() => 'FailFastException';
}

class MalformedFuchsiaUrlException implements Exception {
  final String packageUrl;
  MalformedFuchsiaUrlException(this.packageUrl);

  @override
  String toString() =>
      'MalformedFuchsiaUrlException: Malformed Fuchsia Package '
      'Url `$packageUrl` could not be parsed';
}

class HashFileNotFoundException implements Exception {
  final String message;
  HashFileNotFoundException(this.message);

  @override
  String toString() => message;
}

class PackageRepositoryException implements Exception {
  String file;
  final String message;
  PackageRepositoryException(this.message, [this.file = '']);

  @override
  String toString() => '$file: $message';
}

class PackageRepositoryParseException extends PackageRepositoryException {
  PackageRepositoryParseException(String message) : super(message);
}

class UnparsedTestException implements Exception {
  final String message;
  UnparsedTestException(this.message);

  @override
  String toString() => message;
}

class UnrunnableTestException implements Exception {
  final String message;
  UnrunnableTestException(this.message);

  @override
  String toString() => message;
}

class SigIntException implements Exception {}

const _missingFxMessage =
    'Did not find `fx` command at expected location: //$fxLocation';

class MissingFxException implements Exception {
  @override
  String toString() => _missingFxMessage;
}

class OutputClosedException implements Exception {
  final int exitCode;
  OutputClosedException([this.exitCode = 0]);
}

class BadMapPathException implements Exception {
  final String message;
  BadMapPathException(this.message);
}
