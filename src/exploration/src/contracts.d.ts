import type { IChart } from '@kanaries/graphic-walker'
export interface TableWorkspace {
  source_id: string
  schema_revision: string
  chart: IChart
  editor_mode: 'simple' | 'advanced'
  detail_mode: 'complete' | 'limited'
  temporal_field?: string
  request_generation: number
}
export interface NumericDisplay {
  field_id: string
  mode: 'exact' | 'approximate'
  origin: 'decimal' | 'native_decimal_mean'
  exact_values?: Array<string | null>
}
export interface ChartKind {
  id: string
  label: string
  roles: Array<{ key: string; label: string; kind: 'dimension' | 'measure' | 'any'; optional?: boolean }>
}
export interface HistogramConfig { field: string; bins: number }
export interface TimeBucketConfig { field: string; unit: 'day' | 'month' }
export interface TimeBucketGroup extends TimeBucketConfig { asFieldKey: string }
// aggregate.groupBy: Array<string | TimeBucketGroup>; measure.agg also accepts distinctCount.
// Source schema: time_bucket_fields: string[]; semantics.timezone = source_calendar.
// IChart.config.flowsqlTimeBucket?: TimeBucketConfig; retains the source field ID as output alias.
// IChart.config.flowsqlHistogram?: HistogramConfig; view op: histogram -> ordered native bin edges/counts.
