#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/svrkit/tcp_server.h"
#include "raft/transport.h"
#include "raft/types.h"

namespace raft {

// Production Transport over TCP.
//
// Topology: **every node dials every peer**, so each pair of nodes has one
// connection in each direction. That looks redundant, but it keeps ownership
// simple and race-free:
//
//   - the outbound sender thread owns its sockets exclusively (blocking
//     connect + write), so a send never has to hop onto another thread's event
//     loop;
//   - the inbound loop (svrkit::TcpServer) owns the accepted sockets
//     exclusively (non-blocking reads);
//   - the dialer identifies itself with a one-way handshake frame, so the
//     acceptor does not need the remote address (which svrkit does not expose).
//
// The receive path never touches RaftNode: decoded messages are handed to
// `post` (RaftRuntime::post), which runs them on the Raft service thread.
class RaftTcpTransport final : public Transport {
public:
  struct Options {
    NodeId node_id;
    std::string listen_host = "127.0.0.1";
    uint16_t listen_port = 0;
    std::vector<PeerConfig> peers; // includes this node; self is skipped
    // Test seam: dials one peer and returns a connected fd owned by the
    // transport. Defaults to common::net::TcpSocket::connect.
    std::function<std::expected<int, std::string>(const PeerConfig &)> connect;
    std::function<void(const char *, const std::string &)> logger;
    size_t max_connections = 64;
    size_t max_outbound_queue = 4096;
    int64_t reconnect_backoff_ms = 500;
  };

  // Runs `work` on the given group's Raft service thread. Returning false means
  // the raft service is stopping or backed up (the frame is dropped and
  // counted). The group id lets one shared transport dispatch to the right
  // group's RaftRuntime.
  using PostFn =
      std::function<bool(uint64_t group_id, std::function<void()>)>;

  RaftTcpTransport(Options options, PostFn post);
  ~RaftTcpTransport() override;

  RaftTcpTransport(const RaftTcpTransport &) = delete;
  RaftTcpTransport &operator=(const RaftTcpTransport &) = delete;

  // Transport interface. `send` only enqueues; `on_message` stores the
  // callback that RaftNode installs in start().
  void send(NodeId to, uint64_t group_id, const Message &message) override;
  void on_message(
      uint64_t group_id,
      std::function<void(NodeId, const Message &)>) override;

  // Binds the inbound listener. Production calls this before run_inbound().
  std::expected<void, std::string> listen();
  // Blocks on the inbound event loop (one dedicated thread in production).
  void run_inbound();
  // Stops the sender thread and the inbound loop. Idempotent.
  void stop();

  // Test seam: attach an already-connected fd (socketpair) to the inbound
  // path, exactly like an accepted connection. The fd is owned afterwards.
  void attach_inbound_connection(int fd);

  uint16_t bound_port() const;
  uint64_t sent_frames() const { return sent_frames_.load(); }
  uint64_t dropped_frames() const { return dropped_frames_.load(); }
  uint64_t receive_errors() const { return receive_errors_.load(); }
  size_t connected_peers() const;

private:
  struct OutboundPeer {
    PeerConfig config;
    int fd = -1;
    int64_t retry_at_ms = 0;
    std::string pending;
    size_t pending_frames = 0;
  };

  common::svrkit::Task
  handle_inbound(std::shared_ptr<common::svrkit::TcpConnection> connection);
  void deliver(uint64_t group_id, NodeId from, Message message);
  void sender_loop();
  void flush_peer(OutboundPeer &peer, int64_t now);
  bool ensure_connected(OutboundPeer &peer, int64_t now);
  void close_peer(OutboundPeer &peer);
  void log(const char *level, const std::string &message) const;
  static int64_t now_ms();

  Options options_;
  PostFn post_;
  common::svrkit::TcpServer inbound_;
  mutable std::mutex callback_mutex_;
  std::map<uint64_t, std::function<void(NodeId, const Message &)>>
      on_messages_;

  std::thread sender_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::pair<NodeId, std::string>> outbound_queue_;
  std::map<uint64_t, OutboundPeer> peers_;
  std::atomic<bool> stopping_{false};

  std::atomic<size_t> connected_peers_{0};
  std::atomic<uint64_t> sent_frames_{0};
  std::atomic<uint64_t> dropped_frames_{0};
  std::atomic<uint64_t> receive_errors_{0};
};

} // namespace raft
