fs::path traffic_marker(const std::string& id) { return fs::path(kAppsRoot) / id / "traffic-enabled"; }

std::string traffic_format() {
  std::string config = "# Managed by Across: JSON access logs and trusted Cloudflare peers\nlog_format across_json escape=json '{\"time\":\"$msec\",\"timestamp\":\"$time_iso8601\",\"ip\":\"$remote_addr\",\"peer\":\"$realip_remote_addr\",\"host\":\"$host\",\"method\":\"$request_method\",\"path\":\"$uri\",\"status\":$status,\"bytes\":$body_bytes_sent,\"duration\":\"$request_time\",\"upstream\":\"$upstream_response_time\",\"agent\":\"$http_user_agent\",\"tls\":\"$ssl_protocol\"}';\n";
  for (const auto& range : {"173.245.48.0/20", "103.21.244.0/22", "103.22.200.0/22", "103.31.4.0/22", "141.101.64.0/18", "108.162.192.0/18", "190.93.240.0/20", "188.114.96.0/20", "197.234.240.0/22", "198.41.128.0/17", "162.158.0.0/15", "104.16.0.0/13", "104.24.0.0/14", "172.64.0.0/13", "131.0.72.0/22", "2400:cb00::/32", "2606:4700::/32", "2803:f800::/32", "2405:b500::/32", "2405:8100::/32", "2a06:98c0::/29", "2c0f:f248::/32"}) config += "set_real_ip_from " + std::string(range) + ";\n";
  config += "real_ip_header CF-Connecting-IP;\nreal_ip_recursive on;\n";
  return config;
}

int traffic_logging(int argc, char** argv) {
  require_root(); const auto id = managed_app_id(argc, argv); AppLock app_lock(id); AppLock nginx_lock(".nginx-global");
  const auto action = option_value(argc, argv, "--action");
  if (action != "enable" && action != "disable") throw std::runtime_error("Select enable or disable logging");
  const auto manifest = across::load_manifest(fs::path(kAppsRoot) / id / "manifest.json");
  const auto global = fs::path(kNginxRoot) / "00-across-logging.conf", marker = traffic_marker(id);
  std::optional<std::string> previous; if (fs::exists(global)) previous = read_file(global);
  const bool enabled = fs::exists(marker);
  try {
    if (action == "enable") { write_file_atomically(global, traffic_format()); write_file_atomically(marker, "enabled\n"); }
    else fs::remove(marker);
    install_nginx_config(manifest);
  } catch (...) {
    if (previous) write_file_atomically(global, *previous); else fs::remove(global);
    if (enabled) write_file_atomically(marker, "enabled\n"); else fs::remove(marker);
    throw;
  }
#ifndef ACROSS_SERVER_TESTING
  write_file_atomically("/etc/logrotate.d/across", "/var/log/nginx/across-*.access.jsonl {\n  daily\n  rotate 30\n  missingok\n  notifempty\n  compress\n  delaycompress\n  sharedscripts\n  postrotate\n    nginx -s reopen >/dev/null 2>&1 || true\n  endscript\n}\n");
#endif
  std::cout << nlohmann::json{{"ok", true}, {"app_id", id}, {"logging", action == "enable"}}.dump() << '\n';
  return 0;
}

struct TrafficAggregate {
  std::uint64_t requests{0}, bytes{0}, errors{0};
  double duration{0};
  std::set<std::string> visitors;
  std::vector<double> latencies;
  void add(const nlohmann::json& row) {
    const double latency = std::stod(row.value("duration", "0"));
    ++requests; bytes += row.value("bytes", std::uint64_t{0}); errors += row.value("status", 0) >= 400;
    duration += latency; visitors.insert(row.value("ip", "unknown")); latencies.push_back(latency);
  }
  nlohmann::json json() const {
    auto sorted = latencies; std::sort(sorted.begin(), sorted.end());
    return {{"requests", requests}, {"bytes", bytes}, {"errors", errors}, {"visitors", visitors.size()},
        {"avg_ms", requests ? duration * 1000 / static_cast<double>(requests) : 0}, {"p95_ms", sorted.empty() ? 0 : sorted[static_cast<std::size_t>(std::ceil(static_cast<double>(sorted.size()) * 0.95)) - 1] * 1000}};
  }
};

