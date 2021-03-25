// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package rules

import (
	"testing"
)

func TestVerifyInternalLinks_wrongXref(t *testing.T) {
	ruleTestCase{
		files: map[string]string{
			"example.md": `
wrong «[tipo in label]»

[typo in label]: https://google.com`,
		},
	}.runOverEvents(t, newVerifyInternalLinks)
}

func TestVerifyInternalLinks_duplicateXref(t *testing.T) {
	ruleTestCase{
		files: map[string]string{
			"example.md": `
[twice]: https://google.com
«[twice]»: http://google.com`,
		},
	}.runOverEvents(t, newVerifyInternalLinks)
}

// TODO(fxbug.dev/62964): Improve verifyInternalLinks rule.
func Ignore_TestVerifyInternalLinks_unknownFile(t *testing.T) {
	ruleTestCase{
		files: map[string]string{
			"example.md": `we do not know «[this page](nothere.md)»`,
		},
	}.runOverEvents(t, newVerifyInternalLinks)
}

// TODO(fxbug.dev/62964): Improve verifyInternalLinks rule.
func Ignore_TestVerifyInternalLinks_unknownAnchor(t *testing.T) {
	ruleTestCase{
		files: map[string]string{
			"one.md": `linking to «[wrong](two.md#anchor)»`,
			"two.md": `## bad {#anquor}`,
		},
	}.runOverEvents(t, newVerifyInternalLinks)
}
