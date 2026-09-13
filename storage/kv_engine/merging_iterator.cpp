// merging_iterator.cpp
#include "merging_iterator.h"

#include <utility>

namespace kv {

MergingIterator::MergingIterator(std::unique_ptr<Iterator> db,
                                 const TxBuffer *buffer, const KeyRange &range)
    : db_(std::move(db)), buffer_(buffer),
      overlay_(buffer != nullptr
                   ? buffer->overlay_scan(range.start ? &*range.start : nullptr,
                                          range.end ? &*range.end : nullptr,
                                          range.direction)
                   : OverlayCursor(nullptr, nullptr, nullptr, range.direction)),
      direction_(range.direction) {
  settle();
}

bool MergingIterator::overlay_first() const {
  if (!overlay_.valid()) {
    return false;
  }
  if (db_ == nullptr || !db_->valid()) {
    return true;
  }
  return direction_ == ScanDirection::kForward ? overlay_.key() < db_->key()
                                               : overlay_.key() > db_->key();
}

void MergingIterator::settle() {
  while (true) {
    const bool db_ok = db_ != nullptr && db_->valid();
    const bool ov_ok = overlay_.valid();
    if (!db_ok && !ov_ok) {
      valid_ = false;
      return;
    }

    // 覆盖层这边更靠前：只有它能提供这个 key
    if (overlay_first()) {
      const OverlayOp op = overlay_.op();
      if (op.is_tombstone()) {
        overlay_.next(); // 纯墓碑（DB 里没有）：跳过
        continue;
      }
      current_key_ = overlay_.key();
      current_value_ = op.value;
      break;
    }

    if (!db_ok) {
      valid_ = false;
      return;
    }

    const bool same_key = ov_ok && (direction_ == ScanDirection::kForward
                                        ? !(db_->key() < overlay_.key())
                                        : !(db_->key() > overlay_.key()));
    if (same_key) {
      const OverlayOp op = overlay_.op();
      if (op.is_tombstone()) {
        // 本事务删掉的：DB 与覆盖层一起吃掉
        db_->next();
        overlay_.next();
        continue;
      }
      current_key_ = db_->key();
      current_value_ = op.value; // 新值覆盖旧值
      break;
    }

    // 只有 DB 有：透传
    current_key_ = db_->key();
    current_value_ = db_->value();
    break;
  }

  valid_ = true;
  // 反推"当前项来自哪个源"：后面 next() 要推进对应的源
  from_db_ = db_ != nullptr && db_->valid() && db_->key() == current_key_;
  from_overlay_ = overlay_.valid() && overlay_.key() == current_key_;
}

void MergingIterator::advance_sources() {
  if (from_db_ && db_ != nullptr) {
    db_->next();
  }
  if (from_overlay_) {
    overlay_.next();
  }
}

// 两个源各自退到"扫描方向上严格在 key 之前"，跳过墓碑后取更靠后的那个
bool MergingIterator::predecessor_of(const Key &key, Key *out_key,
                                     ByteValue *out_value) {
  bool has_db = false;
  Key db_key;
  ByteValue db_value;
  if (db_ != nullptr) {
    db_->seek(key);
    if (!db_->valid()) {
      db_->seek_to_last(); // key 在区间之外：之前那一项就是区间的最后一项
    } else {
      db_->prev(); // 朝扫描起点的方向退一格
    }
    while (db_->valid() && buffer_ != nullptr &&
           buffer_->lookup(db_->key()).is_tombstone()) {
      db_->prev();
    }
    if (db_->valid()) {
      has_db = true;
      db_key = db_->key();
      db_value = db_->value();
    }
  }

  bool has_ov = false;
  Key ov_key;
  ByteValue ov_value;
  overlay_.seek(key);
  if (!overlay_.valid()) {
    overlay_.seek_finish();
  } else {
    overlay_.prev();
  }
  while (overlay_.valid() && overlay_.op().is_tombstone()) {
    overlay_.prev();
  }
  if (overlay_.valid()) {
    has_ov = true;
    ov_key = overlay_.key();
    ov_value = overlay_.op().value;
  }

  if (!has_db && !has_ov) {
    return false;
  }
  const bool take_overlay =
      has_ov && (!has_db || later_in_scan(direction_, ov_key, db_key));
  *out_key = take_overlay ? ov_key : db_key;
  *out_value = take_overlay ? ov_value : db_value;
  return true;
}

// 扫描方向的最后一项：两个源各自到最后，跳过墓碑后取更靠后的那个
bool MergingIterator::last_item(Key *out_key, ByteValue *out_value) {
  bool has_db = false;
  Key db_key;
  ByteValue db_value;
  if (db_ != nullptr) {
    db_->seek_to_last();
    while (db_->valid() && buffer_ != nullptr &&
           buffer_->lookup(db_->key()).is_tombstone()) {
      db_->prev();
    }
    if (db_->valid()) {
      has_db = true;
      db_key = db_->key();
      db_value = db_->value();
    }
  }

  bool has_ov = false;
  Key ov_key;
  ByteValue ov_value;
  overlay_.seek_finish();
  while (overlay_.valid() && overlay_.op().is_tombstone()) {
    overlay_.prev();
  }
  if (overlay_.valid()) {
    has_ov = true;
    ov_key = overlay_.key();
    ov_value = overlay_.op().value;
  }

  if (!has_db && !has_ov) {
    return false;
  }
  const bool take_overlay =
      has_ov && (!has_db || later_in_scan(direction_, ov_key, db_key));
  *out_key = take_overlay ? ov_key : db_key;
  *out_value = take_overlay ? ov_value : db_value;
  return true;
}

void MergingIterator::reposition_to(const Key &key) {
  if (db_ != nullptr) {
    db_->seek(key);
  }
  overlay_.seek(key);
  settle();
}

void MergingIterator::next() {
  if (!valid_) {
    return;
  }
  advance_sources();
  settle();
}

void MergingIterator::prev() {
  if (!valid_) {
    return;
  }
  Key key;
  ByteValue value;
  if (!predecessor_of(current_key_, &key, &value)) {
    valid_ = false;
    return;
  }
  reposition_to(key);
}

void MergingIterator::seek(const Key &key) {
  if (db_ != nullptr) {
    db_->seek(key);
  }
  overlay_.seek(key);
  settle();
}

void MergingIterator::seek_to_first() {
  if (db_ != nullptr) {
    db_->seek_to_first();
  }
  overlay_.seek_start();
  settle();
}

void MergingIterator::seek_to_last() {
  Key key;
  ByteValue value;
  if (!last_item(&key, &value)) {
    valid_ = false;
    return;
  }
  reposition_to(key);
}

KVPair MergingIterator::kvpair() const {
  KVPair pair;
  if (valid_) {
    pair.key = current_key_;
    pair.value = current_value_;
  }
  return pair;
}

Status MergingIterator::status() const {
  return db_ != nullptr ? db_->status() : Status::InternalError;
}

std::string MergingIterator::error_message() const {
  return db_ != nullptr ? db_->error_message() : "no db iterator";
}

} // namespace kv
