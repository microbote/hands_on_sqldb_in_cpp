#include "raft_bootstrap.h"

#if defined(SQLDB_HAVE_LEVELDB)

#include <filesystem>
#include <charconv>
#include <map>
#include <system_error>
#include <utility>

#include "raft/peers.h"

namespace server {
namespace {

std::string to_string(const raft::Error &error) {
  return std::string{raft::error_code_to_string(error.code)} + ": " +
         error.message;
}

std::expected<uint16_t, std::string> parse_port(const std::string &text) {
  uint32_t port = 0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const auto rc = std::from_chars(begin, end, port, 10);
  if (text.empty() || rc.ec != std::errc() || rc.ptr != end || port == 0 ||
      port > 65535) {
    return std::unexpected("raft.listen: invalid port '" + text + "'");
  }
  return static_cast<uint16_t>(port);
}

} // namespace

RaftBootstrap::~RaftBootstrap() { stop(); }

std::expected<std::unique_ptr<RaftBootstrap>, std::string>
RaftBootstrap::open(const ServerConfig &config,
                    std::shared_ptr<kv::KVStore> local, Logger &logger) {
  return open(config, std::move(local), logger, Options{});
}

std::expected<std::unique_ptr<RaftBootstrap>, std::string>
RaftBootstrap::open(const ServerConfig &config,
                    std::shared_ptr<kv::KVStore> local, Logger &logger,
                    Options options) {
  const std::vector<raft::PeerConfig> peers = config.raft_peers();
  if (peers.empty()) {
    return std::unexpected("raft.peers is empty");
  }
  const uint64_t node_id = config.raft_node_id();
  bool self_found = false;
  std::vector<raft::NodeId> peer_ids;
  peer_ids.reserve(peers.size());
  for (const raft::PeerConfig &peer : peers) {
    peer_ids.push_back(peer.node_id);
    if (peer.node_id.value == node_id) {
      self_found = true;
    }
  }
  if (!self_found) {
    return std::unexpected("raft.node_id " + std::to_string(node_id) +
                           " does not appear in raft.peers");
  }
  if (local == nullptr || !local->is_open()) {
    return std::unexpected(
        "raft requires an already-open local KVStore as its state machine");
  }

  const raft::GroupRouter router = config.raft_group_router();
  const size_t group_count = router.group_count();
  if (group_count > 1 && config.raft_snapshot_entries() > 0) {
    // Multi-group snapshots need per-group ranges (M3); until then, compaction
    // in a multi-group deployment would use the wrong key range.
    return std::unexpected(
        "raft.snapshot_entries > 0 is not supported with multi-group shards "
        "(per-group snapshots are M3)");
  }

  auto self = std::unique_ptr<RaftBootstrap>(new RaftBootstrap());
  self->logger_ = &logger;
  self->local_ = std::move(local);
  self->heartbeat_ms_ = static_cast<int64_t>(config.raft_heartbeat_ms());

  const std::string base = config.raft_log_path();
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  if (ec) {
    return std::unexpected("cannot create raft log directory " + base + ": " +
                           ec.message());
  }

  raft::RaftTcpTransport::Options transport_options;
  transport_options.node_id = raft::NodeId{node_id};
  transport_options.listen_host = config.raft_listen_host();
  if (auto port = parse_port(config.raft_listen_port()); port.has_value()) {
    transport_options.listen_port = *port;
  } else if (options.bind_listener && peers.size() > 1) {
    return std::unexpected(port.error());
  }
  transport_options.peers = peers;
  transport_options.logger = [&logger](const char *level,
                                       const std::string &message) {
    logger.log(level, message);
  };
  // One shared transport for every group: inbound frames carry a group id and
  // post_to_runtime() routes them to the right group's service thread.
  self->transport_ = std::make_unique<raft::RaftTcpTransport>(
      std::move(transport_options),
      [raw = self.get()](uint64_t group_id, std::function<void()> work) {
        return raw->post_to_runtime(group_id, std::move(work));
      });

  std::map<uint64_t, raft::RaftExecutor *> executors;
  for (size_t g = 0; g < group_count; ++g) {
    auto group = std::make_unique<RaftGroupState>();

    // Per-group durable stores. Single-group keeps the historic layout
    // (<log_path>/log, <log_path>/request_results); multi-group nests under
    // <log_path>/group<N>/.
    const std::string group_dir =
        group_count == 1 ? base : base + "/group" + std::to_string(g);
    std::filesystem::create_directories(group_dir, ec);
    if (ec) {
      return std::unexpected("cannot create raft group directory " + group_dir +
                             ": " + ec.message());
    }
    if (auto ok = group->log_store.open(group_dir + "/log"); !ok.has_value()) {
      return std::unexpected("cannot open raft log store for group " +
                             std::to_string(g) + ": " + to_string(ok.error()));
    }
    if (auto ok = group->request_results.open(group_dir + "/request_results");
        !ok.has_value()) {
      return std::unexpected("cannot open raft request result store for group " +
                             std::to_string(g) + ": " +
                             to_string(ok.error()));
    }
    group->state_machine = std::make_unique<raft::KVStateMachine>(
        self->local_, group->request_results);

    raft::NodeConfig node_config{
        raft::NodeId{node_id}, peer_ids, config.raft_election_timeout_ms(),
        config.raft_heartbeat_ms()};
    // Group 0 owns @system/* plus every default key: no single contiguous
    // range (used only for snapshots, which are disabled in multi-group M1).
    // Data groups use their router range.
    node_config.group_range =
        g == 0 ? kv::KeyRange::from(kv::Key{})
               : router.group_range(g).value_or(kv::KeyRange::from(kv::Key{}));
    node_config.snapshot_entries_threshold = config.raft_snapshot_entries();
    node_config.group_id = g;
    group->node = std::make_unique<raft::RaftNode>(
        std::move(node_config), group->log_store, *self->transport_,
        *group->state_machine, self->clock_);
    group->runtime = std::make_unique<raft::RaftRuntime>(
        *group->node, "raft-g" + std::to_string(g),
        config.raft_proposal_timeout_ms(), config.raft_read_timeout_ms());
    group->runtime->start();

    // start() installs the transport callback; it must run on the group's
    // service thread like every other RaftNode call.
    std::expected<void, raft::Error> node_started;
    auto ran = group->runtime->run([&] { node_started = group->node->start(); });
    if (!ran.has_value()) {
      return std::unexpected("raft service thread for group " +
                             std::to_string(g) + " did not start: " +
                             ran.error().message);
    }
    if (!node_started.has_value()) {
      return std::unexpected("RaftNode::start failed for group " +
                             std::to_string(g) + ": " +
                             to_string(node_started.error()));
    }
    executors[g] = group->runtime.get();
    self->groups_.push_back(std::move(group));
  }

  if (options.bind_listener && peers.size() > 1) {
    if (auto ok = self->transport_->listen(); !ok.has_value()) {
      return std::unexpected(ok.error());
    }
    self->inbound_thread_ = std::thread(
        [raw = self.get()] { raw->transport_->run_inbound(); });
    logger.log("info", "raft listening on " + config.raft_listen() +
                           " node_id=" + std::to_string(node_id) +
                           " groups=" + std::to_string(group_count) +
                           " peers=" + raft::format_peer_list(peers));
  } else {
    logger.log("info", "raft node_id=" + std::to_string(node_id) +
                           " groups=" + std::to_string(group_count) +
                           " (no listener) peers=" +
                           raft::format_peer_list(peers));
  }

  self->timer_thread_ = std::thread([raw = self.get()] {
    raw->arm_timer();
    raw->timer_loop_.run();
  });

  // Client-facing endpoints for redirects: node id -> SQL address. Missing
  // entries mean "we know who the leader is but not where its SQL port is", so
  // the client gets a plain NotLeader without a reconnect target.
  self->store_ = std::make_shared<raft::RaftKVStore>(
      self->local_, std::move(executors), router,
      config.raft_sql_endpoints());
  const kv::Status status = self->store_->open(kv::DatabaseOptions{});
  if (status != kv::Status::OK) {
    return std::unexpected(std::string{"cannot open RaftKVStore: "} +
                           kv::status_to_string(status));
  }
  return std::unique_ptr<RaftBootstrap>(std::move(self));
}

bool RaftBootstrap::post_to_runtime(uint64_t group_id,
                                    std::function<void()> work) {
  if (group_id >= groups_.size() || groups_[group_id]->runtime == nullptr) {
    // Only reachable before the runtimes exist (the inbound loop cannot be
    // running yet) or for an unregistered group: drop and count via transport.
    return false;
  }
  return groups_[group_id]->runtime->post(std::move(work));
}

void RaftBootstrap::arm_timer() {
  timer_loop_.add_timer(heartbeat_ms_, [this] { on_tick(); });
}

void RaftBootstrap::on_tick() {
  for (auto &group : groups_) {
    if (group->runtime != nullptr) {
      group->runtime->request_tick();
    }
  }
  if (!timer_loop_.stopped()) {
    arm_timer();
  }
}

void RaftBootstrap::stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;

  timer_loop_.stop();
  if (timer_thread_.joinable()) {
    timer_thread_.join();
  }
  if (transport_ != nullptr) {
    transport_->stop();
  }
  if (inbound_thread_.joinable()) {
    inbound_thread_.join();
  }
  // Reverse startup order: runtimes first, then stores.
  for (auto it = groups_.rbegin(); it != groups_.rend(); ++it) {
    auto &group = *it;
    if (group->runtime != nullptr) {
      group->runtime->stop();
    }
    if (auto ok = group->request_results.close(); !ok.has_value()) {
      if (logger_ != nullptr) {
        logger_->log("warn", "closing raft request result store failed: " +
                                 to_string(ok.error()));
      }
    }
    if (auto ok = group->log_store.close(); !ok.has_value()) {
      if (logger_ != nullptr) {
        logger_->log("warn", "closing raft log store failed: " +
                                 to_string(ok.error()));
      }
    }
  }
}

} // namespace server

#endif // SQLDB_HAVE_LEVELDB
