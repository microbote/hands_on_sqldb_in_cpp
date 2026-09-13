// server/poller_poll.cpp —— poll(2) 后端：全平台兜底（零依赖、可移植）。
// Poller::create() 也放这里（这个文件在哪都编译）。
#include "poller.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace server {
namespace {

class PollPoller final : public Poller {
public:
  bool watch(int fd, short events) override {
    events_[fd] = events;
    return true; // poll 没有"注册"一说，每次 wait 全量带上去
  }

  void unwatch(int fd) override { events_.erase(fd); }

  int wait(int timeout_ms, Event *out, int max_events) override {
    std::vector<pollfd> fds;
    std::vector<int> owners;
    fds.reserve(events_.size());
    owners.reserve(events_.size());
    for (const auto &[fd, events] : events_) {
      pollfd entry{};
      entry.fd = fd;
      entry.events = events;
      fds.push_back(entry);
      owners.push_back(fd);
    }
    const int ready =
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
    if (ready <= 0) {
      return ready; // 0 = 超时；-1 = 错误（EINTR 由上层处理）
    }
    int n = 0;
    for (size_t i = 0; i < fds.size(); ++i) {
      if (fds[i].revents == 0) {
        continue;
      }
      if (n >= max_events) {
        break; // 放不下的下轮再来（水平触发，不会丢）
      }
      out[n++] = Event{owners[i], fds[i].revents};
    }
    return n;
  }

private:
  std::unordered_map<int, short> events_;
};

} // namespace

std::unique_ptr<Poller> make_poll_poller() {
  return std::make_unique<PollPoller>();
}

std::unique_ptr<Poller> Poller::create() {
#if defined(__APPLE__)
  if (auto poller = make_kqueue_poller(); poller != nullptr) {
    return poller;
  }
#elif defined(__linux__)
  if (auto poller = make_epoll_poller(); poller != nullptr) {
    return poller;
  }
#endif
  return make_poll_poller();
}

} // namespace server
