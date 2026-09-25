{{/*
Common helpers for the choreography-saga chart.
*/}}

{{- define "cs.fullname" -}}
{{- printf "%s-%s" .Release.Name .Component | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "cs.image" -}}
{{- $reg := .root.Values.global.imageRegistry -}}
{{- printf "%s%s:%s" $reg .image.repository .image.tag -}}
{{- end -}}

{{- define "cs.labels" -}}
app.kubernetes.io/name: {{ .Component }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/part-of: choreography-saga
app.kubernetes.io/managed-by: {{ .Release.Service }}
helm.sh/chart: {{ printf "%s-%s" .Chart.Name .Chart.Version }}
{{- end -}}

{{- define "cs.selectorLabels" -}}
app.kubernetes.io/name: {{ .Component }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end -}}
