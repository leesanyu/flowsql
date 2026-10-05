// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

export const normalizeStreamOptions = (schema, options, arrayInputs) => {
  const out = {}
  for (const field of schema) {
    let value = options[field.key]
    if (field.type === 'array') {
      value = (arrayInputs[field.key] || '').split(',').map(item => item.trim()).filter(Boolean)
    } else if (field.type === 'int') {
      value = Number(value)
      if (!Number.isInteger(value) || (field.has_range &&
          (value < field.min_value || (field.max_value > 0 && value > field.max_value)))) {
        throw new Error(`参数 ${field.key} 必须是范围内的整数`)
      }
    } else if (field.type === 'bool') {
      value = !!value
    }
    if (field.required && (value === '' || value === null || value === undefined ||
        (Array.isArray(value) && value.length === 0))) {
      throw new Error(`参数 ${field.key} 为必填`)
    }
    if (field.type === 'enum' && !(field.enum_values || []).includes(value)) {
      throw new Error(`参数 ${field.key} 必须选择有效值`)
    }
    out[field.key] = value
  }
  return out
}