std::string device_description(const std::string& agent) {
  std::string os = agent.find("Android") != std::string::npos ? "Android" : agent.find("iPhone") != std::string::npos || agent.find("iPad") != std::string::npos ? "iOS" : agent.find("Windows") != std::string::npos ? "Windows" : agent.find("Macintosh") != std::string::npos ? "macOS" : agent.find("Linux") != std::string::npos ? "Linux" : "Unknown OS";
  const auto browser = agent.find("Edg/") != std::string::npos ? "Edge" : agent.find("Firefox/") != std::string::npos ? "Firefox" : agent.find("Chrome/") != std::string::npos ? "Chrome" : agent.find("Safari/") != std::string::npos ? "Safari" : "Other client";
  return os + " / " + browser;
}

nlohmann::json host_monitoring() {
  nlohmann::json result{{"timestamp", std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()}};
  if (fs::exists("/proc/stat")) {
    std::istringstream input(read_file("/proc/stat")); std::string label; std::uint64_t value, total = 0, idle = 0; int index = 0; input >> label;
    while (index < 8 && input >> value) { total += value; if (index == 3 || index == 4) idle += value; ++index; }
    result["cpu_total"] = total; result["cpu_idle"] = idle;
  }
  if (fs::exists("/proc/loadavg")) result["loadavg"] = read_file("/proc/loadavg");
  if (fs::exists("/proc/meminfo")) {
    std::istringstream input(read_file("/proc/meminfo")); std::string line;
    while (std::getline(input, line)) { std::istringstream row(line); std::string name; std::uint64_t value = 0; row >> name >> value; if (name == "MemTotal:") result["memory_total"] = value * 1024; if (name == "MemAvailable:") result["memory_available"] = value * 1024; }
  }
  const auto disk = fs::space("/"); result["disk_total"] = disk.capacity; result["disk_free"] = disk.free;
  result["interfaces"] = nlohmann::json::array(); std::uint64_t rx = 0, tx = 0;
  if (fs::exists("/proc/net/dev")) {
    std::istringstream input(read_file("/proc/net/dev")); std::string line;
    while (std::getline(input, line)) {
      const auto colon = line.find(':'); if (colon == std::string::npos) continue;
      std::string name = line.substr(0, colon); name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char character) { return std::isspace(character); }), name.end());
      std::istringstream row(line.substr(colon + 1)); std::uint64_t incoming = 0, outgoing = 0, ignored = 0; row >> incoming;
      for (int index = 0; index < 7; ++index) row >> ignored;
      row >> outgoing;
      result["interfaces"].push_back({{"name", name}, {"rx_bytes", incoming}, {"tx_bytes", outgoing}});
      if (name != "lo") { rx += incoming; tx += outgoing; }
    }
  }
  result["rx_bytes"] = rx; result["tx_bytes"] = tx;
  result["connections"] = command_reading({"ss", "-s"});
  return result;
}

