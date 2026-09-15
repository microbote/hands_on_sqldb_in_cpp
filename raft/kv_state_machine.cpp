#include "raft/kv_state_machine.h"

#include "raft/proposal_payload.h"

namespace raft {
namespace {

constexpr std::string_view kSnapshotMagic = "SQSN";
constexpr uint32_t kSnapshotVersion = 1;

void append_u32(std::string &out, uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xff));
  out.push_back(static_cast<char>((value >> 16) & 0xff));
  out.push_back(static_cast<char>((value >> 8) & 0xff));
  out.push_back(static_cast<char>(value & 0xff));
}

void append_u64(std::string &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

void append_bytes(std::string &out, std::string_view value) {
  append_u64(out, value.size());
  out.append(value.data(), value.size());
}

std::expected<uint64_t, Error> read_u64(std::string_view value,
                                        size_t &offset) {
  if (value.size() < offset + sizeof(uint64_t)) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "truncated raft snapshot"});
  }
  uint64_t result = 0;
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    result = (result << 8) |
             static_cast<uint64_t>(
                 static_cast<unsigned char>(value[offset + i]));
  }
  offset += sizeof(uint64_t);
  return result;
}

std::expected<std::string, Error> read_bytes(std::string_view value,
                                             size_t &offset) {
  auto size = read_u64(value, offset);
  if (!size.has_value()) {
    return std::unexpected(size.error());
  }
  if (*size > value.size() - offset) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "truncated raft snapshot"});
  }
  std::string result{value.substr(offset, *size)};
  offset += *size;
  return result;
}

Error kv_status_to_error(kv::Status status) {
  const auto code = status == kv::Status::InvalidArgument
                        ? ErrorCode::InvalidArgument
                        : (status == kv::Status::IOError ? ErrorCode::IOError
                                                         : ErrorCode::InternalError);
  return Error{
      code,
      std::string{"local kv write failed: "} + kv::status_to_string(status)};
}

} // namespace

KVStateMachine::KVStateMachine(std::shared_ptr<kv::KVStore> local,
                               RequestResultStore &request_results)
    : local_(std::move(local)), request_results_(request_results) {}

std::expected<std::string, Error>
KVStateMachine::apply(const LogEntry &entry) {
  if (entry.data.empty()) {
    return std::string{"noop"};
  }

  auto payload = decode_proposal_payload(entry.data);
  if (!payload.has_value()) {
    return std::unexpected(payload.error());
  }

  auto previous = request_results_.find(payload->client_id,
                                          payload->request_id);
  if (!previous.has_value()) {
    return std::unexpected(previous.error());
  }
  if (previous->has_value()) {
    return **previous;
  }

  const kv::Status status = local_->write_batch(payload->batch);
  if (status != kv::Status::OK) {
    return std::unexpected(kv_status_to_error(status));
  }

  constexpr std::string_view applied = "applied";
  if (auto saved = request_results_.save(
          payload->client_id, payload->request_id, std::string{applied});
      !saved.has_value()) {
    return std::unexpected(saved.error());
  }
  return std::string{applied};
}

std::expected<std::string, Error>
KVStateMachine::snapshot(kv::KeyRange range) {
  if (!range.start.has_value() ||
      (range.end.has_value() && *range.end <= *range.start)) {
    return std::unexpected(Error{
        ErrorCode::InvalidArgument,
        "raft snapshots require a start bound and a non-empty key range"});
  }
  if (range.direction != kv::ScanDirection::kForward) {
    return std::unexpected(Error{
        ErrorCode::InvalidArgument,
        "P0 raft snapshots require a forward key range"});
  }

  auto iterator = local_->new_iterator(range);
  if (iterator == nullptr) {
    return std::unexpected(
        Error{ErrorCode::InternalError, "failed to create raft snapshot iterator"});
  }

  std::string result;
  result.append(kSnapshotMagic);
  append_u32(result, kSnapshotVersion);
  append_u64(result, 0); // placeholder; count is filled after iteration

  uint64_t count = 0;
  for (iterator->seek_to_first(); iterator->valid(); iterator->next()) {
    append_bytes(result, iterator->key());
    append_bytes(result, iterator->value());
    ++count;
  }
  if (iterator->status() != kv::Status::OK &&
      iterator->status() != kv::Status::NotFound) {
    return std::unexpected(Error{
        ErrorCode::InternalError,
        std::string{"raft snapshot scan failed: "} +
            kv::status_to_string(iterator->status())});
  }

  const size_t count_offset = kSnapshotMagic.size() + sizeof(uint32_t);
  for (int shift = 56; shift >= 0; shift -= 8) {
    result[count_offset + static_cast<size_t>((56 - shift) / 8)] =
        static_cast<char>((count >> shift) & 0xff);
  }
  return result;
}

