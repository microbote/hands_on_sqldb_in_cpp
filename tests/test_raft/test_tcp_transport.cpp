// RaftTcpTransport: wire framing, handshake and the "post, never call back
// inline" rule. No bind() is needed: the sender's dialer and the inbound path
// both accept injected socketpair fds.

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "raft/message_codec.h"
#include "raft/tcp_transport.h"
#include "test_framework.h"

namespace {

using raft::NodeId;

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool write_all(int fd, const std::string &data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t wrote = ::write(fd, data.data() + sent, data.size() - sent);
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

// Reads complete frames from a blocking fd until `expected` arrived or the
// deadline passes.
std::vector<std::string> read_frames(int fd, size_t expected,
                                     int64_t timeout_ms) {
  raft::FrameDecoder decoder;
  std::vector<std::string> frames;
  const int64_t deadline = now_ms() + timeout_ms;
  while (frames.size() < expected && now_ms() < deadline) {
    pollfd p{fd, POLLIN, 0};
    const int rc = ::poll(&p, 1, 20);
    if (rc <= 0) {
      continue;
    }
    char buffer[4096];
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got <= 0) {
      break;
    }
    decoder.append(std::string_view(buffer, static_cast<size_t>(got)));
    while (frames.size() < expected) {
      std::string frame;
      auto next = decoder.next(&frame);
      if (!next.has_value() || !next.value()) {
        break;
      }
      frames.push_back(std::move(frame));
    }
  }
  return frames;
}

bool wait_for_eof(int fd, int64_t timeout_ms) {
  const int64_t deadline = now_ms() + timeout_ms;
  while (now_ms() < deadline) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 20) <= 0) {
      continue;
    }
    char buffer[64];
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got == 0) {
      return true;
    }
    if (got < 0) {
      return false;
    }
  }
  return false;
}

std::string hello_payload(uint64_t node_id) {
  std::string payload;
  payload.push_back(static_cast<char>(1)); // kind = handshake
  for (int shift = 56; shift >= 0; shift -= 8) {
    payload.push_back(static_cast<char>((node_id >> shift) & 0xff));
  }
  return payload;
}

raft::RaftTcpTransport::Options base_options(uint64_t node_id) {
  raft::RaftTcpTransport::Options options;
  options.node_id = NodeId{node_id};
  options.logger = [](const char *, const std::string &) {};
  return options;
}

TEST(TcpTransport, SendsHandshakeThenFramesInOrder) {
  int pair[2] = {-1, -1};
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  std::atomic<int> connects{0};
  auto options = base_options(1);
  options.peers = {raft::PeerConfig{NodeId{2}, "127.0.0.1", 1}};
  options.connect = [&](const raft::PeerConfig &)
      -> std::expected<int, std::string> {
    connects.fetch_add(1);
    return ::dup(pair[1]);
  };

  raft::RaftTcpTransport transport(
      options, [](std::function<void()>) { return true; });
  transport.send(NodeId{2}, raft::RequestVoteRequest{5, NodeId{1}, 3, 2});
  transport.send(NodeId{2}, raft::AppendEntriesResponse{5, true, 4, 7});

  const std::vector<std::string> frames = read_frames(pair[0], 3, 2000);
  CHECK_EQ(frames.size(), size_t{3});
  if (frames.size() == 3) {
    // Frame 0: handshake carrying our node id.
    CHECK_EQ(frames[0].size(), size_t{9});
    if (frames[0].size() == 9) {
      CHECK_EQ(static_cast<uint8_t>(frames[0][0]), uint8_t{1});
      uint64_t id = 0;
      for (size_t i = 1; i < 9; ++i) {
        id = (id << 8) |
             static_cast<uint64_t>(static_cast<unsigned char>(frames[0][i]));
      }
      CHECK_EQ(id, uint64_t{1});
    }

    auto vote = raft::decode_message(frames[1]);
    CHECK_TRUE(vote.has_value());
    if (vote.has_value()) {
      const auto &request = std::get<raft::RequestVoteRequest>(*vote);
      CHECK_EQ(request.term, uint64_t{5});
      CHECK_EQ(request.candidate_id.value, uint64_t{1});
      CHECK_EQ(request.last_log_index, uint64_t{3});
      CHECK_EQ(request.last_log_term, uint64_t{2});
    }

    auto append = raft::decode_message(frames[2]);
    CHECK_TRUE(append.has_value());
    if (append.has_value()) {
      const auto &response = std::get<raft::AppendEntriesResponse>(*append);
      CHECK_EQ(response.term, uint64_t{5});
      CHECK_TRUE(response.success);
      CHECK_EQ(response.match_index, uint64_t{4});
      CHECK_EQ(response.round, uint64_t{7});
    }
  }

  CHECK_EQ(connects.load(), 1);
  CHECK_EQ(transport.connected_peers(), size_t{1});
  CHECK_EQ(transport.sent_frames(), uint64_t{2});

  transport.stop();
  ::close(pair[0]);
  ::close(pair[1]);
}

