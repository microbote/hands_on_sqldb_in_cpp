// tests/test_server/test_e2e.cpp
//
// 端到端：真起一个 Server（mock 引擎 + 内核挑端口），用阻塞客户端连上去跑 SQL。
// 覆盖设计里的验收标准：多行插入、NULL 与空串、错误 span、
// 两条连接的事务可见性（A 未提交 B 看不到）、只读事务不挡写者、只读可重复读。
#include "test_framework.h"

#include "storage_helper.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>

namespace {

std::string cell(const srvtest::Client::Response &response, size_t row,
                 size_t column) {
  if (row >= response.rows.size() || column >= response.rows[row].size()) {
    return "<out-of-range>";
  }
  const auto &value = response.rows[row][column];
  return value.is_null ? "<null>" : value.text;
}

// 沙箱/CI 里可能不允许 bind()：这时跳过（打印说明），不算失败
bool started_or_skip(srvtest::RunningServer &server) {
  if (server.start()) {
    return true;
  }
  fmt::print(stderr, "[skip] 无法监听回环端口（{}）；这个环境不允许 bind()\n",
             server.last_error());
  return false;
}

// ---- 多线程测试用的"裸"客户端助手 ----
// 不用 srvtest::Client：它的 CHECK 会碰测试框架的全局计数器，不是线程安全的。

bool raw_next_frame(int fd, std::string &in, server::DecodedFrame *frame) {
  while (true) {
    size_t consumed = 0;
    std::string error;
    if (server::try_decode_frame(in, frame, &consumed, &error)) {
      in.erase(0, consumed);
      return true;
    }
    if (!error.empty()) {
      return false;
    }
    char chunk[4096];
    const ssize_t got = ::read(fd, chunk, sizeof(chunk));
    if (got <= 0) {
      return false;
    }
    in.append(chunk, static_cast<size_t>(got));
  }
}

int raw_connect(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  timeval timeout{5, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  std::string in;
  server::DecodedFrame hello;
  if (!raw_next_frame(fd, in, &hello) ||
      hello.type != server::FrameType::kHello) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// 跑一条语句，返回结果行数；任何环节出错返回 -1
int raw_row_count(int fd, std::string &in, const std::string &sql) {
  const std::string out = server::encode_query(sql);
  size_t sent = 0;
  while (sent < out.size()) {
    const ssize_t wrote = ::write(fd, out.data() + sent, out.size() - sent);
    if (wrote <= 0) {
      return -1;
    }
    sent += static_cast<size_t>(wrote);
  }
  int rows = 0;
  while (true) {
    server::DecodedFrame frame;
    if (!raw_next_frame(fd, in, &frame)) {
      return -1;
    }
    if (frame.type == server::FrameType::kRow) {
      ++rows;
    } else if (frame.type == server::FrameType::kOk) {
      return rows;
    } else if (frame.type == server::FrameType::kError) {
      return -1;
    }
  }
}

} // namespace

TEST(ServerE2E, CreateInsertSelect) {
  srvtest::RunningServer server;
  if (!started_or_skip(server)) {
    return;
  }
  srvtest::Client client(server.port());

  auto created = client.query("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
  CHECK(created.ok);

  auto inserted =
      client.query("INSERT INTO t (id, v) VALUES (1, 10), (2, 20), (3, 30)");
  CHECK(inserted.ok);
  CHECK_EQ(inserted.affected_rows, uint64_t{3});

  auto selected = client.query("SELECT id, v FROM t ORDER BY id");
  CHECK(selected.ok);
  CHECK_EQ(selected.columns.size(), size_t{2});
  CHECK_EQ(selected.rows.size(), size_t{3});
  CHECK_EQ(cell(selected, 0, 0), std::string("1"));
  CHECK_EQ(cell(selected, 2, 1), std::string("30"));
}

TEST(ServerE2E, NullAndEmptyStringStayApart) {
  srvtest::RunningServer server;
  if (!started_or_skip(server)) {
    return;
  }
  srvtest::Client client(server.port());

  CHECK(client.query("CREATE TABLE n (id INT PRIMARY KEY, s VARCHAR(8), x INT)")
            .ok);
  CHECK(client.query("INSERT INTO n (id, s, x) VALUES (1, '', NULL)").ok);
  CHECK(client.query("INSERT INTO n (id, s, x) VALUES (2, 'NULL', 5)").ok);

  auto selected = client.query("SELECT s, x FROM n ORDER BY id");
  CHECK(selected.ok);
  CHECK_EQ(selected.rows.size(), size_t{2});
  // 第 1 行：空串 + NULL
  CHECK(!selected.rows[0][0].is_null);
  CHECK_EQ(selected.rows[0][0].text, std::string());
  CHECK(selected.rows[0][1].is_null);
  // 第 2 行：字面量字符串 "NULL" + 5
  CHECK(!selected.rows[1][0].is_null);
  CHECK_EQ(selected.rows[1][0].text, std::string("NULL"));
  CHECK_EQ(selected.rows[1][1].text, std::string("5"));
}

TEST(ServerE2E, ErrorsCarrySpanAndStayOpen) {
  srvtest::RunningServer server;
  if (!started_or_skip(server)) {
    return;
  }
  srvtest::Client client(server.port());

  auto missing = client.query("SELECT * FROM nosuch");
  CHECK(!missing.ok);
  CHECK(missing.error.message.find("nosuch") != std::string::npos);
  CHECK(missing.error.begin_line == 1);
  CHECK(missing.error.begin_column > 1); // 指向表名
  CHECK(missing.error.sql.find("nosuch") != std::string::npos);

  auto syntax = client.query("SELCT 1");
  CHECK(!syntax.ok);

  // 出错之后连接还能继续用（长连接：多语句交互）
  CHECK(client.query("CREATE TABLE e (id INT PRIMARY KEY)").ok);
}

TEST(ServerE2E, TransactionVisibilityAcrossConnections) {
  srvtest::RunningServer server;
  if (!started_or_skip(server)) {
    return;
  }
  srvtest::Client a(server.port());
  srvtest::Client b(server.port());

  CHECK(a.query("CREATE TABLE tx (id INT PRIMARY KEY, v INT)").ok);

  // A 开事务并插入：B 看不到
  CHECK(a.query("BEGIN").ok);
  CHECK(a.query("INSERT INTO tx (id, v) VALUES (1, 100)").ok);
  auto from_a = a.query("SELECT id FROM tx");
  CHECK(from_a.ok);
  CHECK_EQ(from_a.rows.size(), size_t{1}); // 读自己的写
  auto from_b = b.query("SELECT id FROM tx");
  CHECK(from_b.ok);
  CHECK_EQ(from_b.rows.size(), size_t{0}); // 未提交不可见

  // A 提交后 B 看得到
  CHECK(a.query("COMMIT").ok);
  auto after = b.query("SELECT id FROM tx");
  CHECK(after.ok);
  CHECK_EQ(after.rows.size(), size_t{1});
}

TEST(ServerE2E, ReadOnlyTransactionDoesNotBlockWriterAndIsRepeatable) {
  srvtest::RunningServer server;
  if (!started_or_skip(server)) {
    return;
  }
  srvtest::Client a(server.port());
  srvtest::Client b(server.port());

  CHECK(a.query("CREATE TABLE rr (id INT PRIMARY KEY, v INT)").ok);
  CHECK(a.query("INSERT INTO rr (id, v) VALUES (1, 1)").ok);

  // A 开只读事务（快照）
  CHECK(a.query("BEGIN").ok);
  auto first = a.query("SELECT v FROM rr WHERE id = 1");
  CHECK(first.ok);
  CHECK_EQ(cell(first, 0, 0), std::string("1"));

  // B 改并提交：A 的读事务不该挡住它
  auto wrote = b.query("UPDATE rr SET v = 2 WHERE id = 1");
  CHECK(wrote.ok);
  auto b_read = b.query("SELECT v FROM rr WHERE id = 1");
  CHECK(b_read.ok);
  CHECK_EQ(cell(b_read, 0, 0), std::string("2"));

  // A 再读：还是快照里的旧值（可重复读）
  auto second = a.query("SELECT v FROM rr WHERE id = 1");
  CHECK(second.ok);
  CHECK_EQ(cell(second, 0, 0), std::string("1"));

  // A 结束事务后看最新
  CHECK(a.query("COMMIT").ok);
  auto third = a.query("SELECT v FROM rr WHERE id = 1");
  CHECK(third.ok);
  CHECK_EQ(cell(third, 0, 0), std::string("2"));
}

TEST(ServerE2E, ManyConnectionsAndStatements) {
  srvtest::RunningServer server;
  if (!started_or_skip(server)) {
    return;
  }

  constexpr int kClients = 4;
  std::vector<std::unique_ptr<srvtest::Client>> clients;
  for (int i = 0; i < kClients; ++i) {
    clients.push_back(std::make_unique<srvtest::Client>(server.port()));
  }
  CHECK(clients[0]->query("CREATE TABLE multi (id INT PRIMARY KEY)").ok);
  for (int i = 0; i < kClients; ++i) {
    const std::string sql =
        "INSERT INTO multi (id) VALUES (" + std::to_string(i + 1) + ")";
    auto response = clients[i]->query(sql);
    CHECK(response.ok);
  }
  auto count = clients[0]->query("SELECT id FROM multi");
  CHECK(count.ok);
  CHECK_EQ(count.rows.size(), size_t{kClients});
}

TEST(ServerE2E, ReadPoolServesConcurrentReaders) {
  srvtest::RunningServer server(true, [](server::ServerConfig &config) {
    config.set("execution.read_threads", sql::Value(int64_t{4}));
  });
  if (!started_or_skip(server)) {
    return;
  }
  // 能 accept 就说明事件循环已经跑起来，读池必然已就位（run() 先建池子）
  CHECK_EQ(server.running().read_pool_size(), size_t{4});

  srvtest::Client setup(server.port());
  CHECK(setup.query("CREATE TABLE rp (id INT PRIMARY KEY, v INT)").ok);
  CHECK(setup.query("INSERT INTO rp (id, v) VALUES (1, 100)").ok);

  // 4 个线程各打 20 条纯 SELECT：都进读线程池并发执行，结果必须全对
  constexpr int kReaders = 4;
  constexpr int kReadsEach = 20;
  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kReaders; ++i) {
    threads.emplace_back([&server, &mismatches] {
      const int fd = raw_connect(server.port());
      if (fd < 0) {
        mismatches.fetch_add(1);
        return;
      }
      std::string in;
      for (int q = 0; q < kReadsEach; ++q) {
        if (raw_row_count(fd, in, "SELECT v FROM rp WHERE id = 1") != 1) {
          mismatches.fetch_add(1);
        }
      }
      ::close(fd);
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  CHECK_EQ(mismatches.load(), 0);
}
