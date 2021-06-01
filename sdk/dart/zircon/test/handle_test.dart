// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:test/test.dart';
import 'package:zircon/zircon.dart';

void main() {
  test('create and duplicate handle', () {
    final HandlePairResult pair = System.eventpairCreate();
    expect(pair.status, equals(ZX.OK));
    expect(pair.first.isValid, isTrue);
    expect(pair.second.isValid, isTrue);

    final Handle duplicate = pair.first.duplicate(ZX.RIGHT_SAME_RIGHTS);
    expect(duplicate.isValid, isTrue);

    final Handle failedDuplicate = pair.first.duplicate(-1);
    expect(failedDuplicate.isValid, isFalse);
  });

  test('failure invalid handle', () {
    final Handle handle = Handle.invalid();
    final Handle duplicate = handle.duplicate(ZX.RIGHT_SAME_RIGHTS);
    expect(duplicate.isValid, isFalse);
  });

  test('handle and its duplicate have same koid', () {
    final HandlePairResult pair = System.eventpairCreate();
    expect(pair.status, equals(ZX.OK));
    expect(pair.first.isValid, isTrue);
    expect(pair.second.isValid, isTrue);

    final Handle duplicate = pair.first.duplicate(ZX.RIGHT_SAME_RIGHTS);
    expect(duplicate.isValid, isTrue);

    expect(pair.first.koid, duplicate.koid);
  });

  test('cache koid and invalidate', () {
    final HandleResult vmo = System.vmoCreate(0);
    expect(vmo.status, equals(ZX.OK));
    int originalKoid = vmo.handle.koid;
    expect(originalKoid, isNot(equals(ZX.KOID_INVALID)));
    // Cached koid should be same value.
    expect(originalKoid, equals(vmo.handle.koid));
    vmo.handle.close();
    // koid should be invalidated.
    expect(vmo.handle.koid, equals(ZX.KOID_INVALID));
  });
}
