// statement/source_span.cpp
#include "source_span.h"

#include <memory>
#include <utility>

namespace stmt {

namespace {

void collect_from_condition(const ASTNode *node, std::vector<NamedSpan> &out);

void collect_from_list(const ASTNode *list_node, std::vector<NamedSpan> &out) {
  if (list_node == nullptr || list_node->type != NODE_LIST) {
    return;
  }
  const auto *list = reinterpret_cast<const ASTNodeList *>(list_node->data);
  for (const ASTNode *item = list->head; item != nullptr; item = item->next) {
    switch (item->type) {
    case NODE_IDENT: {
      const auto *ident = reinterpret_cast<const IdentNode *>(item->data);
      out.push_back({sql::Identifier(ident->name != nullptr ? ident->name : ""),
                     item->span});
      break;
    }
    case NODE_ORDER: {
      const auto *order = reinterpret_cast<const OrderNode *>(item->data);
      out.push_back(
          {sql::Identifier(order->column != nullptr ? order->column : ""),
           item->span});
      break;
    }
    case NODE_ASSIGNMENT: {
      const auto *assign = reinterpret_cast<const AssignmentNode *>(item->data);
      out.push_back(
          {sql::Identifier(assign->column != nullptr ? assign->column : ""),
           item->span});
      break;
    }
    case NODE_COLUMN_DEF: {
      const auto *column = reinterpret_cast<const ColumnDefNode *>(item->data);
      out.push_back(
          {sql::Identifier(column->name != nullptr ? column->name : ""),
           item->span});
      break;
    }
    case NODE_LIST:
      // INSERT 的值列表等：里面没有名字
      break;
    default:
      break;
    }
  }
}

void collect_from_condition(const ASTNode *node, std::vector<NamedSpan> &out) {
  if (node == nullptr) {
    return;
  }
  switch (node->type) {
  case NODE_COMPARE: {
    const auto *compare = reinterpret_cast<const CompareNode *>(node->data);
    out.push_back(
        {sql::Identifier(compare->column != nullptr ? compare->column : ""),
         node->span});
    break;
  }
  case NODE_IN:
  case NODE_NOT_IN: {
    const auto *in = reinterpret_cast<const InNode *>(node->data);
    out.push_back(
        {sql::Identifier(in->column != nullptr ? in->column : ""), node->span});
    break;
  }
  case NODE_BINARY_OP: {
    const auto *binary = reinterpret_cast<const BinaryOpNode *>(node->data);
    collect_from_condition(binary->left, out);
    collect_from_condition(binary->right, out);
    break;
  }
  case NODE_NOT: {
    const auto *not_node = reinterpret_cast<const NotNode *>(node->data);
    collect_from_condition(not_node->child, out);
    break;
  }
  default:
    break;
  }
}

void collect_from_statement(const ASTNode *ast, std::vector<NamedSpan> &out) {
  if (ast == nullptr) {
    return;
  }
  switch (ast->type) {
  case NODE_SELECT: {
    const auto *select = reinterpret_cast<const SelectNode *>(ast->data);
    out.push_back(
        {sql::Identifier(select->table != nullptr ? select->table : ""),
         select->table_span});
    collect_from_list(select->columns, out);
    collect_from_condition(select->condition, out);
    collect_from_list(select->order_by, out);
    break;
  }
  case NODE_INSERT: {
    const auto *insert = reinterpret_cast<const InsertNode *>(ast->data);
    out.push_back(
        {sql::Identifier(insert->table != nullptr ? insert->table : ""),
         insert->table_span});
    collect_from_list(insert->columns, out);
    break;
  }
  case NODE_UPDATE: {
    const auto *update = reinterpret_cast<const UpdateNode *>(ast->data);
    out.push_back(
        {sql::Identifier(update->table != nullptr ? update->table : ""),
         update->table_span});
    collect_from_list(update->assignments, out);
    collect_from_condition(update->condition, out);
    break;
  }
  case NODE_DELETE: {
    const auto *del = reinterpret_cast<const DeleteNode *>(ast->data);
    out.push_back({sql::Identifier(del->table != nullptr ? del->table : ""),
                   del->table_span});
    collect_from_condition(del->condition, out);
    break;
  }
  case NODE_CREATE_TABLE: {
    const auto *create = reinterpret_cast<const CreateTableNode *>(ast->data);
    out.push_back({sql::Identifier(
                       create->table_name != nullptr ? create->table_name : ""),
                   create->table_span});
    collect_from_list(create->columns, out);
    break;
  }
  case NODE_DROP_TABLE: {
    const auto *drop = reinterpret_cast<const DropTableNode *>(ast->data);
    out.push_back(
        {sql::Identifier(drop->table_name != nullptr ? drop->table_name : ""),
         drop->table_span});
    break;
  }
  case NODE_USE:
  case NODE_CREATE_DATABASE:
  case NODE_DROP_DATABASE: {
    const auto *db = reinterpret_cast<const DatabaseNode *>(ast->data);
    out.push_back({sql::Identifier(db->db_name != nullptr ? db->db_name : ""),
                   db->db_span});
    break;
  }
  default:
    break;
  }
}

} // namespace

std::vector<NamedSpan> collect_source_spans(const ASTNode *ast) {
  std::vector<NamedSpan> spans;
  collect_from_statement(ast, spans);
  return spans;
}

SpanResolver make_span_resolver(const ASTNode *ast) {
  auto spans =
      std::make_shared<std::vector<NamedSpan>>(collect_source_spans(ast));
  return [spans](const sql::Identifier &name) -> SSpan {
    for (const auto &entry : *spans) {
      if (!entry.name.empty() && entry.name == name) {
        return entry.span;
      }
    }
    return sspan_unknown();
  };
}

} // namespace stmt
