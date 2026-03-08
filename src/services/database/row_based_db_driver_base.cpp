// 注意：此文件中的实现已移至各具体数据库驱动中
// (mysql_driver.cpp, sqlite_driver.cpp)
//
// RowBasedBatchReader 和 RowBasedBatchWriter 现在作为各驱动的內部类实现
// 每个驱动根据自身的 API 特性实现批量读写功能

#include "row_based_db_driver_base.h"

namespace flowsql {
namespace database {

// 空的实现文件，所有功能已移至具体驱动
// 保留此文件用于未来可能的公共功能

}  // namespace database
}  // namespace flowsql
