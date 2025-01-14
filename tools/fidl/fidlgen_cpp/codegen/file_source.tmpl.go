// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const fileSourceTmpl = `
{{- define "Filename:Source" -}}
  {{- .LibrarySlashes }}/cpp/fidl_v2.cc
{{- end }}


{{- define "File:Source" -}}
{{- UseUnified -}}

// WARNING: This file is machine generated by fidlgen_cpp.

#include <{{ .Library | Filename "Header" }}>
{{ "" }}

{{- /* When the library name only has one component, the natural types are trivially
       available in our namespace. */}}
{{- if not .SingleComponentLibraryName }}
{{- range .Decls }}
{{- if Eq .Kind Kinds.Const }}{{ template "ConstDefinition" . }}{{- end }}
{{- end }}
{{- end }}
{{ "" }}

{{ EndOfFile }}
{{ end }}
`
