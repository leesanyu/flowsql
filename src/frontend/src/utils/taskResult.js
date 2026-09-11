// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

export const formatTaskResult = (result, runningMessage = '') => {
  if (Array.isArray(result.data)) {
    if (result.status === 'completed' && result.data.length === 0) {
      return {
        columns: [],
        rows: [],
        message: `执行完成（${result.rows ?? 0} 行已写入）`
      }
    }
    const rows = result.data
    const columns = rows.length > 0 && typeof rows[0] === 'object' && rows[0] !== null
      ? Object.keys(rows[0])
      : []
    return { columns, rows }
  }
  if (result.status === 'completed') {
    if (result.data?.columns && result.data?.data) {
      const rows = result.data.data.map(row => {
        const obj = {}
        result.data.columns.forEach((col, idx) => {
          obj[col] = row[idx]
        })
        return obj
      })
      return {
        columns: result.data.columns,
        rows
      }
    }
    return {
      columns: [],
      rows: [],
      message: `执行完成（${result.rows ?? 0} 行已写入）`
    }
  }
  if (result.status === 'failed') {
    return { error: result.error || '执行失败' }
  }
  if (result.status === 'pending' || result.status === 'running') {
    return {
      columns: [],
      rows: [],
      message: runningMessage || `任务执行中（${result.status}）`
    }
  }
  return {
    columns: [],
    rows: [],
    message: `任务状态：${result.status || 'unknown'}`
  }
}
