// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

export const taskExecutionPolicy = (analysis, selectedMode = 'sync') => {
  const continuousSource = analysis?.task_kind === 'batch' && analysis?.requires_async === true
  const requiresAsync = analysis?.requires_async === true ||
    analysis?.task_kind === 'stream' || analysis?.task_kind === 'mixed' || analysis?.statement_count > 1
  return {
    mode: requiresAsync ? 'async' : selectedMode,
    allowSync: !requiresAsync,
    continuousSource,
    notice: continuousSource ? '当前 SQL 使用连续采集源，已切换为异步执行。' : ''
  }
}
