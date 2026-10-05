// C++ program on top of the preloaded allocator: containers, strings,
// shared_ptr and objects deleted by a different thread than created them.
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

int main() {
  std::map<std::string, std::vector<int>> m;
  for (int i = 0; i < 20000; i++) m["key" + std::to_string(i % 3000)].push_back(i);
  std::unordered_map<int, std::string> u;
  for (int i = 0; i < 50000; i++) u[i] = std::string(static_cast<size_t>(i % 200), 'x');
  size_t total = 0;
  for (auto &kv : m) total += kv.second.size();

  std::vector<std::unique_ptr<std::vector<double>>> handoff;
  std::mutex mu;
  std::thread producer([&] {
    for (int i = 0; i < 20000; i++) {
      auto p = std::make_unique<std::vector<double>>(static_cast<size_t>(i % 300), 1.5);
      std::lock_guard<std::mutex> g(mu);
      handoff.push_back(std::move(p));
    }
  });
  producer.join();
  std::thread consumer([&] { handoff.clear(); }); // deleted on another thread
  consumer.join();

  auto sp = std::make_shared<std::string>(1000, 'y');
  std::thread t([sp] { (void)sp->size(); });
  t.join();
  bool ok = total == 20000 && u.size() == 50000 && handoff.empty();
  std::printf("smoke_cxx: %s\n", ok ? "ok" : "FAILED");
  return ok ? 0 : 1;
}
