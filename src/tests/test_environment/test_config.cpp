// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <tests/common/test_environment.h>

#include <iostream>

int main() {
    try {
        const auto parameters = flowsql::test::DatabaseParameters("mysql", "npm");
        if (parameters.at("database") != "test_npm") return 2;
        std::cout << parameters.at("host") << ':' << parameters.at("port") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
