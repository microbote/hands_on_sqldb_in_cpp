#include <string>
#include <vector>

#include "raft/peers.h"
#include "test_framework.h"

namespace {

using raft::NodeId;

TEST(RaftPeers, ParsesAndFormatsPeerList) {
  auto peers = raft::parse_peer_list(
      "1@127.0.0.1:5434, 2@10.0.0.7:5435,\n3@[::1]:5436");
  CHECK_TRUE(peers.has_value());
  if (!peers.has_value()) {
    return;
  }
  CHECK_EQ(peers->size(), size_t{3});
  CHECK_EQ((*peers)[0].node_id.value, uint64_t{1});
  CHECK_EQ((*peers)[0].host, std::string("127.0.0.1"));
  CHECK_EQ((*peers)[0].port, uint16_t{5434});
  CHECK_EQ((*peers)[1].host, std::string("10.0.0.7"));
  CHECK_EQ((*peers)[2].host, std::string("[::1]"));
  CHECK_EQ((*peers)[2].port, uint16_t{5436});

  CHECK_EQ(raft::format_peer_list(*peers),
           std::string("1@127.0.0.1:5434,2@10.0.0.7:5435,3@[::1]:5436"));
}

TEST(RaftPeers, RejectsMalformedLists) {
  const std::vector<std::string> bad = {
      "",
      "1",
      "1@127.0.0.1",
      "1@:5434",
      "0@127.0.0.1:5434",
      "x@127.0.0.1:5434",
      "1@127.0.0.1:0",
      "1@127.0.0.1:70000",
      "1@127.0.0.1:5434,1@127.0.0.1:5435", // duplicate id
      "1@127.0.0.1:5434,2@127.0.0.1:5434", // duplicate endpoint
      "1@127.0.0.1:5434,,2@127.0.0.1:5435", // empty entry in the middle
  };
  for (const auto &text : bad) {
    auto peers = raft::parse_peer_list(text);
    CHECK_FALSE(peers.has_value());
    if (!peers.has_value()) {
      CHECK(peers.error().find("raft.peers") != std::string::npos);
    }
  }
}

TEST(RaftPeers, AllowsTrailingComma) {
  auto peers = raft::parse_peer_list("1@127.0.0.1:5434,");
  CHECK_TRUE(peers.has_value());
  if (peers.has_value()) {
    CHECK_EQ(peers->size(), size_t{1});
    CHECK_EQ((*peers)[0].node_id.value, uint64_t{1});
  }
}

} // namespace
