#include "raft_bootstrap.h"

#if defined(SQLDB_HAVE_LEVELDB)

#include <filesystem>
#include <charconv>
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

  auto self = std::unique_ptr<RaftBootstrap>(new RaftBootstrap());
  self->logger_ = &logger;
  self->local_ = std::move(local);
  self->heartbeat_ms_ = static_cast<int64_t>(config.raft_heartbeat_ms());

  // Two separate LevelDBs under <log_path>: the Raft log/hard state and the
  // idempotency results. Neither is the business KV directory.
  const std::string base = config.raft_log_path();
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  if (ec) {
    return std::unexpected("cannot create raft log directory " + base + ": " +
                           ec.message());
  }
  if (auto ok = self->log_store_.open(base + "/log"); !ok.has_value()) {
    return std::unexpected("cannot open raft log store: " +
                           to_string(ok.error()));
  }
  if (auto ok = self->request_results_.open(base + "/request_results");
      !ok.has_value()) {
    return std::unexpected("cannot open raft request result store: " +
                           to_string(ok.error()));
  }

  self->state_machine_ =
      std::make_unique<raft::KVStateMachine>(self->local_,
                                             self->request_results_);

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
  // The transport must never touch RaftNode; it hands messages to the runtime.
  // `runtime_` is created right below, before the inbound loop can start, so
  // this indirection only exists to break the construction cycle.
  self->transport_ = std::make_unique<raft::RaftTcpTransport>(
      std::move(transport_options),
      [raw = self.get()](std::function<void()> work) {
        return raw->post_to_runtime(std::move(work));
      });

  raft::NodeConfig node_config{raft::NodeId{node_id}, std::move(peer_ids),
                               config.raft_election_timeout_ms(),
                               config.raft_heartbeat_ms()};
  // P1 单 group 覆盖整个 key space（从 "" 扫到结尾）。P2 多 group 时每个
  // group 在这里拿到自己的 range。
  node_config.group_range = kv::KeyRange::from(kv::Key{});
  node_config.snapshot_entries_threshold = config.raft_snapshot_entries();
  self->node_ = std::make_unique<raft::RaftNode>(
      std::move(node_config), self->log_store_, *self->transport_,
      *self->state_machine_, self->clock_);
  self->runtime_ = std::make_unique<raft::RaftRuntime>(*self->node_, "raft");
  self->runtime_->start();

  // start() installs the transport callback; it must run on the raft service
  // thread like every other RaftNode call. The inbound loop starts afterwards,
  // so no message can arrive before the callback exists.
  std::expected<void, raft::Error> node_started;
  auto ran = self->runtime_->run([&] { node_started = self->node_->start(); });
  if (!ran.has_value()) {
    return std::unexpected("raft service thread did not start: " +
                           ran.error().message);
  }
  if (!node_started.has_value()) {
    return std::unexpected("RaftNode::start failed: " +
                           to_string(node_started.error()));
  }

  if (options.bind_listener && peers.size() > 1) {
    if (auto ok = self->transport_->listen(); !ok.has_value()) {
      return std::unexpected(ok.error());
    }
    self->inbound_thread_ = std::thread(
        [raw = self.get()] { raw->transport_->run_inbound(); });
    logger.log("info", "raft listening on " + config.raft_listen() +
                           " node_id=" + std::to_string(node_id) +
                           " peers=" + raft::format_peer_list(peers));
  } else {
    logger.log("info", "raft node_id=" + std::to_string(node_id) +
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
      self->local_, *self->runtime_, config.raft_sql_endpoints());
  const kv::Status status = self->store_->open(kv::DatabaseOptions{});
  if (status != kv::Status::OK) {
    return std::unexpected(std::string{"cannot open RaftKVStore: "} +
                           kv::status_to_string(status));
  }
  return std::unique_ptr<RaftBootstrap>(std::move(self));
}

bool RaftBootstrap::post_to_runtime(std::function<void()> work) {
  if (runtime_ == nullptr) {
    // Only reachable before the runtime exists: the inbound loop cannot be
    // running yet, so dropping here keeps the early path safe.
    return false;
  }
  return runtime_->post(std::move(work));
}

void RaftBootstrap::arm_timer() {
  timer_loop_.add_timer(heartbeat_ms_, [this] { on_tick(); });
}

void RaftBootstrap::on_tick() {
  if (runtime_ != nullptr) {
    runtime_->request_tick();
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
  if (runtime_ != nullptr) {
    runtime_->stop();
  }
  if (auto ok = request_results_.close(); !ok.has_value()) {
    if (logger_ != nullptr) {
      logger_->log("warn", "closing raft request result store failed: " +
                               to_string(ok.error()));
    }
  }
  if (auto ok = log_store_.close(); !ok.has_value()) {
    if (logger_ != nullptr) {
      logger_->log("warn",
                   "closing raft log store failed: " + to_string(ok.error()));
    }
  }
}

} // namespace server

#endif // SQLDB_HAVE_LEVELDB