TEST(TcpTransport, ReceivesHandshakeThenPostsMessages) {
  int pair[2] = {-1, -1};
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  std::mutex posted_mutex;
  std::vector<std::function<void()>> posted;
  auto options = base_options(1);
  raft::RaftTcpTransport transport(options, [&](std::function<void()> work) {
    std::lock_guard<std::mutex> lock(posted_mutex);
    posted.push_back(std::move(work));
    return true;
  });

  std::vector<std::pair<NodeId, raft::Message>> received_all;
  transport.on_message([&](NodeId sender, const raft::Message &message) {
    received_all.emplace_back(sender, message);
  });

  transport.attach_inbound_connection(pair[1]);
  std::thread loop_thread([&] { transport.run_inbound(); });

  const std::string outbound =
      raft::frame_payload(hello_payload(9)) +
      raft::encode_frame(raft::RequestVoteResponse{4, true}) +
      raft::encode_frame(
          raft::AppendEntriesRequest{4, NodeId{9}, 1, 3, {}, 1, 6});
  CHECK_TRUE(write_all(pair[0], outbound));

  // Wait until the inbound loop produced work items.
  const int64_t deadline = now_ms() + 2000;
  while (now_ms() < deadline) {
    {
      std::lock_guard<std::mutex> lock(posted_mutex);
      if (posted.size() >= 2) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  {
    std::lock_guard<std::mutex> lock(posted_mutex);
    CHECK_EQ(posted.size(), size_t{2});
  }
  // The receive path must not call back inline: the frames are only queued.
  CHECK_TRUE(received_all.empty());

  std::vector<std::function<void()>> work_items;
  {
    std::lock_guard<std::mutex> lock(posted_mutex);
    work_items.swap(posted);
  }
  for (auto &work : work_items) {
    work();
  }

  CHECK_EQ(received_all.size(), size_t{2});
  if (received_all.size() == 2) {
    CHECK_EQ(received_all[0].first.value, uint64_t{9});
    CHECK_EQ(std::get<raft::RequestVoteResponse>(received_all[0].second).term,
             uint64_t{4});
    CHECK_EQ(received_all[1].first.value, uint64_t{9});
    const auto &append =
        std::get<raft::AppendEntriesRequest>(received_all[1].second);
    CHECK_EQ(append.term, uint64_t{4});
    CHECK_EQ(append.leader_id.value, uint64_t{9});
    CHECK_EQ(append.round, uint64_t{6});
  }

  transport.stop();
  loop_thread.join();
  ::close(pair[0]);
}

TEST(TcpTransport, ClosesConnectionThatSkipsTheHandshake) {
  int pair[2] = {-1, -1};
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  std::atomic<int> posted{0};
  auto options = base_options(1);
  raft::RaftTcpTransport transport(
      options, [&](std::function<void()>) {
        posted.fetch_add(1);
        return true;
      });
  transport.attach_inbound_connection(pair[1]);
  std::thread loop_thread([&] { transport.run_inbound(); });

  // A Raft frame first: no handshake, so the transport must drop the peer.
  CHECK_TRUE(write_all(pair[0],
                       raft::encode_frame(raft::RequestVoteResponse{1, true})));
  CHECK_TRUE(wait_for_eof(pair[0], 2000));
  CHECK_EQ(posted.load(), 0);
  CHECK_EQ(transport.receive_errors(), uint64_t{1});

  transport.stop();
  loop_thread.join();
  ::close(pair[0]);
}

TEST(TcpTransport, ReconnectsAfterWriteFailure) {
  int pair[2] = {-1, -1};
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  std::atomic<int> connects{0};
  auto options = base_options(1);
  options.peers = {raft::PeerConfig{NodeId{2}, "127.0.0.1", 1}};
  options.reconnect_backoff_ms = 10;
  options.connect = [&](const raft::PeerConfig &)
      -> std::expected<int, std::string> {
    connects.fetch_add(1);
    return ::dup(pair[1]);
  };

  raft::RaftTcpTransport transport(
      options, [](std::function<void()>) { return true; });
  transport.send(NodeId{2}, raft::RequestVoteResponse{1, true});
  CHECK_EQ(read_frames(pair[0], 2, 2000).size(), size_t{2});

  // Peer goes away: the next write fails, so the sender must reconnect.
  ::close(pair[0]);
  transport.send(NodeId{2}, raft::RequestVoteResponse{2, true});

  const int64_t deadline = now_ms() + 2000;
  while (connects.load() < 2 && now_ms() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  CHECK_GE(connects.load(), 2);

  transport.stop();
  ::close(pair[1]);
}

} // namespace
