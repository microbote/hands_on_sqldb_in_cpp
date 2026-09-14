#include "raft/leveldb_request_result_store.h"

#if defined(SQLDB_HAVE_LEVELDB)

namespace raft {
namespace {

constexpr std::string_view kPrefix = "client/";
constexpr size_t kEncodedIdSize = sizeof(uint64_t);

void append_u64(std::string &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    const auto byte = static_cast<unsigned char>((value >> shift) & 0xff);
    out.push_back(static_cast<char>(byte));
  }
}

std::string client_key(uint64_t client_id) {
  std::string key{kPrefix};
  append_u64(key, client_id);
  return key;
}

std::expected<uint64_t, Error> decode_u64(std::string_view value,
                                          size_t &offset) {
  if (value.size() < offset + kEncodedIdSize) {
    return std::unexpected(Error{
        ErrorCode::InternalError, "truncated request result record"});
  }
  uint64_t result = 0;
  for (size_t i = 0; i < kEncodedIdSize; ++i) {
    result = (result << 8) |
             static_cast<uint64_t>(
                 static_cast<unsigned char>(value[offset + i]));
  }
  offset += kEncodedIdSize;
  return result;
}

Error leveldb_error(const leveldb::Status &status) {
  return Error{ErrorCode::IOError, status.ToString()};
}

std::expected<void, Error> check_db(const leveldb::DB *db) {
  if (db == nullptr) {
    return std::unexpected(
        Error{ErrorCode::InternalError, "request result store is not open"});
  }
  return {};
}

} // namespace

LevelDBRequestResultStore::~LevelDBRequestResultStore() {
  (void)close();
}

std::expected<void, Error>
LevelDBRequestResultStore::open(const std::string &path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (db_ != nullptr) {
    return std::unexpected(Error{
        ErrorCode::InvalidArgument,
        "request result store is already open"});
  }

  leveldb::Options options;
  options.create_if_missing = true;
  options.paranoid_checks = true;
  leveldb::DB *db = nullptr;
  const leveldb::Status status = leveldb::DB::Open(options, path, &db);
  if (!status.ok()) {
    delete db;
    return std::unexpected(leveldb_error(status));
  }
  db_.reset(db);
  return {};
}

std::expected<void, Error> LevelDBRequestResultStore::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  db_.reset();
  return {};
}

std::expected<std::optional<std::string>, Error>
LevelDBRequestResultStore::find(uint64_t client_id,
                                uint64_t request_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto check = check_db(db_.get()); !check.has_value()) {
    return std::unexpected(check.error());
  }

  std::string value;
  const leveldb::Status status = db_->Get(
      leveldb::ReadOptions(), client_key(client_id), &value);
  if (status.IsNotFound()) {
    return std::optional<std::string>{};
  }
  if (!status.ok()) {
    return std::unexpected(leveldb_error(status));
  }
  if (value.size() < kEncodedIdSize) {
    return std::unexpected(Error{
        ErrorCode::InternalError, "truncated request result record"});
  }

  size_t offset = 0;
  auto saved_request_id = decode_u64(value, offset);
  if (!saved_request_id.has_value()) {
    return std::unexpected(saved_request_id.error());
  }
  if (*saved_request_id == request_id) {
    return value.substr(offset);
  }
  if (*saved_request_id > request_id) {
    return std::unexpected(Error{
        ErrorCode::InvalidArgument,
        "request id is older than the client's latest applied request"});
  }
  return std::optional<std::string>{};
}

std::expected<void, Error>
LevelDBRequestResultStore::save(uint64_t client_id, uint64_t request_id,
                                std::string result) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto check = check_db(db_.get()); !check.has_value()) {
    return std::unexpected(check.error());
  }

  std::string value;
  append_u64(value, request_id);
  value.append(result);

  leveldb::WriteOptions options;
  options.sync = true;
  const leveldb::Status status =
      db_->Put(options, client_key(client_id), value);
  if (!status.ok()) {
    return std::unexpected(leveldb_error(status));
  }
  return {};
}

} // namespace raft

#endif
