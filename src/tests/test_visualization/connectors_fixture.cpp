// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <services/database/drivers/clickhouse_driver.h>
#include <services/database/drivers/mysql_driver.h>
#include <services/database/drivers/postgres_driver.h>
#include <services/visualization/workflow.h>
#include <tests/common/test_environment.h>

#include <cassert>
#include <iostream>
#include <regex>

using namespace flowsql;
using namespace flowsql::database;
using namespace flowsql::visualization;

int main(int argc, char** argv) {
    try {
        if (argc != 4 || !std::regex_match(argv[3], std::regex("vis_e2e_[a-f0-9]+"))) return 2;
        const std::string mode = argv[1], dialect = argv[2], table = argv[3];
        std::unique_ptr<IDbDriver> driver;
        if (dialect == "mysql")
            driver = std::make_unique<MysqlDriver>();
        else if (dialect == "postgres")
            driver = std::make_unique<PostgresDriver>();
        else if (dialect == "clickhouse")
            driver = std::make_unique<ClickHouseDriver>();
        else
            return 2;
        assert(driver->Connect(flowsql::test::DriverParameters(dialect)) == 0);
        auto* provider = dynamic_cast<IDbSessionFactoryProvider*>(driver.get());
        assert(provider);
        auto session = provider->CreateSession();
        const auto quoted = QuoteIdentifier(table, dialect);
        const auto schema = QuoteIdentifier(table + ".schema", dialect);
        const auto numeric = QuoteIdentifier(table + "_numeric", dialect);
        const auto statistics = QuoteIdentifier(table + "_statistics", dialect);
        auto execute = [&](const std::string& sql) { assert(session->ExecuteSql(sql.c_str()) >= 0); };
        if (mode == "seed") {
            const auto definition =
                dialect == "clickhouse"
                    ? "(host String,time String,value Nullable(Float64),x Float64,allnull Nullable(Float64),wide "
                      "Int64,precise Nullable(Decimal(38,20)),flag Bool,unsupported Array(Int64)) ENGINE=Memory"
                    : "(host VARCHAR(30),time VARCHAR(30),value " +
                          std::string(dialect == "postgres" ? "DOUBLE PRECISION" : "DOUBLE") + ",x " +
                          (dialect == "postgres" ? "DOUBLE PRECISION" : "DOUBLE") + ",allnull " +
                          (dialect == "postgres" ? "DOUBLE PRECISION" : "DOUBLE") +
                          ",wide BIGINT,precise DECIMAL(38,20),flag BOOLEAN,unsupported " +
                          (dialect == "postgres" ? "BYTEA" : "BLOB") + ")";
            execute("CREATE TABLE " + quoted + definition);
            const auto unsupported = dialect == "clickhouse" ? "[1,2]" : "NULL";
            execute("INSERT INTO " + quoted +
                    " VALUES('alpha','2026-01-01T00:00:00Z',10,1,NULL,9007199254740993,0.10000000000000000001,true," +
                    unsupported + "),('alpha','2026-01-01T00:01:00Z',NULL,2,NULL,9007199254740993,1.5,false," +
                    unsupported + "),('beta','2026-01-01T00:02:00Z',20,3,NULL,9007199254740993,2.5,true," +
                    unsupported + ")");
            execute("CREATE TABLE " + numeric +
                    (dialect == "clickhouse"
                         ? "(label String,precise Nullable(Decimal(38,20)),wide Nullable(Int64)) ENGINE=Memory"
                         : "(label VARCHAR(30),precise DECIMAL(38,20),wide BIGINT)"));
            execute("INSERT INTO " + numeric +
                    " VALUES('tenth',0.1,1),('exact',1.5,2),('precision',0.10000000000000000001,3),"
                    "('null',NULL,NULL),('unsafe',NULL,9007199254740993)");
            execute("CREATE TABLE " + statistics +
                    (dialect == "clickhouse"
                         ? "(stamp Nullable(Date),identity Nullable(String),wide Int64,event_at Nullable(DateTime),"
                           "zoned Nullable(DateTime64(3,'Asia/Shanghai')),allnull Nullable(String),"
                           "precise Nullable(Decimal(38,20)),flag Nullable(Bool)) ENGINE=Memory"
                         : "(stamp DATE,identity VARCHAR(30),wide BIGINT,event_at " +
                               std::string(dialect == "postgres" ? "TIMESTAMP" : "DATETIME") + ",zoned " +
                               (dialect == "postgres" ? "TIMESTAMPTZ" : "TIMESTAMP NULL") +
                               ",allnull VARCHAR(30),precise DECIMAL(38,20),flag BOOLEAN)"));
            // Seed an offset-aware instant; later baseline/API sessions retain their own native calendar.
            if (dialect == "mysql") execute("SET SESSION time_zone='+08:00'");
            const std::string offset = dialect == "postgres" ? "+08:00" : "";
            execute("INSERT INTO " + statistics +
                    " VALUES('2024-02-29','x',9007199254740993,'2024-02-29 23:30:00','2024-03-01 00:30:00" + offset +
                    "',NULL,0.10000000000000000001,true),"
                    "('2024-02-29','x',9007199254740992,'2024-02-29 01:00:00','2024-03-01 00:30:00" +
                    offset +
                    "',NULL,0.10000000000000000002,false),"
                    "('2024-03-01','',2,'2024-03-01 01:00:00','2024-03-02 00:30:00" +
                    offset +
                    "',NULL,0.10000000000000000001,true),"
                    "('2025-01-01',NULL,3,'2025-01-01 01:00:00','2025-01-01 00:30:00" +
                    offset + "',NULL,NULL,NULL),(NULL,'y',4,NULL,NULL,NULL,NULL,NULL)");
            if (dialect == "postgres") {
                execute("CREATE SCHEMA " + schema);
                execute("CREATE TABLE " + schema + "." + quoted + "(host TEXT)");
                execute("INSERT INTO " + schema + "." + quoted + " VALUES('other-schema')");
            }
        } else if (mode == "cleanup") {
            if (dialect == "postgres") execute("DROP SCHEMA IF EXISTS " + schema + " CASCADE");
            execute("DROP TABLE IF EXISTS " + statistics);
            execute("DROP TABLE IF EXISTS " + numeric);
            execute("DROP TABLE IF EXISTS " + quoted);
        } else if (mode == "baseline" || mode == "numeric_baseline" || mode == "statistics_baseline") {
            auto* begin = dynamic_cast<IDbSnapshotSession*>(session.get());
            auto* query = dynamic_cast<IDatabaseExplorationSessionV1*>(session.get());
            DatabaseSnapshotOptionsV1 options;
            options.consistent_transaction = dialect != "clickhouse";
            assert(begin && query && begin->BeginSnapshot(options) == 0);
            const auto avg = dialect == "clickhouse" ? "avgOrNull(value)" : "AVG(value)";
            std::vector<std::string> sql = {
                "SELECT host," + std::string(avg) + " FROM " + quoted + " GROUP BY host",
                "SELECT time," + std::string(avg) + " FROM " + quoted + " GROUP BY time",
                "SELECT x,value FROM " + quoted,
                "SELECT host," + std::string(avg) + " FROM " + quoted + " WHERE host='alpha' GROUP BY host"};
            if (mode == "numeric_baseline") {
                sql = {"SELECT label,precise FROM " + numeric};
                for (const auto& aggregate : {"sum", "mean", "min", "max"}) {
                    const auto function =
                        dialect == "clickhouse"
                            ? (std::string(aggregate) == "mean" ? "avgOrNull" : std::string(aggregate) + "OrNull")
                            : (std::string(aggregate) == "mean" ? "AVG" : std::string(aggregate));
                    sql.push_back("SELECT " + function + "(precise) FROM " + numeric);
                }
            }
            if (mode == "statistics_baseline") {
                sql.clear();
                for (const std::string field : {"stamp", "event_at", "zoned"}) {
                    for (const std::string unit : {"day", "month"}) {
                        const auto column = QuoteIdentifier(field, dialect);
                        std::string bucket;
                        if (dialect == "mysql")
                            bucket = unit == "day" ? "CAST(DATE(" + column + ") AS CHAR)"
                                                   : "CONCAT(LEFT(CAST(DATE(" + column + ") AS CHAR),7),'-01')";
                        else if (dialect == "postgres")
                            bucket = "CAST(CAST(date_trunc('" + unit + "'," + column + ") AS DATE) AS TEXT)";
                        else
                            bucket = "toString(" + std::string(unit == "day" ? "toDate(" : "toStartOfMonth(") + column +
                                     "))";
                        const auto identity = QuoteIdentifier("identity", dialect);
                        const auto count = dialect == "clickhouse" ? "uniqExact(" + identity + ")"
                                                                   : "COUNT(DISTINCT " + identity + ")";
                        sql.push_back("SELECT " + bucket + "," + count + " FROM " + statistics + " GROUP BY " + bucket);
                    }
                }
            }
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            writer.StartArray();
            for (const auto& statement : sql) {
                ExplorationResultV1 result;
                assert(query->Query(statement.c_str(), nullptr, 0, 100, 65536, &result) == 0);
                writer.StartArray();
                for (const auto& row : result.rows) {
                    writer.StartArray();
                    for (const auto& cell : row) {
                        if (std::holds_alternative<std::monostate>(cell))
                            writer.Null();
                        else if (auto value = std::get_if<int64_t>(&cell))
                            writer.Int64(*value);
                        else if (auto value = std::get_if<double>(&cell))
                            writer.Double(*value);
                        else if (auto value = std::get_if<std::string>(&cell))
                            writer.String(value->data(), value->size());
                        else if (auto value = std::get_if<ExplorationDecimalV1>(&cell))
                            writer.String(value->text.data(), value->text.size());
                        else
                            assert(false);
                    }
                    writer.EndArray();
                }
                writer.EndArray();
            }
            writer.EndArray();
            std::cout << "BASELINE " << buffer.GetString() << std::endl;
        } else
            return 2;
        return 0;
    } catch (...) {
        std::cerr << "connector fixture failed; inspect non-secret test assertion" << std::endl;
        return 1;
    }
}
