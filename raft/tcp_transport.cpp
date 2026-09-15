#include "raft/tcp_transport.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <optional>
#include <string_view>
#include <utility>

#include "common/net/socket.h"
#include "common/net/socket_util.h"
#include "raft/message_codec.h"

namespace raft {
namespace {

// Handshake payload: u8 kind + u64 node id, wrapped in the usual frame.
constexpr uint8_t kHelloKind = 1;
constexpr size_t kHelloPayloadSize = 9;

std::string encode_hello(uint64_t node_id) {
  std::string payload;
  payload.push_back(static_cast<char>(kHelloKind));
  for (int shift = 56; shift >= 0; shift -= 8) {
    payload.push_back(static_cast<char>((node_id >> shift) & 0xff));
  }
  return payload;
}

std::expected<NodeId, std::string> decode_hello(std::string_view payload) {
  if (payload.size() != kHelloPayloadSize ||
      static_cast<uint8_t>(payload[0]) != kHelloKind) {
    return std::unexpected("invalid raft handshake frame");
  }
  uint64_t node_id = 0;
  for (size_t i = 1; i < kHelloPayloadSize; ++i) {
    node_id = (node_id << 8) |
              static_cast<uint64_t>(static_cast<unsigned char>(payload[i]));
  }
  if (node_id == 0) {
    return std::unexpected("raft handshake carries node id 0");
  }
  return NodeId{node_id};
}

// Blocking write of a whole buffer. socket_write() never raises SIGPIPE.
bool write_all(int fd, std::string_view data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t wrote = common::net::socket_write(
        fd, data.data() + sent, data.size() - sent);
    if (wrote > 0) {
      sent += static_cast<size_t>(wrote);
      continue;
    }
    if (wrote < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

std::string peer_label(const PeerConfig &peer) {
  return std::to_string(peer.node_id.value) + "@" + peer.host + ":" +
         std::to_string(peer.port);
}

} // namespace

RaftTcpTransport::RaftTcpTransport(Options options, PostFn post)
    : options_(std::move(options)), post_(std::move(post)),
      inbound_(common::svrkit::TcpServerOptions{
                   // Graceful shutdown resumes the reader coroutines
                   // (shutdown(SHUT_RD) makes them see EOF), so the grace
                   // period only bounds a peer that refuses to go away.
                   options_.max_connections, 128, 1000, options_.logger},
               [this](std::shared_ptr<common::svrkit::TcpConnection> conn) {
                 return handle_inbound(std::move(conn));
               }) {
  if (!options_.connect) {
    options_.connect = [](const PeerConfig &peer) -> std::expected<int, std::string> {
      auto socket = common::net::TcpSocket::connect(peer.host, peer.port);
      if (!socket.has_value()) {
        return std::unexpected(socket.error());
      }
      common::net::socket_suppress_sigpipe(socket->fd());
      return socket->release();
    };
  }

  for (const PeerConfig &peer : options_.peers) {
    if (peer.node_id == options_.node_id) {
      continue; // never dial ourselves
    }
    OutboundPeer entry;
    entry.config = peer;
    peers_[peer.node_id.value] = std::move(entry);
  }
  sender_ = std::thread([this] { sender_loop(); });
}

RaftTcpTransport::~RaftTcpTransport() { stop(); }

void RaftTcpTransport::send(NodeId to, uint64_t group_id,
                            const Message &message) {
  if (to == options_.node_id) {
    return; // RaftNode never addresses itself; dropping here keeps the queue clean
  }
  std::string frame = encode_frame(group_id, message);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || outbound_queue_.size() >= options_.max_outbound_queue) {
      dropped_frames_.fetch_add(1);
      return;
    }
    outbound_queue_.emplace_back(to, std::move(frame));
  }
  wake_.notify_one();
}

void RaftTcpTransport::on_message(
    uint64_t group_id,
    std::function<void(NodeId, const Message &)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  on_messages_[group_id] = std::move(callback);
}

std::expected<void, std::string> RaftTcpTransport::listen() {
  auto ok = inbound_.listen(options_.listen_host, options_.listen_port);
  if (!ok.has_value()) {
    return std::unexpected("raft listen failed: " + ok.error());
  }
  options_.listen_port = inbound_.port();
  return {};
}

void RaftTcpTransport::run_inbound() { inbound_.run(); }

void RaftTcpTransport::attach_inbound_connection(int fd) {
  inbound_.attach_connection(fd);
}

uint16_t RaftTcpTransport::bound_port() const { return inbound_.port(); }

size_t RaftTcpTransport::connected_peers() const {
  return connected_peers_.load();
}

void RaftTcpTransport::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }

  // Graceful inbound shutdown: this wakes the event loop, which marks every
  // connection stopping, shuts down its read side and lets the handler
  // coroutines return. A hard loop stop would strand suspended coroutine
  // frames (and their fds would be closed underneath them).
  inbound_.request_shutdown();

  wake_.notify_all();
  if (sender_.joinable()) {
    sender_.join();
  }
}