std::expected<void, Error>
KVStateMachine::restore(std::string_view snapshot) {
  (void)snapshot;
  return std::unexpected(Error{
      ErrorCode::InvalidArgument,
      "KVStateMachine requires the group key range for snapshot restore"});
}

std::expected<void, Error>
KVStateMachine::restore(kv::KeyRange range, std::string_view snapshot) {
  constexpr size_t header_size =
      kSnapshotMagic.size() + sizeof(uint32_t) + sizeof(uint64_t);
  if (snapshot.size() < header_size ||
      snapshot.substr(0, kSnapshotMagic.size()) != kSnapshotMagic) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "invalid raft snapshot magic"});
  }

  size_t offset = kSnapshotMagic.size();
  uint32_t version = 0;
  for (size_t i = 0; i < sizeof(uint32_t); ++i) {
    version = (version << 8) |
              static_cast<uint32_t>(
                  static_cast<unsigned char>(snapshot[offset + i]));
  }
  offset += sizeof(uint32_t);
  if (version != kSnapshotVersion) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "unsupported raft snapshot version"});
  }

  auto count = read_u64(snapshot, offset);
  if (!count.has_value()) {
    return std::unexpected(count.error());
  }
  if (*count > snapshot.size()) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "invalid raft snapshot count"});
  }

  if (!range.start.has_value() ||
      (range.end.has_value() && *range.end <= *range.start)) {
    return std::unexpected(Error{
        ErrorCode::InvalidArgument,
        "raft snapshot restore requires a start bound and a non-empty range"});
  }
  if (range.direction != kv::ScanDirection::kForward) {
    return std::unexpected(Error{
        ErrorCode::InvalidArgument,
        "P0 raft snapshot restore requires a forward key range"});
  }

  kv::WriteBatch batch;
  batch.set_sync(true);
  if (range.end.has_value()) {
    batch.remove_range(*range.start, *range.end);
  } else {
    // Whole-range restore without an upper bound: remove_range needs a
    // concrete end, so expand the clear into per-key deletes (the same
    // strategy the LevelDB engine uses for remove_range itself).
    kv::KeyRange scan{range.start, range.end, 0, kv::ScanDirection::kForward};
    auto iterator = local_->new_iterator(scan);
    if (iterator == nullptr) {
      return std::unexpected(Error{
          ErrorCode::InternalError,
          "failed to create raft snapshot clear iterator"});
    }
    for (iterator->seek_to_first(); iterator->valid(); iterator->next()) {
      batch.remove(iterator->key());
    }
    if (iterator->status() != kv::Status::OK &&
        iterator->status() != kv::Status::NotFound) {
      return std::unexpected(Error{
          ErrorCode::InternalError,
          std::string{"raft snapshot clear scan failed: "} +
              kv::status_to_string(iterator->status())});
    }
  }
  for (uint64_t i = 0; i < *count; ++i) {
    auto key = read_bytes(snapshot, offset);
    if (!key.has_value()) {
      return std::unexpected(key.error());
    }
    auto value = read_bytes(snapshot, offset);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    batch.put(*key, *value);
  }
  if (offset != snapshot.size()) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "trailing bytes in raft snapshot"});
  }

  const kv::Status status = local_->write_batch(batch);
  if (status != kv::Status::OK) {
    return std::unexpected(kv_status_to_error(status));
  }
  return {};
}

} // namespace raft