nlohmann::json traffic_report(const std::vector<std::string>& apps, int hours, const fs::path& logs) {
  const auto now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  TrafficAggregate total; std::map<std::string, TrafficAggregate> projects, pages, visitors, devices;
  std::map<int, std::uint64_t> statuses; std::map<long long, std::uint64_t> buckets;
  std::vector<nlohmann::json> recent; std::size_t invalid = 0; bool partial = false;
  nlohmann::json logging = nlohmann::json::object();
  for (const auto& id : apps) {
    logging[id] = fs::exists(traffic_marker(id));
    for (const auto& suffix : {".1", ""}) {
      const auto path = logs / ("across-" + id + ".access.jsonl" + suffix);
      if (!fs::is_regular_file(path)) continue;
      std::ifstream file(path, std::ios::binary); const auto size = fs::file_size(path);
      constexpr std::uint64_t cap = 8 * 1024 * 1024;
      if (size > cap) { partial = true; file.seekg(static_cast<std::streamoff>(size - cap)); std::string discarded; std::getline(file, discarded); }
      std::string line;
      while (std::getline(file, line)) {
        try {
          auto row = nlohmann::json::parse(line); const auto time = std::stod(row.at("time").get<std::string>());
          if (!std::isfinite(time)) throw std::runtime_error("Invalid timestamp");
          if (time < now - hours * 3600 || time > now + 60) continue;
          const auto duration = std::stod(row.value("duration", "0"));
          if (!std::isfinite(duration) || duration < 0 || !row["status"].is_number_integer() || row["status"].get<int>() < 100 || row["status"].get<int>() > 599 || !row["bytes"].is_number_unsigned()) throw std::runtime_error("Invalid access log reading");
          row["app_id"] = id; row["device"] = device_description(row.value("agent", ""));
          total.add(row); projects[id].add(row); pages[id + " " + row.value("path", "/")].add(row); visitors[row.value("ip", "unknown")].add(row); devices[row["device"].get<std::string>()].add(row);
          ++statuses[row.value("status", 0)]; ++buckets[static_cast<long long>(time / 60) * 60];
          recent.push_back(std::move(row)); if (recent.size() > 500) recent.erase(std::min_element(recent.begin(), recent.end(), [](const auto& left, const auto& right) { return std::stod(left["time"].template get<std::string>()) < std::stod(right["time"].template get<std::string>()); }));
        } catch (...) { ++invalid; }
      }
    }
  }
  auto encode = [](const auto& entries) {
    std::vector<nlohmann::json> rows;
    for (const auto& [name, stats] : entries) { auto row = stats.json(); row["name"] = name; rows.push_back(row); }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) { return left["requests"].template get<std::uint64_t>() > right["requests"].template get<std::uint64_t>(); });
    if (rows.size() > 500) rows.resize(500);
    return rows;
  };
  std::sort(recent.begin(), recent.end(), [](const auto& left, const auto& right) { return std::stod(left["time"].template get<std::string>()) > std::stod(right["time"].template get<std::string>()); });
  if (recent.size() > 100) recent.resize(100);
  nlohmann::json series = nlohmann::json::array(); if (!buckets.empty()) for (auto time = buckets.begin()->first; time <= buckets.rbegin()->first; time += 60) series.push_back({{"time", time}, {"requests", buckets.count(time) ? buckets.at(time) : 0}});
  nlohmann::json codes = nlohmann::json::array(); for (const auto& [status, count] : statuses) codes.push_back({{"status", status}, {"requests", count}});
  return {{"ok", true}, {"hours", hours}, {"summary", total.json()}, {"projects", encode(projects)}, {"pages", encode(pages)}, {"visitors", encode(visitors)}, {"devices", encode(devices)}, {"statuses", codes}, {"series", series}, {"recent", recent}, {"invalid_lines", invalid}, {"partial", partial}, {"logging", logging}, {"scope", "Current and previous daily access logs, up to 8 MiB per file; traffic starts when logging is enabled"}};
}

int monitoring(int argc, char** argv) {
  require_root(); const auto id = option_value(argc, argv, "--app");
  const int hours = bounded_number(option_value(argc, argv, "--hours"), 1, 48);
  std::vector<std::string> apps;
  if (id == "all") { for (const auto& entry : fs::directory_iterator(kAppsRoot)) if (fs::is_regular_file(entry.path() / "manifest.json")) apps.push_back(entry.path().filename().string()); }
  else { managed_app_id(argc, argv); if (!fs::is_regular_file(fs::path(kAppsRoot) / id / "manifest.json")) throw std::runtime_error("Unknown website"); apps.push_back(id); }
  auto report = traffic_report(apps, hours, "/var/log/nginx"); report["host"] = host_monitoring();
  std::cout << report.dump() << '\n'; return 0;
}
