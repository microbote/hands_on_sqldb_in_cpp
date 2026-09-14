// P1b end-to-end: the production startup path (RaftBootstrap) with the real
// TCP transport, the real timers and LevelDB log/result stores.
//
// The single-node case needs no socket at all (a one-member group cannot
// receive connections), which also makes it usable where bind() is denied.
// The three-node case exercises loopback TCP end to end.

#include "test_framework.h"

#if defined(SQLDB_HAVE_LEVELDB)

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "server/config.h"
#include "server/logger.h"
#include "server/raft_bootstrap.h"
#include "storage/kv_engine/kv_factory.h"

namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

class TempDir {
public:
  TempDir() {
    const auto base = std::filesystem::temp_directory_path() /
                      "sqldb-raft-bootstrap-XXXXXX";
    // Keep one string: begin()/end() from two different temporaries would be
    // unrelated iterators (and libc++ turns that into length_error("vector")).
    const std::string base_string = base.string();
    std::vector<char> buffer(base_string.begin(), base_string.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr) {
      return;
    }
    path_ = buffer.data();
  }

  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }

  const std::string &path() const { return path_; }

private:
  std::string path_;
};

std::string raft_config_text(int node_id, int port, const std::string &peers,
                             const std::string &log_path,
                             int election_timeout_ms, int heartbeat_ms) {
  return "[raft]\nenabled = true\nnode_id = " + std::to_string(node_id) +
         "\nlisten = 127.0.0.1:" + std::to_string(port) + "\npeers = " +
         peers + "\nelection_timeout_ms = " +
         std::to_string(election_timeout_ms) + "\nheartbeat_ms = " +
         std::to_string(heartbeat_ms) + "\nlog_path = " + log_path + "\n";
}

// CHECK_EQ on an expected's error text: passes with "ok", prints the real
// message on failure.
template <typename T>
std::string describe(const std::expected<T, std::string> &value) {
  return value.has_value() ? std::string{"ok"} : value.error();
}

std::shared_ptr<kv::KVStore> open_local(const std::string &path) {
  // LevelDB's create_if_missing creates the final directory but not parents.
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    return nullptr;
  }
  kv::DatabaseOptions options;
  options.set_path(path).set_create_if_missing(true);
  return kv::open_store(kv::EngineType::LEVELDB, options);
}

// KVStore exposes the raw read path (the state machine's view) through
// new_iterator; sessions go through KVEngine instead.
std::optional<kv::ByteValue> read_applied(kv::KVStore &store,
                                          const kv::Key &key) {
  auto iterator = store.new_iterator(kv::KeyRange::from(key));
  if (iterator == nullptr || !iterator->valid() || iterator->key() != key) {
    return std::nullopt;
  }
  return iterator->value();
}

