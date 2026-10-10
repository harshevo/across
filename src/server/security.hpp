bool valid_ip(const std::string& value) {
  in6_addr address{};
  return inet_pton(AF_INET, value.c_str(), &address) == 1 ||
         inet_pton(AF_INET6, value.c_str(), &address) == 1;
}

int bounded_number(const std::string& value, int minimum, int maximum) {
  if (!std::regex_match(value, std::regex("^[0-9]{1,9}$"))) throw std::runtime_error("Enter a valid number");
  const int number = std::stoi(value);
  if (number < minimum || number > maximum) throw std::runtime_error("Number must be between " + std::to_string(minimum) + " and " + std::to_string(maximum));
  return number;
}

std::string administrator_ip() {
  std::istringstream connection(std::getenv("SSH_CONNECTION") ? std::getenv("SSH_CONNECTION") : "");
  std::string ip; connection >> ip;
  return valid_ip(ip) ? ip : "";
}

nlohmann::json command_reading(const std::vector<std::string>& arguments) {
  const auto result = across::run_process(arguments, std::chrono::seconds{15});
  return {{"available", result.ok()}, {"output", result.output}};
}

std::pair<int, std::string> process_identity(int pid) {
  const auto stat = read_file(fs::path("/proc") / std::to_string(pid) / "stat");
  const auto closing = stat.rfind(')');
  if (closing == std::string::npos) throw std::runtime_error("Cannot inspect process identity");
  std::istringstream fields(stat.substr(closing + 2));
  std::string state, start, ignored; int parent = 0;
  fields >> state >> parent;
  for (int index = 5; index <= 21; ++index) fields >> ignored;
  fields >> start;
  if (start.empty()) throw std::runtime_error("Cannot inspect process start time");
  return {parent, start};
}

