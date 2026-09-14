// tests/test_svrkit/test_poller.cpp
//
// 平台后端（mac=kqueue / linux=epoll / 兜底=poll）的统一行为：
// watch 之后事件能报上来、事件位用 poll 词汇、unwatch 之后不再报。
#include "test_framework.h"

#include "common/net/poller.h"

#include <sys/socket.h>
#include <unistd.h>

namespace {

struct SocketPair {
  SocketPair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      fds[0] = fds[1] = -1;
    }
  }
  ~SocketPair() {
    if (fds[0] >= 0) {
      ::close(fds[0]);
    }
    if (fds[1] >= 0) {
      ::close(fds[1]);
    }
  }
  int fds[2];
};

} // namespace

TEST(Poller, ReportsReadReadinessInPollVocabulary) {
  auto poller = common::net::Poller::create();
  CHECK(poller != nullptr);
  SocketPair pair;
  CHECK(pair.fds[0] >= 0);
  if (poller == nullptr || pair.fds[0] < 0) {
    return;
  }

  CHECK(poller->watch(pair.fds[0], POLLIN));
  common::net::Poller::Event fired[4];
  // 没数据：超时返回 0
  CHECK_EQ(poller->wait(20, fired, 4), 0);

  // 写一个字节：读端 POLLIN
  CHECK_EQ(::write(pair.fds[1], "x", 1), ssize_t{1});
  const int ready = poller->wait(1000, fired, 4);
  CHECK_EQ(ready, 1);
  if (ready == 1) {
    CHECK_EQ(fired[0].fd, pair.fds[0]);
    CHECK((fired[0].revents & POLLIN) != 0);
  }

  // 水平触发：数据没读走，再 wait 还报
  CHECK_EQ(poller->wait(20, fired, 4), 1);
  char byte = 0;
  CHECK_EQ(::read(pair.fds[0], &byte, 1), ssize_t{1});
  CHECK_EQ(poller->wait(20, fired, 4), 0);

  // unwatch 之后：即使有事件也不报
  poller->unwatch(pair.fds[0]);
  CHECK_EQ(::write(pair.fds[1], "y", 1), ssize_t{1});
  CHECK_EQ(poller->wait(20, fired, 4), 0);
}

TEST(Poller, ReportsWriteReadinessAndPeerClose) {
  auto poller = common::net::Poller::create();
  CHECK(poller != nullptr);
  SocketPair pair;
  CHECK(pair.fds[0] >= 0);
  if (poller == nullptr || pair.fds[0] < 0) {
    return;
  }

  // 写端几乎总是可写（缓冲区没满）
  CHECK(poller->watch(pair.fds[1], POLLOUT));
  common::net::Poller::Event fired[4];
  CHECK_EQ(poller->wait(1000, fired, 4), 1);
  CHECK((fired[0].revents & POLLOUT) != 0);

  // 对端关闭：读端要报（POLLIN|POLLHUP 任一，各后端词汇略有差异）
  CHECK(poller->watch(pair.fds[0], POLLIN));
  ::close(pair.fds[1]);
  pair.fds[1] = -1;
  const int ready = poller->wait(1000, fired, 4);
  CHECK(ready >= 1);
  if (ready >= 1) {
    CHECK_EQ(fired[0].fd, pair.fds[0]);
    CHECK((fired[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0);
  }
}
