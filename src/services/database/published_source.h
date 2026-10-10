// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/idatabase_published_source.h>
namespace flowsql::database {
std::shared_ptr<IDatabasePublishedProgressReaderV1> MakeNpmPublishedProgressReader(IDatabaseChannel* source,
                                                                                   DatabaseSnapshotOptionsV1 options);
}
