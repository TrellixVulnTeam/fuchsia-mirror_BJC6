// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'package:fidl_fuchsia_fibonacci/fidl_async.dart' as fidl_fib;

class FibonacciImpl extends fidl_fib.Fibonacci {
  @override
  Future<int> calculate(int n) async {
    return _fib(n);
  }

  int _fib(int n) {
    if (n <= 1) {
      return n;
    }
    return _fib(n - 1) + _fib(n - 2);
  }
}
