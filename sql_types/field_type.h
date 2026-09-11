#pragma once

#include "common/c_types.h"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace sql {

// ============================================================
// SQL 数据类型枚举
//
// 设计约定（与存储层一致）：
//   - 整型只有"逻辑"区分：TINYINT/SMALLINT/INT/BIGINT 在 Value 里
//     一律用 int64_t 存储与编码；区别只体现在 schema 类型与写入/读取
//     时的取值范围校验上。
//   - 同一 family 内的值可以直接相互比较（类型提升到更宽的类型）。
// ============================================================
enum class DataType : uint8_t {
  // ---- 整型 family（底层统一 int64_t）----
  TINYINT = 0,
  SMALLINT,
  INT,
  BIGINT,
  // ---- 字符串 family ----
  CHAR,      // 定长字符串（声明长度，上限 255 字节）
  VARCHAR,
  TEXT,
  // ---- 布尔 ----
  BOOLEAN,
  // ---- 时间 family ----
  DATE,      // 距 1970-01-01 的天数
  TIME,      // 自 00:00:00 起的秒数 [0, 86399]
  DATETIME,  // Unix 秒（UTC，暂不支持时区）
  // ---- 特殊 ----
  NULL_TYPE,
  UNKNOWN_TYPE
};

enum class DataTypeClass : uint8_t {
  INTEGER,
  STRING,
  BOOLEAN,
  TEMPORAL,
  UNKNOWN_CLASS
};

// ------------------------------------------------------------
// family 判定
// ------------------------------------------------------------

inline bool is_integer(DataType type) {
  return type == DataType::TINYINT || type == DataType::SMALLINT ||
         type == DataType::INT || type == DataType::BIGINT;
}

inline bool is_string(DataType type) {
  return type == DataType::CHAR || type == DataType::VARCHAR ||
         type == DataType::TEXT;
}

inline bool is_boolean(DataType type) { return type == DataType::BOOLEAN; }

inline bool is_temporal(DataType type) {
  return type == DataType::DATE || type == DataType::TIME ||
         type == DataType::DATETIME;
}

// MySQL 里 BOOLEAN 就是 TINYINT(1)，参与数值比较
inline bool is_numeric(DataType type) {
  return is_integer(type) || is_boolean(type);
}

inline bool is_null(DataType type) {
  return type == DataType::NULL_TYPE || type == DataType::UNKNOWN_TYPE;
}

inline bool is_orderable(DataType type) {
  return is_integer(type) || is_string(type) || is_boolean(type) ||
         is_temporal(type);
}

inline bool is_hashable(DataType type) { return is_orderable(type); }

inline bool is_indexable(DataType type) { return is_orderable(type); }

inline DataTypeClass get_type_class(DataType type) {
  if (is_integer(type)) {
    return DataTypeClass::INTEGER;
  }
  if (is_string(type)) {
    return DataTypeClass::STRING;
  }
  if (is_boolean(type)) {
    return DataTypeClass::BOOLEAN;
  }
  if (is_temporal(type)) {
    return DataTypeClass::TEMPORAL;
  }
  return DataTypeClass::UNKNOWN_CLASS;
}

// ------------------------------------------------------------
// 整型的字节宽度与取值范围（schema 校验、类型提升用）
// ------------------------------------------------------------

inline int integer_bytes(DataType type) {
  switch (type) {
  case DataType::TINYINT:
    return 1;
  case DataType::SMALLINT:
    return 2;
  case DataType::INT:
    return 4;
  case DataType::BIGINT:
    return 8;
  default:
    return 0;
  }
}

inline int64_t integer_min(DataType type) {
  switch (integer_bytes(type)) {
  case 1:
    return -128;
  case 2:
    return -32768;
  case 4:
    return INT32_MIN;
  case 8:
    return INT64_MIN;
  default:
    return 0;
  }
}

inline int64_t integer_max(DataType type) {
  switch (integer_bytes(type)) {
  case 1:
    return 127;
  case 2:
    return 32767;
  case 4:
    return INT32_MAX;
  case 8:
    return INT64_MAX;
  default:
    return 0;
  }
}

