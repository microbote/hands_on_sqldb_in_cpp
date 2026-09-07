#pragma once

#include "catalog.h"

namespace sql{

class DatabaseView {
 public:
  DatabaseView(Catalog& catalog, const std::string& db_name)
      : catalog_(catalog), db_name_(db_name) {}

  std::optional<TableSchema> get_table_schema(const std::string& table) const {
    return catalog_.get_table_schema(db_name_, table);
  }
  std::vector<std::string> list_tables() const {
    return catalog_.list_tables(db_name_);
  }
  bool table_exists(const std::string& table) const {
    return catalog_.table_exists(db_name_, table);
  }

 private:
  Catalog& catalog_;           // 不是指针，避免空指针
  std::string db_name_;
};

}