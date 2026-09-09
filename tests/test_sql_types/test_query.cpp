// test_query.cpp
#include "test_framework.h"
#include "sql_types/query.h"
#include "sql_types/condition_types.h"

using namespace sql;

TEST(Query, SelectQuery) {
    SelectQuery q;
    q.table = Identifier("users");
    q.columns = {ColumnRef{Identifier("id"), {}, {}},
                 ColumnRef{Identifier("name"), {}, {}}};
    q.where = make_compare("age", CompareOp::GT, Value(18));
    
    Query query(std::move(q));
    CHECK(query.is_select());
    CHECK(!query.is_insert());
    CHECK(!query.is_update());
    CHECK(!query.is_delete());
    CHECK(query.is_dml());
    CHECK(!query.is_ddl());
    
    CHECK(query.type() == QueryType::SELECT);
    
    // 访问具体类型
    auto* select = query.select();
    CHECK(select != nullptr);
    CHECK_EQ(select->table.str(), "users");
    CHECK_EQ(select->columns.size(), 2);
    CHECK(select->where != nullptr);
}

TEST(Query, InsertQuery) {
    InsertQuery q;
    q.table = Identifier("users");
    q.columns = {Identifier("name"), Identifier("age")};
    q.values = {{Value(std::string("Alice")), Value(30)},
                {Value(std::string("Bob")), Value(25)}};
    
    Query query(std::move(q));
    CHECK(query.is_insert());
    CHECK(query.is_dml());
    CHECK(!query.is_ddl());
    CHECK(query.type() == QueryType::INSERT);
    
    auto* insert = query.insert();
    CHECK(insert != nullptr);
    CHECK_EQ(insert->table.str(), "users");
    CHECK_EQ(insert->columns.size(), 2);
    CHECK_EQ(insert->values.size(), 2);
    CHECK_EQ(insert->values[0][0].as_str(), "Alice");
}

TEST(Query, UpdateQuery) {
    UpdateQuery q;
    q.table = Identifier("users");
    q.assignments = {{Identifier("age"), Value(31)}};
    q.where = make_compare("name", CompareOp::EQ, Value(std::string("Alice")));
    
    Query query(std::move(q));
    CHECK(query.is_update());
    CHECK(query.is_dml());
    CHECK(query.type() == QueryType::UPDATE);
    
    auto* update = query.update();
    CHECK(update != nullptr);
    CHECK_EQ(update->table.str(), "users");
    CHECK_EQ(update->assignments.size(), 1);
}

TEST(Query, DeleteQuery) {
    DeleteQuery q;
    q.table = Identifier("users");
    q.where = make_compare("age", CompareOp::LT, Value(18));
    
    Query query(std::move(q));
    CHECK(query.is_delete());
    CHECK(query.is_dml());
    CHECK(query.type() == QueryType::DELETE);
    
    auto* del = query.delete_();
    CHECK(del != nullptr);
    CHECK_EQ(del->table.str(), "users");
}

TEST(Query, DDLQueries) {
    // CreateTable
    CreateTableQuery create_q;
    create_q.table = Identifier("users");
    create_q.columns = {
        ColumnDef{Identifier("id"), DataType::INT, true, false},
        ColumnDef{Identifier("name"), DataType::VARCHAR, false, false}
    };
    Query q1(std::move(create_q));
    CHECK(q1.is_create_table());
    CHECK(q1.is_ddl());
    CHECK(!q1.is_dml());
    
    // DropTable
    DropTableQuery drop_q;
    drop_q.table = Identifier("users");
    Query q2(std::move(drop_q));
    CHECK(q2.is_drop_table());
    CHECK(q2.is_ddl());
    
    // CreateDatabase
    CreateDatabaseQuery create_db;
    create_db.database = Identifier("mydb");
    Query q3(std::move(create_db));
    CHECK(q3.type() == QueryType::CREATE_DATABASE);
    
    // DropDatabase
    DropDatabaseQuery drop_db;
    drop_db.database = Identifier("mydb");
    Query q4(std::move(drop_db));
    CHECK(q4.type() == QueryType::DROP_DATABASE);
    
    // UseDatabase
    UseDatabaseQuery use_db;
    use_db.database = Identifier("mydb");
    Query q5(std::move(use_db));
    CHECK(q5.type() == QueryType::USE_DATABASE);
}

