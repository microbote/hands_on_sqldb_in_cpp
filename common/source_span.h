#ifndef COMMON_SOURCE_SPAN_H
#define COMMON_SOURCE_SPAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
   SSpan：源代码里的一段位置（1-based，列区间为左闭右开）

     begin_line   = 起始行（1-based）
     begin_column = 起始列（1-based，按字节计）
     end_line     = 结束行
     end_column   = 结束列（不含该列）

   例：SQL 文本 "SELECT id FROM t;" 里的 "id"
       begin_line = begin? end_line = 1
       begin_column = 8, end_column = 10   -> [8, 10)

   行/列都为 0 表示"未知位置"（sspan_valid() 为 0）。
   位置信息只用于报错/日志，不参与语义。
   ============================================================ */
typedef struct SSpan {
  uint32_t begin_line;
  uint32_t begin_column;
  uint32_t end_line;
  uint32_t end_column;
} SSpan;

static inline SSpan sspan_unknown(void) {
  SSpan span;
  span.begin_line = 0;
  span.begin_column = 0;
  span.end_line = 0;
  span.end_column = 0;
  return span;
}

static inline SSpan sspan_make(uint32_t begin_line, uint32_t begin_column,
                               uint32_t end_line, uint32_t end_column) {
  SSpan span;
  span.begin_line = begin_line;
  span.begin_column = begin_column;
  span.end_line = end_line;
  span.end_column = end_column;
  return span;
}

static inline int sspan_valid(SSpan span) {
  return span.begin_line > 0 && span.begin_column > 0 && span.end_line > 0 &&
         span.end_column > 0;
}

static inline int sspan_equal(SSpan a, SSpan b) {
  return a.begin_line == b.begin_line && a.begin_column == b.begin_column &&
         a.end_line == b.end_line && a.end_column == b.end_column;
}

/* 单行 span 的列宽度；跨行返回 0 */
static inline uint32_t sspan_width(SSpan span) {
  if (!sspan_valid(span) || span.begin_line != span.end_line ||
      span.end_column <= span.begin_column) {
    return 0;
  }
  return span.end_column - span.begin_column;
}

#ifdef __cplusplus
}
#endif

#endif /* COMMON_SOURCE_SPAN_H */