// ------------------------------------------------------------
// 时间类型的取值范围（与 temporal.h 的解析保持一致）
//   DATE     : 0001-01-01 ~ 9999-12-31
//   TIME     : 00:00:00 ~ 23:59:59
//   DATETIME : 0001-01-01 00:00:00 ~ 9999-12-31 23:59:59
// ------------------------------------------------------------
inline constexpr int64_t kDateMinDays = -719162;   // 0001-01-01
inline constexpr int64_t kDateMaxDays = 2932896;   // 9999-12-31
inline constexpr int64_t kTimeMinSeconds = 0;
inline constexpr int64_t kTimeMaxSeconds = 86399;  // 23:59:59
inline constexpr int64_t kSecondsPerDay = 86400;
inline constexpr int64_t kDateTimeMinSeconds = kDateMinDays * kSecondsPerDay;
inline constexpr int64_t kDateTimeMaxSeconds =
    kDateMaxDays * kSecondsPerDay + kTimeMaxSeconds;

// ------------------------------------------------------------
// 字符串类型的容量（字节）。
//
// 与整型"逻辑宽度只在 schema/校验里"的约定一致：字符串的声明长度同样
// 只影响 schema 校验，不进入 Value 存储、也不进入 key 编码。
//   CHAR(n)     : 声明长度 n，n ∈ [1, 255]，缺省 255
//   VARCHAR(n)  : 声明长度 n，n ∈ [1, 65535]，缺省 65535（等价 TEXT 容量）
//   TEXT        : 65535，不接受声明长度
// 长度单位是 **字节**（UTF-8 中文一个字 3 字节）。
// ------------------------------------------------------------
inline constexpr uint32_t kMaxCharLength = 255;
inline constexpr uint32_t kMaxVarcharLength = 65535;
inline constexpr uint32_t kMaxTextLength = 65535;

// 该字符串类型的实际容量（declared_length = 0 表示未声明）
inline uint32_t string_capacity(DataType type, uint32_t declared_length = 0) {
  switch (type) {
  case DataType::CHAR:
    return declared_length == 0 ? kMaxCharLength : declared_length;
  case DataType::VARCHAR:
    return declared_length == 0 ? kMaxVarcharLength : declared_length;
  case DataType::TEXT:
    return kMaxTextLength;
  default:
    return 0;
  }
}

// 声明的长度本身是否合法（用于 DDL 校验）。
// 0 表示"未声明"，对任何类型都合法（各自使用默认容量）；
// 只有**显式写出**的长度才会被这里判非法（如 CHAR(300) / TEXT(10)）。
inline bool valid_declared_length(DataType type, uint32_t declared_length) {
  if (declared_length == 0) {
    return true;   // 未声明
  }
  switch (type) {
  case DataType::CHAR:
    return declared_length <= kMaxCharLength;
  case DataType::VARCHAR:
    return declared_length <= kMaxVarcharLength;
  case DataType::TEXT:
    return declared_length == 0;   // TEXT 不接受声明长度
  default:
    return false;                  // 非字符串类型不能带长度
  }
}

// 字符串内容长度是否放得进该列（长度单位为字节）
inline bool can_represent_length(DataType type, uint32_t declared_length,
                                 size_t byte_length) {
  return byte_length <= string_capacity(type, declared_length);
}

// 字符串的类型提升结果容量：与整型"取更宽的类型"同理，取两个容量的较大者。
// 用于 UNION / CASE 这类"结果列需要多长"的推导（比较本身不需要容量）。
// 注意：CONCAT(a, b) 这类表达式的结果长度是**两者之和**，由 SQL 层决定，
// 不要用这里的 max 规则。
inline uint32_t common_string_capacity(DataType a, uint32_t a_length,
                                      DataType b, uint32_t b_length) {
  const uint32_t cap_a = string_capacity(a, a_length);
  const uint32_t cap_b = string_capacity(b, b_length);
  return cap_a >= cap_b ? cap_a : cap_b;
}

// int64 表示的该类型值是否在合法范围内
inline bool can_represent(DataType type, int64_t v) {
  if (is_integer(type)) {
    return v >= integer_min(type) && v <= integer_max(type);
  }
  if (is_boolean(type)) {
    return v == 0 || v == 1;
  }
  switch (type) {
  case DataType::DATE:
    return v >= kDateMinDays && v <= kDateMaxDays;
  case DataType::TIME:
    return v >= kTimeMinSeconds && v <= kTimeMaxSeconds;
  case DataType::DATETIME:
    return v >= kDateTimeMinSeconds && v <= kDateTimeMaxSeconds;
  default:
    return true;
  }
}

