import React, { useEffect, useRef } from 'react'
import { resultReading } from '../../frontend/src/utils/semanticReading.js'
import { showJsonReading } from '../../frontend/src/utils/semanticJsonDialog.js'

const currentLink = target => target.kind === 'channel' ? '/channels?dataset=' + encodeURIComponent(JSON.stringify(target.locator)) : '/operators?name=' + encodeURIComponent(target.name)
function managementOrigin() {
  if (window.parent !== window) return location.origin
  const configured = new URLSearchParams(location.search).get('web_url')
  if (!configured) return null
  try {
    const target = new URL(configured)
    return ['http:', 'https:'].includes(target.protocol) ? target.origin : null
  } catch (error) {
    if (error instanceof TypeError) return null
    throw error
  }
}

export default function SemanticPanel({ context, title, selectedIds = null, fieldLabels = {} }) {
  const reader = useRef(null)
  const selection = selectedIds?.join(',')
  useEffect(() => () => reader.current?.close(), [context, selection])
  function open() {
    const management = managementOrigin()
    const indexes = selectedIds === null ? null : context?.fields.filter(field => selectedIds.includes(field.output_name)).map(field => field.output_index) || []
    const value = resultReading(context, indexes, fieldLabels)
    if (!management) value.reading.management = '独立入口未设置有效 Web 地址；可在 Web 按 JSON 中的当前对象打开管理页。'
    reader.current = showJsonReading({ title, value,
      links: management ? (context?.sources || []).map((source, i) => ({ label: '当前对象管理页' + (i ? ' ' + (i + 1) : ''), href: management + currentLink(source.target) })) : []
    })
  }
  return <button type="button" className="semantic-trigger" data-semantic-panel={title} onClick={open}>{title}</button>
}
