const textItems = (items, origin) => items.filter(item => item.origin === origin).map(item => item.text)
const sectionText = items => {
  const descriptions = items.filter(item => item.description)
  return descriptions.map(item => descriptions.length === 1 ? item.description : `${item.port_id}：${item.description}`).join('\n\n')
}

export function operatorDescriptionNotes({ usage, basis }) {
  const annotations = usage.annotations
  const initialized = basis.description_ref && ['input_description', 'parameter_description', 'output_description'].every(key => Object.hasOwn(annotations, key))
  const editableText = items => {
    const local = textItems(items, 'local')
    return local.length || initialized ? local : textItems(items, 'author')
  }
  return {
    purpose: editableText(usage.identity.purposes).join('\n\n'),
    usage_notes: editableText(usage.limitations).join('\n\n'),
    examples: editableText(usage.examples),
    input_description: annotations.input_description ?? sectionText(usage.inputs),
    parameter_description: annotations.parameter_description ?? usage.parameters.description ?? '',
    output_description: annotations.output_description ?? sectionText(usage.outputs),
    ...Object.fromEntries(['inputs', 'parameters', 'outputs'].map(section => [section, JSON.parse(JSON.stringify(annotations[section]))]))
  }
}
