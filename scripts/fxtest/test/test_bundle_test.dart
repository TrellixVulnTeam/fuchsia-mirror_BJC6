// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'package:fxtest/fxtest.dart';
import 'package:test/test.dart';

void main() {
  group('TestBundle', () {
    test('should assemble correct arguments for component tests', () {
      const componentUrl =
          'fuchsia-pkg://fuchsia.com/pkg-name#meta/component-name.cmx';
      final testDef = TestDefinition.fromJson(
        {
          'environments': [],
          'test': {
            'cpu': 'x64',
            'label': '//scripts/lib:lib_tests(//build/toolchain:host_x64)',
            'name': 'lib_tests',
            'os': 'fuchsia',
            'package_url': componentUrl,
            'runtime_deps': 'host_x64/gen/scripts/lib/lib_tests.deps.json'
          }
        },
        buildDir: '/whatever',
      );
      final commandTokens =
          testDef.createExecutionHandle().getInvocationTokens([]);
      expect(
        commandTokens.fullCommand,
        ['fx', 'shell', 'run-test-component', componentUrl].join(' '),
      );
      final commandTokens2 = testDef
          .createExecutionHandle()
          .getInvocationTokens(['--max-log-severity=WARN']);
      expect(
        commandTokens2.fullCommand,
        [
          'fx',
          'shell',
          'run-test-component',
          "'--max-log-severity=WARN'",
          componentUrl
        ].join(' '),
      );
    });

    test('should assemble correct arguments for component suite tests', () {
      const componentUrl =
          'fuchsia-pkg://fuchsia.com/pkg-name#meta/component-name.cm';
      final testDef = TestDefinition.fromJson(
        {
          'environments': [],
          'test': {
            'cpu': 'x64',
            'label': '//scripts/lib:lib_tests(//build/toolchain:host_x64)',
            'name': 'lib_tests',
            'os': 'fuchsia',
            'package_url': componentUrl,
            'runtime_deps': 'host_x64/gen/scripts/lib/lib_tests.deps.json',
            'parallel': 2,
          }
        },
        buildDir: '/whatever',
      );
      final commandTokens =
          testDef.createExecutionHandle().getInvocationTokens([]);
      expect(
        commandTokens.fullCommand,
        [
          'fx',
          'ffx',
          'test',
          'run',
          '--disable-output-directory',
          '--parallel',
          '2',
          componentUrl
        ].join(' '),
      );

      final testDef2 = TestDefinition.fromJson(
        {
          'environments': [],
          'test': {
            'cpu': 'x64',
            'label': '//scripts/lib:lib_tests(//build/toolchain:host_x64)',
            'name': 'lib_tests',
            'os': 'fuchsia',
            'package_url': componentUrl,
            'runtime_deps': 'host_x64/gen/scripts/lib/lib_tests.deps.json',
          }
        },
        buildDir: '/whatever',
      );

      // passed flag should be present in command line
      final commandTokens2 = testDef2
          .createExecutionHandle()
          .getInvocationTokens(['--some-flag', 'some_val']);
      expect(
        commandTokens2.fullCommand,
        [
          'fx',
          'ffx',
          'test',
          'run',
          '--disable-output-directory',
          "'--some-flag'",
          "'some_val'",
          componentUrl
        ].join(' '),
      );
    });

    test(
        'should assemble correct arguments for component suite tests with parallel override',
        () {
      const componentUrl =
          'fuchsia-pkg://fuchsia.com/pkg-name#meta/component-name.cm';
      final testDef = TestDefinition.fromJson(
        {
          'environments': [],
          'test': {
            'cpu': 'x64',
            'label': '//scripts/lib:lib_tests(//build/toolchain:host_x64)',
            'name': 'lib_tests',
            'os': 'fuchsia',
            'package_url': componentUrl,
            'runtime_deps': 'host_x64/gen/scripts/lib/lib_tests.deps.json',
            'parallel': 2,
          }
        },
        buildDir: '/whatever',
      );
      final commandTokens = testDef
          .createExecutionHandle(parallelOverride: '5')
          .getInvocationTokens([]);
      expect(
        commandTokens.fullCommand,
        [
          'fx',
          'ffx',
          'test',
          'run',
          '--disable-output-directory',
          '--parallel 5',
          componentUrl
        ].join(' '),
      );
    });

    test(
        'should use run-test-suite for component suite tests with run-test-suite override',
        () {
      const componentUrl =
          'fuchsia-pkg://fuchsia.com/pkg-name#meta/component-name.cm';
      final testDef = TestDefinition.fromJson(
        {
          'environments': [],
          'test': {
            'cpu': 'x64',
            'label': '//scripts/lib:lib_tests(//build/toolchain:host_x64)',
            'name': 'lib_tests',
            'os': 'fuchsia',
            'package_url': componentUrl,
            'runtime_deps': 'host_x64/gen/scripts/lib/lib_tests.deps.json',
          }
        },
        buildDir: '/whatever',
      );
      final commandTokens = testDef
          .createExecutionHandle(useRunTestSuiteForV2: true)
          .getInvocationTokens([]);
      expect(
        commandTokens.fullCommand,
        ['fx', 'shell', 'run-test-suite', componentUrl].join(' '),
      );
    });
  });
}
