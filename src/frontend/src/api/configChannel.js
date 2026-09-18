// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

const pageLimit = (limit) => Number.isInteger(limit) ? Math.min(100, Math.max(1, limit)) : 100

export const createConfigChannelApi = (client) => ({
  listConfigChannels: (cursor = '', limit = 100) =>
    client.post('/api/channels/config/list', { cursor, limit: pageLimit(limit) }),
  publishConfigChannel: (payload) => client.post('/api/channels/config/publish', payload),
  listConfigHistory: (name, cursor = '', limit = 100) =>
    client.post('/api/channels/config/history', { name, cursor, limit: pageLimit(limit) }),
  resolveConfigChannel: (exactReference) =>
    client.post('/api/channels/config/resolve', { exact_reference: exactReference })
})
