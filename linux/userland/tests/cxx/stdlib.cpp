// libc++ on KlaussCPU Linux (CLANG_ON_BOARD_PLAN.md M2): containers,
// strings, threads, iostreams, std::filesystem on simple paths, and a few
// heavier headers.  Prints PASS/FAIL lines and "DONE stdlib failures=N";
// exit status = failures.  Uses /tmp (run.sh's initramfs has it writable).
#include <algorithm>
#include <atomic>
#include <bitset>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

static int failures;
static void check(bool ok, const char *name, int line) {
  if (ok)
    std::printf("PASS %s\n", name);
  else {
    std::printf("FAIL %s (line %d)\n", name, line);
    ++failures;
  }
}
#define CHECK(c, name) check((c), (name), __LINE__)

namespace fs = std::filesystem;

int main() {
  // Strings.
  std::string s = "hello";
  s += ", world";
  s.insert(0, ">> ");
  CHECK(s == ">> hello, world" && s.find("world") == 10 && s.substr(3, 5) == "hello",
        "string");
  std::string longs(1000, 'x');
  longs.replace(500, 1, "y");
  CHECK(longs.size() == 1000 && longs[500] == 'y' && longs.rfind('y') == 500, "string-long");
  CHECK(std::to_string(-1234567890123LL) == "-1234567890123" && std::stoi("  42") == 42 &&
            std::stod("2.5") == 2.5,
        "string-convert");

  // Containers and algorithms.
  std::vector<int> v(1000);
  std::iota(v.begin(), v.end(), 0);
  std::mt19937 rng(12345);
  std::shuffle(v.begin(), v.end(), rng);
  std::sort(v.begin(), v.end());
  bool sorted = true;
  for (int i = 0; i < 1000; ++i)
    sorted &= v[i] == i;
  CHECK(sorted && std::accumulate(v.begin(), v.end(), 0LL) == 499500, "vector-sort");
  std::map<std::string, int> m;
  for (int i = 0; i < 100; ++i)
    m["k" + std::to_string(i)] = i * i;
  CHECK(m.size() == 100 && m["k7"] == 49 && m.begin()->first == "k0", "map");
  std::unordered_map<int, std::string> um;
  for (int i = 0; i < 500; ++i)
    um[i * 7] = std::to_string(i);
  CHECK(um.size() == 500 && um[70] == "10" && !um.count(3), "unordered-map");
  std::set<double> ds{3.5, -1.25, 2.0};
  CHECK(*ds.begin() == -1.25 && *ds.rbegin() == 3.5, "set-double");
  std::bitset<40> bs(0xF0F0F0F0F0ULL);
  CHECK(bs.count() == 20 && bs.test(4) && !bs.test(0), "bitset");

  // Smart pointers, std::function, variant, optional.
  auto sp = std::make_shared<std::vector<int>>(3, 9);
  std::weak_ptr<std::vector<int>> wp = sp;
  std::function<int(int)> f = [sp](int x) { return x + (*sp)[2]; };
  CHECK(f(1) == 10 && wp.use_count() == 2, "shared-function");
  sp.reset();
  f = nullptr;
  CHECK(wp.expired(), "weak-expired");
  std::variant<int, std::string> var = std::string("v");
  CHECK(std::holds_alternative<std::string>(var) && std::get<1>(var) == "v", "variant");
  std::optional<long> o;
  CHECK(!o && o.value_or(5) == 5, "optional");

  // Formatting and parsing.
  CHECK(std::format("{:>6}|{:#x}|{:.3f}", "ab", 255, 3.14159) == "    ab|0xff|3.142", "format");
  char buf[32];
  auto r = std::to_chars(buf, buf + sizeof buf, 1.0 / 3);
  CHECK(std::string(buf, r.ptr) == "0.3333333333333333", "to-chars-double");
  long parsed = 0;
  const char *num = "-98765";
  std::from_chars(num, num + 6, parsed);
  CHECK(parsed == -98765, "from-chars");

  // Regex (locale-based classification, the C/POSIX locale).
  std::regex re(R"((\w+)@(\w+)\.com)");
  std::smatch mm;
  std::string mail = "mail bob@example.com now";
  CHECK(std::regex_search(mail, mm, re) && mm[1] == "bob" && mm[2] == "example", "regex");

  // iostreams.
  std::ostringstream os;
  os << "n=" << 42 << ' ' << 1.5 << ' ' << std::hex << 255;
  CHECK(os.str() == "n=42 1.5 ff", "ostringstream");
  std::istringstream is("10 20 thirty");
  int a = 0, b = 0;
  std::string w;
  is >> a >> b >> w;
  CHECK(a == 10 && b == 20 && w == "thirty", "istringstream");
  std::cout << "INFO cout works " << 123 << std::endl;
  CHECK(std::cout.good(), "cout");

  // Threads, mutex, condition variable, atomics.
  std::mutex mu;
  long counter = 0;
  std::atomic<int> acount{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t)
    ts.emplace_back([&] {
      for (int i = 0; i < 1000; ++i) {
        std::lock_guard<std::mutex> g(mu);
        ++counter;
        acount.fetch_add(1);
      }
    });
  for (auto &t : ts)
    t.join();
  CHECK(counter == 4000 && acount == 4000, "thread-mutex");
  std::condition_variable cv;
  bool ready = false, seen = false;
  std::thread waiter([&] {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] { return ready; });
    seen = true;
  });
  {
    std::lock_guard<std::mutex> g(mu);
    ready = true;
  }
  cv.notify_one();
  waiter.join();
  CHECK(seen, "condition-variable");
  auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms >= 20, "sleep-steady-clock");

  // std::filesystem on simple paths, and fstreams.
  std::error_code ec;
  fs::path dir = "/tmp/cxx-fs-test";
  fs::remove_all(dir, ec);
  bool fsok = fs::create_directories(dir / "sub");
  {
    std::ofstream out(dir / "sub" / "a.txt");
    out << "line one\nline two\n";
  }
  {
    std::ofstream out(dir / "b.txt");
    out << "b";
  }
  std::ifstream in(dir / "sub" / "a.txt");
  std::string l1, l2;
  std::getline(in, l1);
  std::getline(in, l2);
  fsok &= l1 == "line one" && l2 == "line two";
  fsok &= fs::file_size(dir / "b.txt") == 1 && fs::is_directory(dir / "sub");
  int entries = 0;
  for (auto &e : fs::recursive_directory_iterator(dir)) {
    (void)e;
    ++entries;
  }
  fsok &= entries == 3;
  fs::rename(dir / "b.txt", dir / "c.txt");
  fsok &= fs::exists(dir / "c.txt") && !fs::exists(dir / "b.txt");
  fsok &= fs::path("/a/b/c.tar.gz").extension() == ".gz" &&
          fs::path("/a/b/c.tar.gz").parent_path() == "/a/b";
  fsok &= fs::remove_all(dir) == 4 && !fs::exists(dir);
  CHECK(fsok, "filesystem");
  try {
    fs::file_size("/tmp/does-not-exist-cxx");
    CHECK(false, "filesystem-error");
  } catch (const fs::filesystem_error &e) {
    CHECK(e.code() == std::errc::no_such_file_or_directory, "filesystem-error");
  }

  std::printf("DONE stdlib failures=%d\n", failures);
  return failures;
}
