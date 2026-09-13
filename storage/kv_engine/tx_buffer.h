// tx_buffer.h
//
// TxBuffer：一个写事务的"变更缓冲"（悲观单写者）。
//
//   - 事务期间**不改 DB**：所有 put/remove 先攒在这里；
//   - 提交 = 攒下的 op 按原顺序合成一个 WriteBatch，一次写入（原子 + 持久）；
//   - 回滚 = 清空缓冲 —— DB 从没被动过，"与原状态一致"是**构造性成立**的。
//
// 两套表示，各管一件事：
//   - view_ ：按 key 的**覆盖视图**（同 key 多次写已合并），负责"读得到什么"；
//   - log_  ：**有序 op 列表**，负责"提交时按什么顺序写"。
//   两者不能合并：remove_range 与 put 的交错顺序会改变最终结果。
//
// 读的回答是**三态**（不是 optional<optional>）：
//   kNone      —— 缓冲里没它，这个 key 由下层（DB）回答
//   kValue     —— 本事务写了新值
//   kTombstone —— 本事务删了它（含被 remove_range 覆盖）
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "kv_engine.h"

namespace kv {

// ============================================================
// 覆盖层对某个 key 的回答（三态）
// ============================================================
struct OverlayOp {
  enum class Kind : uint8_t {
    kNone,      // 缓冲没意见 -> 去查 DB
    kValue,     // 本事务写了新值
    kTombstone, // 本事务删了它
  };

  Kind kind = Kind::kNone;
  ByteValue value; // 仅当 kind == kValue 有效

  bool covered() const { return kind != Kind::kNone; }
  bool has_value() const { return kind == Kind::kValue; }
  bool is_tombstone() const { return kind == Kind::kTombstone; }
};

// 覆盖视图里的一项（内部类型，OverlayCursor 遍历的就是它）
struct TxEntry {
  OverlayOp op;
  size_t seq = 0; // 序号：同一 key 多次操作时，序号大的生效
};

// 范围删除（DROP / TRUNCATE）：不展开成逐键，靠序号参与"谁生效"的判定
struct TxRangeDelete {
  Key begin;
  Key end;
  size_t seq = 0;
};

class TxBuffer;

// ============================================================
// OverlayCursor：覆盖层的**有序游标**（合并迭代器用）
//
// 只遍历"本事务明确动过的 key"（put/remove）；范围删除的影响由 lookup()
// 体现 —— op() 返回**有效**操作（kValue / kTombstone，不会是 kNone）。
// ============================================================
class OverlayCursor {
public:
  // buffer == nullptr 表示"没有事务"：游标恒为无效（空覆盖层）
  OverlayCursor(const TxBuffer *buffer, const Key *begin, const Key *end,
                ScanDirection direction);

  bool valid() const;
  void next(); // 按扫描方向前进（与 Iterator::next 一致）
  void prev(); // 与 next() 相反

  const Key &key() const;
  OverlayOp op() const;

  void seek(const Key &key);
  void seek_start();  // 扫描方向的起点
  void seek_finish(); // 扫描方向的终点（对应 Iterator::seek_to_last）

private:
  using MapIterator = std::map<Key, TxEntry>::const_iterator;
  bool in_bounds() const;

  const TxBuffer *buffer_ = nullptr;
  // 边界必须**复制**：调用方的 KeyRange 往往是临时/局部对象（比如
  // Table::scan 里的 kv_range），存指针会悬垂。
  std::optional<Key> begin_; // nullopt = -∞
  std::optional<Key> end_;   // nullopt = +∞（开区间）
  ScanDirection direction_ = ScanDirection::kForward;
  MapIterator it_;
  MapIterator map_end_;
};

// ============================================================
// TxBuffer
// ============================================================
class TxBuffer {
public:
  friend class OverlayCursor;

  TxBuffer() = default;

  // ---- 写（进缓冲，不动 DB） ----
  void put(const Key &key, const ByteValue &value);
  void remove(const Key &key);
  void remove_range(const Key &begin, const Key &end);

  // ---- 读（覆盖视图） ----
  OverlayOp lookup(const Key &key) const;
  // 有序游标：合并迭代器用它把"本事务的改动"按 key 顺序并进去
  OverlayCursor overlay_scan(const Key *begin, const Key *end,
                             ScanDirection direction) const {
    return OverlayCursor(this, begin, end, direction);
  }

  // ---- 提交 / 回滚 ----
  // 按 op 顺序合成 WriteBatch（提交方负责真正写入）
  WriteBatch to_batch() const;
  void clear();

  bool empty() const { return log_.empty(); }
  size_t op_count() const { return log_.size(); }
  size_t bytes() const { return bytes_; }

  static constexpr size_t kDefaultLimit = 64u * 1024u * 1024u; // 64MB
  bool exceeds(size_t limit = kDefaultLimit) const { return bytes_ > limit; }

private:
  void note_bytes(size_t extra) { bytes_ += extra; }

  std::map<Key, TxEntry> view_;
  std::vector<TxRangeDelete> range_deletes_;
  std::vector<WriteBatch::Op> log_;
  size_t bytes_ = 0;
  size_t next_seq_ = 1;
};

} // namespace kv