// ============================================================
// 类型族（type family）
//
// TINYINT/SMALLINT/INT/BIGINT 底层都是 int64_t，VARCHAR/TEXT 底层都是
// 字符串，DATE/DATETIME 都是"从纪元起的偏移量"。因此列类型与值类型的
// 一致性判定必须按 *族*，否则 Value(int64_t(5)) 永远写不进 BIGINT 列。
// ============================================================
inline bool is_same_family(DataType a, DataType b) {
  if (a == b) {
    return true;
  }
  if (is_integer(a) && is_integer(b)) {
    return true;
  }
  if (is_string(a) && is_string(b)) {
    return true;
  }
  if (is_temporal(a) && is_temporal(b)) {
    return true;
  }
  return false;
}

// 类型提升：两个类型比较/运算时使用的公共类型。
// UNKNOWN_TYPE 表示两者不同族（是否强制转换由 SQL 层决定）。
inline DataType common_type(DataType a, DataType b) {
  if (a == b) {
    return a;
  }
  if (is_integer(a) && is_integer(b)) {
    return integer_bytes(a) >= integer_bytes(b) ? a : b;
  }
  if (is_boolean(a) && is_integer(b)) {
    return b;
  }
  if (is_integer(a) && is_boolean(b)) {
    return a;
  }
  if (is_boolean(a) && is_boolean(b)) {
    return DataType::BOOLEAN;
  }
  if (is_string(a) && is_string(b)) {
    // 与整型一样提升到"更宽"的类型：CHAR -> VARCHAR -> TEXT
    // （容量是 schema 级信息，比较时只按字节序，不做截断）
    return (a == DataType::TEXT || b == DataType::TEXT) ? DataType::TEXT
                                                        : DataType::VARCHAR;
  }
  // DATE 与 DATETIME 提升到 DATETIME；TIME 与日期无关，
  // 与 DATE/DATETIME 混用不做隐式转换（SQL 层判 UNKNOWN）。
  if (is_temporal(a) && is_temporal(b)) {
    if (a == DataType::TIME || b == DataType::TIME) {
      return a == b ? a : DataType::UNKNOWN_TYPE;
    }
    return DataType::DATETIME;
  }
  return DataType::UNKNOWN_TYPE;
}

// 大小写不敏感的 ASCII 比较（不分配内存）
inline bool iequals_ascii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// 严格整数解析：整串必须是十进制数字（"12abc" 视为非法），不抛异常
inline bool parse_int64_strict(std::string_view text, int64_t& out) {
  if (text.empty()) {
    return false;
  }
  size_t begin = 0;
  if (text[0] == '+' || text[0] == '-') {
    begin = 1;
  }
  if (begin == text.size()) {
    return false;
  }
  const char* first = text.data() + begin;
  const char* last = text.data() + text.size();
  const auto result = std::from_chars(first, last, out);
  return result.ec == std::errc() && result.ptr == last;
}

// ============================================================
// 数据类型名称
// ============================================================
inline const char *data_type_name(DataType type) {
  switch (type) {
  case DataType::TINYINT:
    return "TINYINT";
  case DataType::SMALLINT:
    return "SMALLINT";
  case DataType::INT:
    return "INT";
  case DataType::BIGINT:
    return "BIGINT";
  case DataType::CHAR:
    return "CHAR";
  case DataType::VARCHAR:
    return "VARCHAR";
  case DataType::TEXT:
    return "TEXT";
  case DataType::BOOLEAN:
    return "BOOLEAN";
  case DataType::DATE:
    return "DATE";
  case DataType::TIME:
    return "TIME";
  case DataType::DATETIME:
    return "DATETIME";
  case DataType::NULL_TYPE:
    return "NULL";
  default:
    return "UNKNOWN";
  }
}

// ============================================================
// string_to_data_type：字符串转 DataType（大小写不敏感）
//   TINYINT / INT8              -> TINYINT
//   SMALLINT / INT16            -> SMALLINT
//   INT / INTEGER / INT32       -> INT
//   BIGINT / INT64              -> BIGINT
//   VARCHAR / VARYING           -> VARCHAR
//   TEXT                        -> TEXT
//   BOOLEAN / BOOL              -> BOOLEAN
//   DATE / TIME / DATETIME / TIMESTAMP -> 对应类型
//   NULL                        -> NULL_TYPE
// 未知类型返回 DataType::UNKNOWN_TYPE
// ============================================================
inline DataType string_to_data_type(std::string_view str) {
  auto eq = [](std::string_view s, std::string_view literal) {
    return iequals_ascii(s, literal);
  };

  if (eq(str, "tinyint") || eq(str, "int8")) {
    return DataType::TINYINT;
  }
  if (eq(str, "smallint") || eq(str, "int16")) {
    return DataType::SMALLINT;
  }
  if (eq(str, "int") || eq(str, "integer") || eq(str, "int32")) {
    return DataType::INT;
  }
  if (eq(str, "bigint") || eq(str, "int64")) {
    return DataType::BIGINT;
  }
  if (eq(str, "char") || eq(str, "character")) {
    return DataType::CHAR;
  }
  if (eq(str, "varchar") || eq(str, "varying")) {
    return DataType::VARCHAR;
  }
  if (eq(str, "text")) {
    return DataType::TEXT;
  }
  if (eq(str, "boolean") || eq(str, "bool")) {
    return DataType::BOOLEAN;
  }
  if (eq(str, "date")) {
    return DataType::DATE;
  }
  if (eq(str, "time")) {
    return DataType::TIME;
  }
  if (eq(str, "datetime") || eq(str, "timestamp")) {
    return DataType::DATETIME;
  }
  if (eq(str, "null")) {
    return DataType::NULL_TYPE;
  }
  return DataType::UNKNOWN_TYPE;
}

