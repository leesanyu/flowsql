// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>
#include <framework/interfaces/iblock_transform_operator.h>

#include "npm_basic_operator.h"

BEGIN_PLUGIN_REGIST(flowsql::npm::NpmBasicOperator)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_BLOCK_TRANSFORM_OPERATOR_V1,
                  flowsql::IBlockTransformOperatorV1)
END_PLUGIN_REGIST()