// A session would retry on the leader's node; the test plays that role.
kv::Status write_until_leader(kv::KVEngine &engine, const std::string &key,
                              const std::string &value, int64_t timeout_ms) {
  const int64_t deadline = now_ms() + timeout_ms;
  kv::Status status = kv::Status::NotLeader;
  while (now_ms() < deadline) {
    status = engine.put(key, value);
    if (status == kv::Status::OK) {
      return status;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  return status;
}

TEST(RaftBootstrap, SingleNodeServesWritesAndSurvivesRestart) {
  TempDir dir;
  CHECK_TRUE(!dir.path().empty());
  auto logger = server::Logger::create("error", "");
  CHECK_TRUE(logger.has_value());
  if (!logger.has_value()) {
    return;
  }

  // One-member group: no peer can connect, so the listener is skipped.
  const std::string config_text = raft_config_text(
      1, 5434, "1@127.0.0.1:5434", dir.path() + "/raft", 80, 20);

  std::shared_ptr<kv::KVStore> last_local;
  const auto open_bootstrap = [&]()
      -> std::expected<std::unique_ptr<server::RaftBootstrap>, std::string> {
    auto config = server::parse_config(config_text);
    if (!config.has_value()) {
      return std::unexpected(config.error());
    }
    if (auto ok = config->validate(); !ok.has_value()) {
      return std::unexpected(ok.error());
    }
    auto local = open_local(dir.path() + "/kv");
    if (local == nullptr) {
      return std::unexpected("cannot open the local leveldb");
    }
    auto bootstrap = server::RaftBootstrap::open(
        *config, local, **logger, server::RaftBootstrap::Options{false});
    if (bootstrap.has_value()) {
      // Keep the local store alive for the assertions below.
      last_local = local;
    }
    return bootstrap;
  };

  {
    auto bootstrap = open_bootstrap();
    CHECK_EQ(describe(bootstrap), std::string{"ok"});
    if (!bootstrap.has_value()) {
      return;
    }
    auto engine = (*bootstrap)->store()->connect();
    CHECK_TRUE(engine != nullptr);
    CHECK_EQ(write_until_leader(*engine, "k", "v1", 5000), kv::Status::OK);

    kv::ByteValue value;
    CHECK_EQ(engine->get("k", &value), kv::Status::OK);
    CHECK_EQ(value, std::string{"v1"});
    // The state machine applied it to the local store as well.
    const auto applied = read_applied(*last_local, "k");
    CHECK_TRUE(applied.has_value());
    if (applied.has_value()) {
      CHECK_EQ(*applied, std::string{"v1"});
    }

    (*bootstrap)->stop();
    CHECK_EQ(last_local->close(), kv::Status::OK);
  }

  // Restart with the same directories: hard state, log, request results and
  // the state machine data must all come back.
  {
    auto bootstrap = open_bootstrap();
    CHECK_EQ(describe(bootstrap), std::string{"ok"});
    if (!bootstrap.has_value()) {
      return;
    }
    auto engine = (*bootstrap)->store()->connect();
    CHECK_TRUE(engine != nullptr);

    // A fresh write must not be mistaken for a replay of the pre-restart
    // request (client ids are salted per process, see RaftKVStore::connect).
    CHECK_EQ(write_until_leader(*engine, "k", "v2", 5000), kv::Status::OK);
    kv::ByteValue value;
    CHECK_EQ(engine->get("k", &value), kv::Status::OK);
    CHECK_EQ(value, std::string{"v2"});

    (*bootstrap)->stop();
    CHECK_EQ(last_local->close(), kv::Status::OK);
  }
}

TEST(RaftCluster, ThreeNodesReplicateOverTcp) {
  TempDir dir;
  CHECK_TRUE(!dir.path().empty());
  auto logger = server::Logger::create("error", "");
  CHECK_TRUE(logger.has_value());
  if (!logger.has_value()) {
    return;
  }

  // Fixed-but-unusual ports: peers must know each other's endpoint before
  // anyone binds, so port 0 is not an option here.
  const int base_port = 42000 + static_cast<int>(::getpid() % 8000);
  std::string peers;
  for (int i = 1; i <= 3; ++i) {
    if (i > 1) {
      peers += ",";
    }
    peers += std::to_string(i) + "@127.0.0.1:" +
             std::to_string(base_port + i - 1);
  }

  struct Node {
    std::shared_ptr<kv::KVStore> local;
    std::unique_ptr<server::RaftBootstrap> bootstrap;
  };
  std::vector<std::unique_ptr<Node>> nodes;
  for (int i = 1; i <= 3; ++i) {
    const std::string node_dir = dir.path() + "/node" + std::to_string(i);
    auto config = server::parse_config(raft_config_text(
        i, base_port + i - 1, peers, node_dir + "/raft", 300, 60));
    CHECK_TRUE(config.has_value());
    if (!config.has_value()) {
      return;
    }
    if (auto ok = config->validate(); !ok.has_value()) {
      CHECK_EQ(ok.error(), std::string{"ok"});
      return;
    }
    auto local = open_local(node_dir + "/kv");
    CHECK_TRUE(local != nullptr);
    if (local == nullptr) {
      return;
    }
    auto bootstrap = server::RaftBootstrap::open(*config, local, **logger);
    if (!bootstrap.has_value() &&
        bootstrap.error().find("bind") != std::string::npos) {
      // The sandbox denies bind(); the same skip rule the other server e2e
      // tests use. Run this test outside the sandbox to exercise the wire.
      fmt::print(stderr,
                 "[skip] RaftCluster.ThreeNodesReplicateOverTcp: {}\n",
                 bootstrap.error());
      return;
    }
    CHECK_EQ(describe(bootstrap), std::string{"ok"});
    if (!bootstrap.has_value()) {
      return;
    }
    auto node = std::make_unique<Node>();
    node->local = local;
    node->bootstrap = std::move(*bootstrap);
    nodes.push_back(std::move(node));
  }

  // Find the leader by trying every node, like a client following the hint.
  const int64_t deadline = now_ms() + 10000;
  kv::Status status = kv::Status::NotLeader;
  while (now_ms() < deadline && status != kv::Status::OK) {
    for (auto &node : nodes) {
      auto engine = node->bootstrap->store()->connect();
      status = engine->put("replicated", "value");
      if (status == kv::Status::OK) {
        break;
      }
    }
    if (status != kv::Status::OK) {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
  }
  CHECK_EQ(status, kv::Status::OK);

  // The committed batch must reach every replica's state machine.
  bool all_replicas_have_it = false;
  const int64_t replica_deadline = now_ms() + 5000;
  while (now_ms() < replica_deadline) {
    all_replicas_have_it = true;
    for (auto &node : nodes) {
      const auto value = read_applied(*node->local, "replicated");
      if (!value.has_value() || *value != "value") {
        all_replicas_have_it = false;
        break;
      }
    }
    if (all_replicas_have_it) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  CHECK_TRUE(all_replicas_have_it);

  for (auto &node : nodes) {
    node->bootstrap->stop();
  }
  for (auto &node : nodes) {
    CHECK_EQ(node->local->close(), kv::Status::OK);
  }
}

} // namespace

#endif // SQLDB_HAVE_LEVELDB
