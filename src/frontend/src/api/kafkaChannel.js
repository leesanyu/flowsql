// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
export const createKafkaChannelApi = (client) => ({
  kafkaDefinitions: () => client.post('/api/channels/kafka/definitions/query', {}),
  kafkaQuery: (kind, after = '') => client.post(`/api/channels/kafka/${kind}/query`, { after, limit: 100 }),
  kafkaSave: (kind, payload, editing = false) => client.post(`/api/channels/kafka/${kind}/${editing ? 'modify' : 'add'}`, payload),
  kafkaRemove: (kind, row) => client.post(`/api/channels/kafka/${kind}/remove`, { name: row.name, expected_revision: row.revision }),
  kafkaTestStart: (payload) => client.post('/api/channels/kafka/tests/start', payload),
  kafkaTestQuery: (test_id) => client.post('/api/channels/kafka/tests/query', { test_id }),
  kafkaTestCancel: (test_id) => client.post('/api/channels/kafka/tests/cancel', { test_id })
})
