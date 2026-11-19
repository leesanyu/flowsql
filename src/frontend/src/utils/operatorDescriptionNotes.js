import { clone } from './dataSemantics.js'
const textItems = (items, origin) => items.filter(item => item.origin === origin).map(item => item.text)

export function operatorDescriptionNotes({ usage }) {
  const annotations = usage.annotations
  const editableText = items => textItems(items, 'local')
  return {
    purpose: editableText(usage.identity.purposes).join('\n\n'),
    usage_notes: editableText(usage.limitations).join('\n\n'),
    examples: editableText(usage.examples),
    input_description: annotations.input_description ?? '',
    parameter_description: annotations.parameter_description ?? '',
    output_description: annotations.output_description ?? '',
    ...Object.fromEntries(['inputs', 'parameters', 'outputs'].map(section => [section, clone(annotations[section] || [])]))
  }
}
