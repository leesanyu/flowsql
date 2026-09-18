// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_CHANNELS_CONFIG_CONFIG_CONTENT_VALIDATOR_H_
#define _FLOWSQL_CHANNELS_CONFIG_CONFIG_CONTENT_VALIDATOR_H_

#include <string>

namespace flowsql::channels::config {

int ValidateConfigContent(const std::string& format,
                          const std::string& content,
                          std::string* error);

}  // namespace flowsql::channels::config

#endif  // _FLOWSQL_CHANNELS_CONFIG_CONFIG_CONTENT_VALIDATOR_H_
