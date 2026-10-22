import type { IChart } from '@kanaries/graphic-walker'
export interface TableWorkspaceV1 {
  source_id: string
  schema_revision: string
  chart: IChart
  editor_mode: 'simple' | 'advanced'
  detail_mode: 'complete' | 'limited'
  temporal_field?: string
  request_generation: number
}
export interface NumericDisplayV1 {
  field_id: string
  mode: 'exact' | 'approximate'
  origin: 'decimal' | 'native_decimal_mean'
  exact_values?: Array<string | null>
}
export interface ChartKindV1 {
  id: string
  label: string
  roles: Array<{ key: string; label: string; kind: 'dimension' | 'measure' | 'any'; optional?: boolean }>
}
export interface HistogramConfigV1 { field: string; bins: number }
export interface TimeBucketConfigV1 { field: string; unit: 'day' | 'month' }
export interface TimeBucketGroupV1 extends TimeBucketConfigV1 { asFieldKey: string }
// aggregate.groupBy: Array<string | TimeBucketGroupV1>; measure.agg also accepts distinctCount.
// Source schema: time_bucket_fields: string[]; semantics.timezone = source_calendar.
// IChart.config.flowsqlTimeBucket?: TimeBucketConfigV1; retains the source field ID as output alias.
// IChart.config.flowsqlHistogram?: HistogramConfigV1; view op: histogram -> ordered native bin edges/counts.
