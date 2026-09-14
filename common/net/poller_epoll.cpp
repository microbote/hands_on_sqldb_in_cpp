// common/net/poller_epoll.cpp —— epoll 后端（Linux）
#include "poller.h"

#if defined(__linux__)

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <vector>

namespace common::net {
namespace {

class EpollPoller final : public Poller {
public:
  EpollPoller() : ep_(::epoll_create1(EPOLL_CLOEXEC)) {}
  ~EpollPoller() override {
    if (ep_ >= 0) {
      ::close(ep_);
    }
  }
  bool usable() const { return ep_ >= 0; }

  bool watch(int fd, short events) override {
    epoll_event wanted{};
    wanted.data.fd = fd;
    if ((events & POLLIN) != 0) {
      wanted.events |= EPOLLIN;
    }
    if ((events & POLLOUT) != 0) {
      wanted.events |= EPOLLOUT;
    }
    wanted.events |= EPOLLRDHUP; // 对端关闭也要报（不开 EPOLLET：保持水平触发）
    // 先 MOD（已注册的路径最常见），没注册过再 ADD
    if (::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &wanted) == 0) {
      return true;
    }
    if (errno != ENOENT) {
      return false;
    }
    return ::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &wanted) == 0;
  }

  void unwatch(int fd) override {
    // 失败（没注册过/fd 已关）无所谓：epoll 对关闭的 fd 会自动摘除
    (void)::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
  }

  int wait(int timeout_ms, Event *out, int max_events) override {
    std::vector<epoll_event> fired(static_cast<size_t>(max_events));
    const int ready =
        ::epoll_wait(ep_, fired.data(), max_events, timeout_ms);
    if (ready <= 0) {
      return ready; // 0 = 超时；-1 = 错误（EINTR 由上层处理）
    }
    int n = 0;
    for (int i = 0; i < ready; ++i) {
      const uint32_t bits = fired[static_cast<size_t>(i)].events;
      short revents = 0;
      if ((bits & EPOLLIN) != 0) {
        revents |= POLLIN;
      }
      if ((bits & EPOLLOUT) != 0) {
        revents |= POLLOUT;
      }
      if ((bits & (EPOLLHUP | EPOLLRDHUP)) != 0) {
        revents |= POLLHUP;
      }
      if ((bits & EPOLLERR) != 0) {
        revents |= POLLERR;
      }
      out[n++] = Event{fired[static_cast<size_t>(i)].data.fd, revents};
    }
    return n;
  }

private:
  int ep_ = -1;
};

} // namespace

std::unique_ptr<Poller> make_epoll_poller() {
  auto poller = std::make_unique<EpollPoller>();
  if (!poller->usable()) {
    return nullptr; // create() 会退回 poll
  }
  return poller;
}

} // namespace common::net

#endif // __linux__
