#pragma once

#include "catalog.h"
#include "identifier.h"

namespace sql{

class DatabaseView {
 public:
  DatabaseView(Catalog& catalog, const Identifier& db_name)
      : catalog_(catalog), db_name_(db_name) {}

  std::optional<TableSchema> get_table_schema(const Identifier& table) const {
    return catalog_.get_table_schema(db_name_, table);
  }
  std::vector<Identifier> list_tables() const {
    return catalog_.list_tables(db_name_);
  }
  bool table_exists(const Identifier& table) const {
    return catalog_.table_exists(db_name_, table);
  }

 private:
  Catalog& catalog_;           // 不是指针，避免空指针
  Identifier db_name_;
};

}