// ============================================================
// 带长度的类型名解析：把 DDL 里的 "VARCHAR(32)" / "CHAR(4)" / "TEXT"
// 解析成 (DataType, 声明长度)。
//
//   "VARCHAR(32)" -> {VARCHAR, 32}
//   "CHAR(4)"     -> {CHAR, 4}
//   "TEXT"        -> {TEXT, 0}
//   "INT"         -> {INT, 0}     （非字符串类型长度必须省略）
//   非法长度/未知类型 -> false
//
// 字符串以外的类型不接受括号参数，出现括号即判非法。
// ============================================================
inline bool parse_type_with_length(std::string_view text, DataType& type,
                                   uint32_t& declared_length) {
  type = DataType::UNKNOWN_TYPE;
  declared_length = 0;

  // 去掉首尾空白
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  const std::string_view name = text.substr(begin, end - begin);
  if (name.empty()) {
    return false;
  }

  const size_t paren = name.find('(');
  if (paren == std::string_view::npos) {
    type = string_to_data_type(name);
    return type != DataType::UNKNOWN_TYPE;
  }
  if (name.back() != ')') {
    return false;
  }

  const std::string_view base = name.substr(0, paren);
  const std::string_view arg = name.substr(paren + 1, name.size() - paren - 2);
  type = string_to_data_type(base);
  if (!is_string(type)) {
    return false;   // 只有字符串类型允许 (n)
  }

  int64_t parsed = 0;
  if (!parse_int64_strict(arg, parsed) || parsed < 0 ||
      parsed > static_cast<int64_t>(kMaxVarcharLength)) {
    return false;
  }
  if (parsed == 0) {
    return false;   // 显式写 (0) 非法
  }
  declared_length = static_cast<uint32_t>(parsed);
  if (!valid_declared_length(type, declared_length)) {
    return false;
  }
  return true;
}

// ============================================================
// C-API 互操作
// ============================================================
inline CDataType to_c(DataType type) {
  switch (type) {
  case DataType::TINYINT:
    return DT_TINYINT;
  case DataType::SMALLINT:
    return DT_SMALLINT;
  case DataType::INT:
    return DT_INT;
  case DataType::BIGINT:
    return DT_BIGINT;
  case DataType::CHAR:
    return DT_CHAR;
  case DataType::VARCHAR:
    return DT_VARCHAR;
  case DataType::TEXT:
    return DT_TEXT;
  case DataType::BOOLEAN:
    return DT_BOOLEAN;
  case DataType::DATE:
    return DT_DATE;
  case DataType::TIME:
    return DT_TIME;
  case DataType::DATETIME:
    return DT_DATETIME;
  case DataType::NULL_TYPE:
    return DT_NULL;
  default:
    return DT_UNKNOWN;
  }
}

inline DataType from_c(::CDataType type) {
  switch (type) {
  case DT_TINYINT:
    return DataType::TINYINT;
  case DT_SMALLINT:
    return DataType::SMALLINT;
  case DT_INT:
    return DataType::INT;
  case DT_BIGINT:
    return DataType::BIGINT;
  case DT_CHAR:
    return DataType::CHAR;
  case DT_VARCHAR:
    return DataType::VARCHAR;
  case DT_TEXT:
    return DataType::TEXT;
  case DT_BOOLEAN:
    return DataType::BOOLEAN;
  case DT_DATE:
    return DataType::DATE;
  case DT_TIME:
    return DataType::TIME;
  case DT_DATETIME:
    return DataType::DATETIME;
  case DT_NULL:
    return DataType::NULL_TYPE;
  default:
    return DataType::UNKNOWN_TYPE;
  }
}

} // namespace sql
