// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_CONSUMER_H_
#define FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_CONSUMER_H_

#include <framework/interfaces/idatabase_channel.h>
#include <operators/npm_basic/npm_protocol_contract.h>

#include <memory>
#include <optional>
#include <string>

namespace flowsql::npm {

/** Database implementation for the task-private managed result consumer factory. */
__attribute__((visibility("default"))) std::unique_ptr<INpmResultConsumerFactoryV1>
MakeNpmDatabaseResultConsumerFactory(IDatabaseChannel* channel, std::string input_namespace,
                                     std::optional<uint32_t> retention_days = std::nullopt);

}  // namespace flowsql::npm

#endif  // FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_CONSUMER_H_
