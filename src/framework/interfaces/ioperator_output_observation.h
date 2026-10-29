// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <common/typedef.h>
#include <functional>
#include <string>
namespace flowsql {
// Optional producer capability, independent of every existing execution vtable.
// Returns a copied JSON array [{port_id,state,snapshot?,row_count?}] captured from
// actual created outputs. Never manufacture an observation from a static contract.
// ENOTSUP means this producer has no additional observed ports.
interface IOperatorOutputObservationV1 {
    virtual ~IOperatorOutputObservationV1() = default;
    virtual int OutputObservations(std::string * observations_json) const = 0;
};
// Optional notification after durable publication. The task copies the callback,
// invokes it serially with its lifecycle, and never calls it after task destruction.
// A nonzero result reports metadata failure after data has already been committed.
interface IOperatorOutputPublicationV1 {
    virtual ~IOperatorOutputPublicationV1() = default;
    virtual void SetOutputPublicationObserver(std::function<int()> observer) = 0;
};
}  // namespace flowsql
