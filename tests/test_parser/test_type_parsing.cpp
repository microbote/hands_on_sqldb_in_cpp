// tests/test_parser/test_type_parsing.cpp
//
// 类型解析：别名表（唯一数据源）、VARCHAR(n)/CHAR(n)、长度边界、
// 以及与 sql_types 别名表的一致性。
#include "test_framework.h"

#include "common/c_types.h"
#include "parser/parser.h"
#include "sql_types/field_type.h"

#include <string>
#include <vector>

namespace {

// 解析一条 CREATE TABLE，并断言成功
parser::ASTNodePtr parse_create(const std::string& column_def) {
    parser::Parser p;
    std::string sql = "CREATE TABLE t (" + column_def + ");";
    auto result = p.parse(sql);
    return result.success ? std::move(result.ast) : nullptr;
}

// 取 CREATE TABLE 的第 index 个列定义节点
const ColumnDefNode* column_def_at(const ASTNode* create_table, int index) {
    if (create_table == nullptr || create_table->type != NODE_CREATE_TABLE) {
        return nullptr;
    }
    const auto* data = (const CreateTableNode*)create_table->data;
    const auto* list = (const ASTNodeList*)data->columns->data;
    const ASTNode* item = list->head;
    for (int i = 0; i < index && item != nullptr; ++i) {
        item = item->next;
    }
    if (item == nullptr || item->type != NODE_COLUMN_DEF) {
        return nullptr;
    }
    return (const ColumnDefNode*)item->data;
}

}  // namespace

TEST(TypeParsing, EveryAliasIsAcceptedAndMapped) {
    // 表驱动：直接遍历 common/c_types.h 的别名表
    for (int i = 0; i < CTYPE_ALIAS_COUNT; ++i) {
        const CTypeAlias& alias = c_type_aliases[i];
        // "null" 是字面量关键字，不是列类型：单独在下面断言它被拒绝
        if (alias.type == DT_NULL) {
            continue;
        }
        auto ast = parse_create(std::string("c ") + alias.name);
        CHECK(ast != nullptr);
        if (ast == nullptr) {
            continue;
        }
        const ColumnDefNode* col = column_def_at(ast.get(), 0);
        CHECK(col != nullptr);
        if (col != nullptr) {
            CHECK(col->data_type == alias.type);
            CHECK_EQ(col->length, 0u);
        }
    }

    // NULL 只能作为字面量，不能作为列类型
    CHECK(parse_create("c NULL") == nullptr);
}

TEST(TypeParsing, NewTypesAreAccepted) {
    const char* defs[] = {
        "id TINYINT PRIMARY KEY",
        "id SMALLINT PRIMARY KEY",
        "id INT PRIMARY KEY",
        "id BIGINT PRIMARY KEY",
        "id INT8 PRIMARY KEY",
        "id INT16 PRIMARY KEY",
        "id INT32 PRIMARY KEY",
        "id INT64 PRIMARY KEY",
        "d DATE",
        "t TIME",
        "dt DATETIME",
        "ts TIMESTAMP",
        "b BOOLEAN",
        "c CHAR",
        "v VARCHAR",
        "x TEXT",
    };
    for (const char* def : defs) {
        auto ast = parse_create(def);
        CHECK(ast != nullptr);
    }
}

TEST(TypeParsing, StringLengthIsCaptured) {
    auto ast = parse_create(
        "id INT PRIMARY KEY, code CHAR(8), name VARCHAR(32), body TEXT");
    CHECK(ast != nullptr);
    if (ast == nullptr) {
        return;
    }

    const ColumnDefNode* id = column_def_at(ast.get(), 0);
    const ColumnDefNode* code = column_def_at(ast.get(), 1);
    const ColumnDefNode* name = column_def_at(ast.get(), 2);
    const ColumnDefNode* body = column_def_at(ast.get(), 3);

    CHECK(id != nullptr && code != nullptr && name != nullptr && body != nullptr);
    if (code != nullptr) {
        CHECK(code->data_type == DT_CHAR);
        CHECK_EQ(code->length, 8u);
    }
    if (name != nullptr) {
        CHECK(name->data_type == DT_VARCHAR);
        CHECK_EQ(name->length, 32u);
    }
    if (body != nullptr) {
        CHECK(body->data_type == DT_TEXT);
        CHECK_EQ(body->length, 0u);
    }
}