nlohmann::json security_snapshot() {
  nlohmann::json result{{"ok", true}, {"administrator_ip", administrator_ip()}, {"domain_only", domain_only_enabled()}};
  for (const auto& [key, arguments] : std::vector<std::pair<std::string, std::vector<std::string>>>{
      {"sessions", {"who", "-u"}}, {"login_history", {"last", "-i", "-n", "30"}},
      {"ssh_activity", {"journalctl", "-u", "ssh", "-u", "sshd", "--since", "24 hours ago", "-n", "100", "--no-pager", "-o", "short-iso"}},
      {"connections", {"ss", "-tnp"}}, {"ports", {"ss", "-lntup"}},
      {"firewall", {"ufw", "status", "numbered"}},
      {"fail2ban", {"fail2ban-client", "status"}}, {"sshd_jail", {"fail2ban-client", "status", "sshd"}},
      {"ssh_config", {"sshd", "-T"}}, {"updates", {"apt", "list", "--upgradable"}}}) result[key] = command_reading(arguments);
  result["websites"] = nlohmann::json::array();
  if (fs::is_directory(kAppsRoot)) for (const auto& entry : fs::directory_iterator(kAppsRoot)) {
    const auto path = entry.path() / "manifest.json"; if (!fs::is_regular_file(path)) continue;
    try {
      const auto manifest = across::load_manifest(path);
      nlohmann::json site{{"app_id", manifest.app_id}, {"domains", manifest.nginx.domains}, {"https", !manifest.nginx.tls_certificate.empty()}, {"rate", manifest.nginx.rate_limit_rps}, {"burst", manifest.nginx.rate_limit_burst}, {"body_mb", manifest.nginx.client_max_body_mb}};
      if (!manifest.nginx.tls_certificate.empty()) site["certificate"] = command_reading({"openssl", "x509", "-in", manifest.nginx.tls_certificate, "-noout", "-dates", "-issuer", "-subject"});
      const auto blocked = entry.path() / "blocked-ips.json"; site["blocked_ips"] = fs::is_regular_file(blocked) ? nlohmann::json::parse(read_file(blocked)) : nlohmann::json::array();
      result["websites"].push_back(site);
    } catch (...) {}
  }
  result["users"] = nlohmann::json::array();
  const auto passwd = across::run_process({"getent", "passwd"}, std::chrono::seconds{10});
  std::istringstream lines(passwd.output); std::string line;
  while (std::getline(lines, line)) {
    std::vector<std::string> columns; std::istringstream row(line); std::string column;
    while (std::getline(row, column, ':')) columns.push_back(column);
    if (columns.size() == 7) result["users"].push_back({{"name", columns[0]}, {"uid", columns[2]}, {"gid", columns[3]}, {"home", columns[5]}, {"shell", columns[6]}});
  }
  result["processes"] = nlohmann::json::array();
  const auto processes = across::run_process({"ps", "-eo", "pid=,ppid=,user=,pcpu=,pmem=,etime=,comm=", "--sort=-pcpu"}, std::chrono::seconds{10});
  std::istringstream process_lines(processes.output);
  while (std::getline(process_lines, line)) {
    std::istringstream row(line); int pid = 0, parent = 0; std::string user, cpu, memory, elapsed, name;
    if (!(row >> pid >> parent >> user >> cpu >> memory >> elapsed >> name)) continue;
    try { result["processes"].push_back({{"pid", pid}, {"parent", parent}, {"user", user}, {"cpu", cpu}, {"memory", memory}, {"elapsed", elapsed}, {"name", name}, {"start", process_identity(pid).second}}); } catch (...) {}
  }
  result["ping_v4"] = fs::is_regular_file("/proc/sys/net/ipv4/icmp_echo_ignore_all") ? read_file("/proc/sys/net/ipv4/icmp_echo_ignore_all").find('1') == std::string::npos : true;
  result["ping_v6_available"] = fs::is_regular_file("/proc/sys/net/ipv6/icmp/echo_ignore_all");
  result["ping_v6"] = result["ping_v6_available"].get<bool>() ? read_file("/proc/sys/net/ipv6/icmp/echo_ignore_all").find('1') == std::string::npos : true;
  const auto settings = fs::path(kStateRoot) / "security-settings.json";
  result["settings"] = fs::is_regular_file(settings) ? nlohmann::json::parse(read_file(settings)) : nlohmann::json{{"maxretry", 5}, {"findtime", 600}, {"bantime", 3600}};
  return result;
}

int security_status() { require_root(); std::cout << security_snapshot().dump() << '\n'; return 0; }

