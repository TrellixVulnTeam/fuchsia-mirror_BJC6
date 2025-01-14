// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const testBaseTemplate = `
{{- define "Filename:TestBase" -}}
  {{- .LibrarySlashes }}/cpp/
  {{- if ExperimentEnabled "natural-types" -}}
    natural_types_test_base.h
  {{- else -}}
    fidl_test_base.h
  {{- end -}}
{{- end }}

{{- define "File:TestBase" -}}
{{- UseNatural -}}
// WARNING: This file is machine generated by fidlgen.

#pragma once

{{ range .Dependencies -}}
#include <{{ . | Filename "Header" }}>
{{ end -}}

#include <{{ .Library | Filename "Header" }}>

{{- range .Decls }}
  {{- if Eq .Kind Kinds.Protocol }}{{ $protocol := .}}
  {{- range $transport, $_ := .Transports }}{{- if eq $transport "Channel" }}
{{ EnsureNamespace $protocol.TestBase }}
class {{ $protocol.TestBase.Name }} : public {{ $protocol }} {
  public:
  virtual ~{{ $protocol.TestBase.Name }}() { }
  virtual void NotImplemented_(const std::string& name) = 0;

  {{- range $protocol.Methods }}
    {{- if .HasRequest }}
  void {{ template "RequestMethodSignature" . }} override {
    NotImplemented_("{{ .Name }}");
  }
    {{- end }}
  {{- end }}

};
  {{- end }}
{{- end }}{{ end }}{{ end -}}

{{ EndOfFile }}
{{ end }}
`
