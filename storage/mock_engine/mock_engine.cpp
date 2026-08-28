#include "mock_engine.h"

// mock_engine.cpp
#include <iostream>
#include <sstream>

#include "mock_engine.h"

namespace kv {

// ============================================================
// MockIterator 实现
// ============================================================
MockIterator::MockIterator(const std::map<Key, ByteValue>* data,
                           const KeyRange& range)
    : data_(data), range_(range), pos_(data_->end()), status_(Status::OK) {
  if (!data_) {
    status_ = Status::InternalError;
    error_msg_ = "Data pointer is null";
    return;
  }

  // 定位到起始位置
  seek_to_first();
}

void MockIterator::seek(const Key& key) {
  if (!data_) {
    status_ = Status::InternalError;
    error_msg_ = "Data pointer is null";
    return;
  }

  if (range_.direction == ScanDirection::kForward) {
    pos_ = data_->lower_bound(key); //first of >= key
  } else {
    // 反向扫描：找 <= key 的最后一个
    auto it = data_->upper_bound(key); //first of > key
    if (it == data_->begin()) {
      pos_ = data_->end();
    } else {
      --it;
      pos_ = it;
    }
  }

  update_status();
}

void MockIterator::seek_to_first() {
  if (!data_) {
    status_ = Status::InternalError;
    error_msg_ = "Data pointer is null";
    return;
  }

  if (range_.direction == ScanDirection::kForward) {
    pos_ = data_->begin();
  } else {
    if (data_->empty()) {
      pos_ = data_->end();
    } else {
      auto it = data_->end();
      --it;
      pos_ = it;
    }
  }

  update_status();
}

void MockIterator::seek_to_last() {
  if (!data_) {
    status_ = Status::InternalError;
    error_msg_ = "Data pointer is null";
    return;
  }

  if (range_.direction == ScanDirection::kForward) {
    if (data_->empty()) {
      pos_ = data_->end();
    } else {
      auto it = data_->end();
      --it;
      pos_ = it;
    }
  } else {
    pos_ = data_->begin();
  }

  update_status();
}

void MockIterator::next() {
  if (!valid()) return;

  if (range_.direction == ScanDirection::kForward) {
    ++pos_;
  } else {
    if (pos_ == data_->begin()) {
      pos_ = data_->end();
    } else {
      --pos_;
    }
  }

  update_status();
}

void MockIterator::prev() {
  if (!valid()) return;

  if (range_.direction == ScanDirection::kForward) {
    if (pos_ == data_->begin()) {
      pos_ = data_->end();
    } else {
      --pos_;
    }
  } else {
    ++pos_;
  }

  update_status();
}

bool MockIterator::valid() const {
  if (!data_) return false;
  if (status_ != Status::OK) return false;
  if (pos_ == data_->end()) return false;

  // 检查范围
  const Key& key = pos_->first;
  if (range_.start && key < *range_.start) return false;
  if (range_.end && key >= *range_.end) return false;

  return true;
}

Key MockIterator::key() const {
  if (!valid()) return "";
  return pos_->first;
}

ByteValue MockIterator::value() const {
  if (!valid()) return "";
  return pos_->second;
}

KVPair MockIterator::kvpair() const {
  if (!valid()) return {"", std::nullopt};
  return {pos_->first, pos_->second};
}

Status MockIterator::status() const { return status_; }

std::string MockIterator::error_message() const { return error_msg_; }

void MockIterator::update_status() {
  if (!data_) {
    status_ = Status::InternalError;
    error_msg_ = "Data pointer is null";
    return;
  }

  if (pos_ == data_->end()) {
    status_ = Status::NotFound;
    error_msg_ = "No more data";
    return;
  }

  if (in_range()) {
    status_ = Status::OK;
    error_msg_ = "";
  } else {
    status_ = Status::NotFound;
    error_msg_ = "Out of range";
  }
}

bool MockIterator::in_range() const {
  if (pos_ == data_->end()) return false;

  const Key& key = pos_->first;
  if (range_.start && key < *range_.start) return false;
  if (range_.end && key >= *range_.end) return false;

  return true;
}

// ============================================================
// MockEngine 实现
// ============================================================

// ----- 生命周期 -----
Status MockEngine::open_database(DatabaseOptions options) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (is_open_) {
    return Status::AlreadyExists;
  }

  options_ = options;
  is_open_ = true;

  return Status::OK;
}

Status MockEngine::close_database() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!is_open_) {
    return Status::NotFound;
  }

  is_open_ = false;
  // 可以选择清空数据或保留
  // data_.clear();

  return Status::OK;
}

bool MockEngine::is_open() const { return is_open_.load(); }

// ----- 单条操作 -----
Status MockEngine::get(const Key& key, ByteValue* value) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  auto it = data_.find(key);
  if (it == data_.end()) {
    return Status::NotFound;
  }

  if (value) {
    *value = it->second;
  }

  return Status::OK;
}

Status MockEngine::put(const Key& key, const ByteValue& value) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  data_[key] = value;
  return Status::OK;
}

Status MockEngine::remove(const Key& key) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  auto it = data_.find(key);
  if (it == data_.end()) {
    return Status::NotFound;
  }

  data_.erase(it);
  return Status::OK;
}

bool MockEngine::exists(const Key& key) {
  if (!is_open_) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  return data_.find(key) != data_.end();
}

// ----- 批量操作 -----
Status MockEngine::get_batch(const std::vector<Key>& keys,
                             MissingKeyPolicy policy,
                             std::vector<std::optional<ByteValue>>* values) {
  if (!is_open_) {
    return Status::InternalError;
  }

  if (!values) {
    return Status::InvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  values->clear();
  values->reserve(keys.size());

  for (const auto& key : keys) {
    auto it = data_.find(key);
    if (it == data_.end()) {
      if (policy == MissingKeyPolicy::kReturnError) {
        return Status::NotFound;
      }
      values->push_back(std::nullopt);
    } else {
      values->push_back(it->second);
    }
  }

  return Status::OK;
}

Status MockEngine::write_batch(const WriteBatch& batch) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto& op : batch.ops()) {
    if (op.type == WriteBatch::OpType::kPut) {
      if (op.data.value.has_value()) {
        data_[op.data.key] = op.data.value.value();
      } else {
        // 如果 value 为空，视为删除
        //data_.erase(op.data.key);
        fprintf(stderr, "key:[%s]'s value is empty\n", op.data.key.c_str());
      }
    } else {  // kRemove
      data_.erase(op.data.key);
    }
  }

  return Status::OK;
}

// ----- 迭代器 -----
std::unique_ptr<Iterator> MockEngine::new_iterator(const KeyRange& range) {
  if (!is_open_) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  return std::make_unique<MockIterator>(&data_, range);
}

// ----- 管理 -----
void MockEngine::flush() {
  // Mock 引擎不需要 flush
}

std::string MockEngine::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::ostringstream oss;
  oss << "MockEngine Stats:\n";
  oss << "  is_open: " << (is_open_ ? "true" : "false") << "\n";
  oss << "  entries: " << data_.size() << "\n";
  oss << "  path: " << options_.path << "\n";
  return oss.str();
}

// ----- 测试辅助 -----
void MockEngine::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  data_.clear();
}

size_t MockEngine::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return data_.size();
}

void MockEngine::dump() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::cout << "MockEngine dump (" << data_.size() << " entries):\n";
  for (const auto& [key, value] : data_) {
    std::cout << "  " << key << " -> " << value << "\n";
  }
}

}  // namespace kv