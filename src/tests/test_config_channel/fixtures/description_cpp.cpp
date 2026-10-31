// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
// A second release of the existing test operator; execution code remains the same.
#define flowsql_operator_data_contract_v1 flowsql_description_original_contract
#include "../../test_builtin/fixtures/fixture_contract_output.cpp"
#undef flowsql_operator_data_contract_v1
EXPORT_API const char* flowsql_operator_data_contract_v1(int index) {
    static const std::string text = [] {
        Json d;
        d.Parse(Declaration().c_str());
        Set(d["operator"], "version", "2", d.GetAllocator());
        Set(d, "title", "Numerical fixture release two", d.GetAllocator());
        return Encode(d);
    }();
    return index ? nullptr : text.c_str();
}
