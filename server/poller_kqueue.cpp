// server/poller_kqueue.cpp —— kqueue 后端（macOS / *BSD）
#include "poller.h"

#if defined(__APPLE__)

#include <sys/event.h>
#include <unistd.h>

#include <cerrno>
#include <vector>

namespace server {
namespace {

class KqueuePoller final : public Poller {
public:
  KqueuePoller() : kq_(::kqueue()) {}
  ~KqueuePoller() override {
    if (kq_ >= 0) {
      ::close(kq_);
    }
  }
  bool usable() const { return kq_ >= 0; }

  bool watch(int fd, short events) override {
    struct kevent changes[2];
    int n = 0;
    // 同 filter 重复 EV_ADD = 更新（等价 modify）；不关注的 filter 明确删掉
    if ((events & POLLIN) != 0) {
      EV_SET(&changes[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
             nullptr);
    } else {
      EV_SET(&changes[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    }
    if ((events & POLLOUT) != 0) {
      EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0,
             nullptr);
    } else {
      EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    return apply(changes, n);
  }

  void unwatch(int fd) override {
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    (void)apply(changes, 2);
  }

  int wait(int timeout_ms, Event *out, int max_events) override {
    std::vector<struct kevent> fired(static_cast<size_t>(max_events));
    timespec timeout{};
    timespec *when = nullptr;
    if (timeout_ms >= 0) {
      timeout.tv_sec = timeout_ms / 1000;
      timeout.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
      when = &timeout;
    }
    const int ready = ::kevent(kq_, nullptr, 0, fired.data(), max_events, when);
    if (ready <= 0) {
      return ready; // 0 = 超时；-1 = 错误（EINTR 由上层处理）
    }
    int n = 0;
    for (int i = 0; i < ready; ++i) {
      const struct kevent &event = fired[static_cast<size_t>(i)];
      short revents = 0;
      if (event.filter == EVFILT_READ) {
        revents |= POLLIN;
      }
      if (event.filter == EVFILT_WRITE) {
        revents |= POLLOUT;
      }
      if ((event.flags & EV_EOF) != 0) {
        revents |= POLLHUP; // 对端关了也能读（read 会返回 0），和 poll 对齐
      }
      if ((event.flags & EV_ERROR) != 0) {
        revents |= POLLERR;
      }
      // 同一 fd 的读/写是两个 kevent：合并成一条 Event
      const int fd = static_cast<int>(event.ident);
      bool merged = false;
      for (int j = 0; j < n; ++j) {
        if (out[j].fd == fd) {
          out[j].revents = static_cast<short>(out[j].revents | revents);
          merged = true;
          break;
        }
      }
      if (!merged) {
        out[n++] = Event{fd, revents};
      }
    }
    return n;
  }

private:
  // 提交变更；EV_DELETE 对不存在的 filter 会回 EV_ERROR+ENOENT，吞掉。
  // 做法：changes 和结果队列同传，错误以"结果事件"的形式回来而不是 -1。
  bool apply(struct kevent *changes, int n) {
    struct kevent results[2];
    const timespec nowait{0, 0};
    const int got = ::kevent(kq_, changes, n, results, 2, &nowait);
    if (got < 0) {
      return false;
    }
    for (int i = 0; i < got; ++i) {
      if ((results[i].flags & EV_ERROR) != 0 &&
          results[i].data != ENOENT) {
        return false;
      }
    }
    return true;
  }

  int kq_ = -1;
};

} // namespace

std::unique_ptr<Poller> make_kqueue_poller() {
  auto poller = std::make_unique<KqueuePoller>();
  if (!poller->usable()) {
    return nullptr; // create() 会退回 poll
  }
  return poller;
}

} // namespace server

#endif // __APPLE__