void RaftTcpTransport::sender_loop() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (stopping_) {
        break;
      }
      if (outbound_queue_.empty()) {
        // Nothing to send: sleep until new work arrives, or until the next
        // reconnect deadline if some peer still has unsent bytes.
        int64_t delay_ms = -1;
        const int64_t now = now_ms();
        for (const auto &[id, peer] : peers_) {
          if (peer.pending.empty()) {
            continue;
          }
          const int64_t due = peer.retry_at_ms - now;
          delay_ms = delay_ms < 0 ? due : std::min(delay_ms, due);
        }
        const auto has_work = [&] { return stopping_ || !outbound_queue_.empty(); };
        if (delay_ms < 0) {
          wake_.wait(lock, has_work);
        } else if (delay_ms > 0) {
          wake_.wait_for(lock, std::chrono::milliseconds(delay_ms), has_work);
        }
        if (stopping_) {
          break;
        }
      }
      while (!outbound_queue_.empty()) {
        auto item = std::move(outbound_queue_.front());
        outbound_queue_.pop_front();
        const auto peer = peers_.find(item.first.value);
        if (peer == peers_.end()) {
          dropped_frames_.fetch_add(1);
          continue;
        }
        peer->second.pending += item.second;
        ++peer->second.pending_frames;
      }
    }

    // Blocking I/O happens without the lock: send() from the Raft service
    // thread must never wait for a dial or a write.
    const int64_t now = now_ms();
    for (auto &[id, peer] : peers_) {
      flush_peer(peer, now);
    }
  }

  for (auto &[id, peer] : peers_) {
    close_peer(peer);
  }
}

void RaftTcpTransport::flush_peer(OutboundPeer &peer, int64_t now) {
  if (peer.pending.empty()) {
    return;
  }
  if (peer.fd < 0 && now < peer.retry_at_ms) {
    return;
  }
  if (peer.fd < 0 && !ensure_connected(peer, now)) {
    return;
  }

  if (!write_all(peer.fd, peer.pending)) {
    log("warn", "raft: send to peer " + peer_label(peer.config) +
                    " failed, will reconnect");
    // The bytes stay in `pending`: the peer sees them again after the
    // reconnect. Raft RPCs tolerate duplicates.
    close_peer(peer);
    peer.retry_at_ms = now + options_.reconnect_backoff_ms;
    return;
  }
  sent_frames_.fetch_add(peer.pending_frames);
  peer.pending.clear();
  peer.pending_frames = 0;
}

bool RaftTcpTransport::ensure_connected(OutboundPeer &peer, int64_t now) {
  auto fd = options_.connect(peer.config);
  if (!fd.has_value()) {
    peer.retry_at_ms = now + options_.reconnect_backoff_ms;
    log("warn", "raft: connect to peer " + peer_label(peer.config) +
                    " failed: " + fd.error());
    return false;
  }
  peer.fd = *fd;
  peer.retry_at_ms = 0;

  const std::string hello =
      frame_payload(encode_hello(options_.node_id.value));
  if (!write_all(peer.fd, hello)) {
    close_peer(peer);
    peer.retry_at_ms = now + options_.reconnect_backoff_ms;
    log("warn", "raft: handshake with peer " + peer_label(peer.config) +
                    " failed");
    return false;
  }
  connected_peers_.fetch_add(1);
  log("info", "raft: connected to peer " + peer_label(peer.config));
  return true;
}

void RaftTcpTransport::close_peer(OutboundPeer &peer) {
  if (peer.fd < 0) {
    return;
  }
  ::close(peer.fd);
  peer.fd = -1;
  connected_peers_.fetch_sub(1);
}

void RaftTcpTransport::deliver(uint64_t group_id, NodeId from,
                               Message message) {
  std::function<void(NodeId, const Message &)> callback;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    const auto it = on_messages_.find(group_id);
    if (it == on_messages_.end()) {
      dropped_frames_.fetch_add(1);
      return;
    }
    callback = it->second;
  }
  if (!callback || !post_) {
    dropped_frames_.fetch_add(1);
    return;
  }
  if (!post_(group_id, [callback, from, message = std::move(message)] {
        callback(from, message);
      })) {
    dropped_frames_.fetch_add(1);
  }
}

common::svrkit::Task RaftTcpTransport::handle_inbound(
    std::shared_ptr<common::svrkit::TcpConnection> connection) {
  FrameDecoder decoder;
  std::optional<NodeId> peer;

  while (!stopping_ && !connection->stopping()) {
    std::string chunk;
    ssize_t nread = 0;
    co_await connection->read_some(chunk, nread);
    if (nread <= 0) {
      break;
    }
    decoder.append(chunk);

    bool fatal = false;
    while (true) {
      std::string frame;
      auto next = decoder.next(&frame);
      if (!next.has_value()) {
        log("warn", std::string("raft: inbound frame rejected: ") +
                        next.error().message);
        receive_errors_.fetch_add(1);
        fatal = true;
        break;
      }
      if (!next.value()) {
        break; // need more bytes
      }

      if (!peer.has_value()) {
        // First frame must be the handshake that tells us who dialed in.
        auto hello = decode_hello(frame);
        if (!hello.has_value()) {
          log("warn", "raft: inbound connection did not start with a "
                       "handshake");
          receive_errors_.fetch_add(1);
          fatal = true;
          break;
        }
        if (*hello == options_.node_id) {
          log("warn", "raft: inbound connection claims our own node id");
          receive_errors_.fetch_add(1);
          fatal = true;
          break;
        }
        peer = *hello;
        log("info", "raft: peer " + std::to_string(peer->value) +
                        " connected");
        continue;
      }

      auto message = decode_message(frame);
      if (!message.has_value()) {
        log("warn", std::string("raft: inbound message rejected: ") +
                        message.error().message);
        receive_errors_.fetch_add(1);
        fatal = true;
        break;
      }
      deliver(message->group_id, *peer, std::move(message->message));
    }
    if (fatal) {
      break;
    }
  }
  if (peer.has_value()) {
    log("info", "raft: peer " + std::to_string(peer->value) + " disconnected");
  }
}

int64_t RaftTcpTransport::now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void RaftTcpTransport::log(const char *level,
                           const std::string &message) const {
  if (options_.logger) {
    options_.logger(level, message);
  }
}

} // namespace raft