TEST(TypeParsing, LengthBoundariesAreEnforcedAtParseTime) {
    // 合法边界
    CHECK(parse_create("c CHAR(1)") != nullptr);
    CHECK(parse_create("c CHAR(255)") != nullptr);
    CHECK(parse_create("c VARCHAR(1)") != nullptr);
    CHECK(parse_create("c VARCHAR(65535)") != nullptr);

    // 非法：0 / 超上限 / 不接受长度的类型
    CHECK(parse_create("c CHAR(0)") == nullptr);
    CHECK(parse_create("c CHAR(256)") == nullptr);
    CHECK(parse_create("c VARCHAR(0)") == nullptr);
    CHECK(parse_create("c VARCHAR(65536)") == nullptr);
    CHECK(parse_create("c TEXT(10)") == nullptr);
    CHECK(parse_create("c INT(4)") == nullptr);
    CHECK(parse_create("c DATE(8)") == nullptr);
}

TEST(TypeParsing, LengthMustBeAnIntegerLiteral) {
    CHECK(parse_create("c VARCHAR(abc)") == nullptr);
    CHECK(parse_create("c VARCHAR(-1)") == nullptr);
    CHECK(parse_create("c VARCHAR(1.5)") == nullptr);
    CHECK(parse_create("c VARCHAR()") == nullptr);
}

TEST(TypeParsing, UnknownTypeIsRejected) {
    CHECK(parse_create("c FLOAT") == nullptr);
    CHECK(parse_create("c BLOB") == nullptr);
    CHECK(parse_create("c") == nullptr);
}

TEST(TypeParsing, TypeAliasTablesStayInSync) {
    // parser 用 C 别名表，sql_types 用自己那份实现：
    // 这里断言两边对同一个名字给出同一个类型，避免两份定义漂移。
    for (int i = 0; i < CTYPE_ALIAS_COUNT; ++i) {
        const CTypeAlias& alias = c_type_aliases[i];
        const sql::DataType from_cpp = sql::string_to_data_type(alias.name);
        CHECK(from_cpp == sql::from_c(alias.type));
    }

    // DataType <-> CDataType 往返一致
    const sql::DataType all[] = {
        sql::DataType::TINYINT,  sql::DataType::SMALLINT,
        sql::DataType::INT,      sql::DataType::BIGINT,
        sql::DataType::CHAR,     sql::DataType::VARCHAR,
        sql::DataType::TEXT,     sql::DataType::BOOLEAN,
        sql::DataType::DATE,     sql::DataType::TIME,
        sql::DataType::DATETIME, sql::DataType::NULL_TYPE,
        sql::DataType::UNKNOWN_TYPE,
    };
    for (sql::DataType type : all) {
        CHECK(sql::from_c(sql::to_c(type)) == type);
    }
}

TEST(TypeParsing, LengthLimitsMatchSqlTypes) {
    // 长度上限也应该只有一份定义
    CHECK_EQ(c_type_max_length(DT_CHAR), sql::kMaxCharLength);
    CHECK_EQ(c_type_max_length(DT_VARCHAR), sql::kMaxVarcharLength);
    CHECK_EQ(CTYPE_MAX_TEXT_LEN, sql::kMaxTextLength);
    CHECK(c_type_length_valid(DT_CHAR, sql::kMaxCharLength) != 0);
    CHECK(c_type_length_valid(DT_CHAR, sql::kMaxCharLength + 1) == 0);
}