int security_action(int argc, char** argv) {
  require_root();
  const auto action = option_value(argc, argv, "--action");
  AppLock lock(".security");
  if (action == "kill-process") {
    const int pid = bounded_number(option_value(argc, argv, "--pid"), 2, 4194304);
    const auto expected_start = option_value(argc, argv, "--start");
    const auto signal = option_value(argc, argv, "--signal");
    if (signal != "TERM" && signal != "KILL") throw std::runtime_error("Select TERM or KILL");
    for (int ancestor = getpid(); ancestor > 1;) {
      if (pid == ancestor) throw std::runtime_error("Cannot terminate the administrator SSH connection or this operation");
      ancestor = process_identity(ancestor).first;
    }
    if (process_identity(pid).second != expected_start) throw std::runtime_error("Process changed; refresh before terminating it");
    const auto name = read_file(fs::path("/proc") / std::to_string(pid) / "comm");
    if (name.find("sshd") != std::string::npos || name.find("systemd") != std::string::npos) throw std::runtime_error("SSH and system manager processes are protected");
    if (::kill(pid, signal == "TERM" ? SIGTERM : SIGKILL) != 0) throw std::runtime_error("Cannot signal process");
  } else if (action == "restart-website") {
    const auto id = managed_app_id(argc, argv); AppLock app_lock(id);
    const auto manifest = across::load_manifest(fs::path(kAppsRoot) / id / "manifest.json");
    if (manifest.type == "node") ensure_ok(run_as_service("pm2 reload " + across::shell_quote("across-" + id)), "Website restart");
    else ensure_ok(across::run_process({"systemctl", "reload", "nginx"}, std::chrono::seconds{30}), "Website reload");
    if (!health_check(manifest)) throw std::runtime_error("Website restarted but its health check failed");
  } else if (action == "install-fail2ban" || action == "fail2ban-limits") {
    const int retry = bounded_number(option_value(argc, argv, "--maxretry"), 1, 100);
    const int findtime = bounded_number(option_value(argc, argv, "--findtime"), 10, 604800);
    const int bantime = bounded_number(option_value(argc, argv, "--bantime"), 10, 31536000);
    const int port = bounded_number(option_value(argc, argv, "--ssh-port"), 1, 65535);
    const fs::path config = "/etc/fail2ban/jail.d/across.local";
    std::optional<std::string> previous; if (fs::is_regular_file(config)) previous = read_file(config);
    std::string ignored = "127.0.0.1/8 ::1";
    const auto settings_path = fs::path(kStateRoot) / "security-settings.json";
    auto settings = fs::is_regular_file(settings_path) ? nlohmann::json::parse(read_file(settings_path)) : nlohmann::json::object();
    auto trusted = settings.value("trusted_administrators", std::vector<std::string>{});
    const auto current = administrator_ip();
    if (current.empty()) throw std::runtime_error("Configure Fail2ban through SSH so administrator access can be preserved");
    if (std::find(trusted.begin(), trusted.end(), current) == trusted.end()) trusted.push_back(current);
    for (const auto& ip : trusted) { if (!valid_ip(ip)) throw std::runtime_error("Invalid trusted administrator IP"); ignored += " " + ip; }
    if (action == "install-fail2ban") ensure_ok(across::run_process({"env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "install", "-y", "fail2ban"}, std::chrono::minutes{5}), "Fail2ban installation");
    try {
      write_file_atomically(config, "[sshd]\nenabled = true\nbackend = systemd\nport = " + std::to_string(port) + "\nmaxretry = " + std::to_string(retry) + "\nfindtime = " + std::to_string(findtime) + "\nbantime = " + std::to_string(bantime) + "\nignoreip = " + ignored + "\n");
      ensure_ok(across::run_process({"fail2ban-client", "-t"}, std::chrono::seconds{30}), "Fail2ban validation");
      ensure_ok(across::run_process({"systemctl", "enable", "--now", "fail2ban"}, std::chrono::seconds{30}), "Fail2ban activation");
      for (int attempt = 0; attempt < 15; ++attempt) {
        if (across::run_process({"fail2ban-client", "ping"}, std::chrono::seconds{5}).ok()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
      }
      ensure_ok(across::run_process({"fail2ban-client", "reload"}, std::chrono::seconds{30}), "Fail2ban reload");
      settings["trusted_administrators"] = trusted; settings["maxretry"] = retry; settings["findtime"] = findtime; settings["bantime"] = bantime;
      write_file_atomically(settings_path, settings.dump());
    } catch (...) { if (previous) write_file_atomically(config, *previous); else fs::remove(config); across::run_process({"fail2ban-client", "reload"}, std::chrono::seconds{30}); throw; }
  } else if (action == "website-block-ip" || action == "website-unblock-ip") {
    const auto ip = option_value(argc, argv, "--ip");
    if (!valid_ip(ip) || ip == administrator_ip() || ip == "127.0.0.1" || ip == "::1") throw std::runtime_error("Enter an IP other than the administrator or loopback");
    const auto id = managed_app_id(argc, argv); AppLock app_lock(id); AppLock nginx_lock(".nginx-global");
    const auto path = fs::path(kAppsRoot) / id / "blocked-ips.json";
    std::optional<std::string> previous; if (fs::is_regular_file(path)) previous = read_file(path);
    auto ips = previous ? nlohmann::json::parse(*previous).get<std::vector<std::string>>() : std::vector<std::string>{};
    ips.erase(std::remove(ips.begin(), ips.end(), ip), ips.end());
    if (action == "website-block-ip") ips.push_back(ip);
    try { write_file_atomically(path, nlohmann::json(ips).dump()); install_nginx_config(across::load_manifest(fs::path(kAppsRoot) / id / "manifest.json")); }
    catch (...) { if (previous) write_file_atomically(path, *previous); else fs::remove(path); throw; }
  } else if (action == "ban-ip" || action == "unban-ip" || action == "block-ip" || action == "unblock-ip") {
    const auto ip = option_value(argc, argv, "--ip");
    if (!valid_ip(ip)) throw std::runtime_error("Enter one IPv4 or IPv6 address");
    if (ip == administrator_ip() || ip == "127.0.0.1" || ip == "::1") throw std::runtime_error("Administrator and loopback IPs are protected");
    if (action == "ban-ip" || action == "unban-ip") ensure_ok(across::run_process({"fail2ban-client", "set", "sshd", action == "ban-ip" ? "banip" : "unbanip", ip}, std::chrono::seconds{30}), "Fail2ban IP action");
    else if (action == "block-ip") ensure_ok(across::run_process({"ufw", "prepend", "deny", "from", ip}, std::chrono::seconds{30}), "Block IP");
    else ensure_ok(across::run_process({"ufw", "--force", "delete", "deny", "from", ip}, std::chrono::seconds{30}), "Unblock IP");
  } else if (action == "ping") {
    const auto mode = option_value(argc, argv, "--mode");
    if (mode != "block" && mode != "allow") throw std::runtime_error("Select block or allow");
    const std::string value = mode == "block" ? "1" : "0";
    const fs::path path = "/etc/sysctl.d/90-across-ping.conf";
    std::optional<std::string> previous; if (fs::is_regular_file(path)) previous = read_file(path);
    const auto old4 = across::run_process({"sysctl", "-n", "net.ipv4.icmp_echo_ignore_all"}).output;
    const bool ipv6 = fs::exists("/proc/sys/net/ipv6/icmp/echo_ignore_all");
    const auto old6 = ipv6 ? across::run_process({"sysctl", "-n", "net.ipv6.icmp.echo_ignore_all"}).output : "";
    try {
      write_file_atomically(path, "net.ipv4.icmp_echo_ignore_all=" + value + "\n" + (ipv6 ? "net.ipv6.icmp.echo_ignore_all=" + value + "\n" : ""));
      ensure_ok(across::run_process({"sysctl", "-p", path.string()}, std::chrono::seconds{15}), "Ping setting");
    } catch (...) {
      if (previous) write_file_atomically(path, *previous); else fs::remove(path);
      across::run_process({"sysctl", "-w", std::string{"net.ipv4.icmp_echo_ignore_all="} + (old4.find('1') == std::string::npos ? "0" : "1")});
      if (ipv6) across::run_process({"sysctl", "-w", std::string{"net.ipv6.icmp.echo_ignore_all="} + (old6.find('1') == std::string::npos ? "0" : "1")});
      throw;
    }
  } else if (action == "ssh-keys-only") {
    const fs::path path = "/etc/ssh/sshd_config.d/00-across-keys-only.conf";
    std::optional<std::string> previous; if (fs::is_regular_file(path)) previous = read_file(path);
    try {
      write_file_atomically(path, "PasswordAuthentication no\nKbdInteractiveAuthentication no\nPubkeyAuthentication yes\n");
      ensure_ok(across::run_process({"sshd", "-t"}, std::chrono::seconds{15}), "SSH validation");
      ensure_ok(across::run_process({"systemctl", "reload", "ssh"}, std::chrono::seconds{30}), "SSH reload");
    } catch (...) { if (previous) write_file_atomically(path, *previous); else fs::remove(path); throw; }
  } else throw std::runtime_error("Unknown security action");
  std::cout << nlohmann::json{{"ok", true}, {"action", action}}.dump() << '\n';
  return 0;
}
