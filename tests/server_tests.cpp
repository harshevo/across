#include <filesystem>
#include <unistd.h>
const std::filesystem::path server_test_root = std::filesystem::temp_directory_path() / ("across-server-tests-" + std::to_string(getpid()));
#define ACROSS_SERVER_TESTING
#define main across_server_entrypoint
#include "../src/server/main.cpp"
#undef main

int main() {
  int failures = 0;
  const auto check = [&](bool condition, const std::string& label) { if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; } };
  const auto rejected = [&](auto function, const std::string& label) { try { function(); check(false, label); } catch (const std::exception&) {} };
  const auto invoke = [](auto function, std::vector<std::string> arguments) {
    std::vector<char*> argv; for (auto& argument : arguments) argv.push_back(argument.data());
    return function(static_cast<int>(argv.size()), argv.data());
  };
  fs::remove_all(server_test_root);
  fs::create_directories(server_test_root / "bin");
  fs::create_directories(fs::path(kAppsRoot) / "sample/current");
  fs::create_directories(kNginxRoot);
  const auto config = nginx_config_path("sample");
  for (const auto& command : {"nginx", "systemctl", "ufw"}) {
    const auto path = server_test_root / "bin" / command;
    std::ofstream output(path); output << "#!/bin/sh\n";
    if (std::string(command) == "nginx") output << "case \"$(cat " << across::shell_quote(config.string()) << ")\" in *reject*) printf rejected; exit 1;; esac\n";
    if (std::string(command) == "systemctl") output << "if [ \"$1\" = reload ] && [ -f " << across::shell_quote((server_test_root / "fail-reload").string()) << " ]; then rm " << across::shell_quote((server_test_root / "fail-reload").string()) << "; exit 1; fi\n";
    output << "exit 0\n"; output.close(); fs::permissions(path, fs::perms::owner_all);
  }
  const auto apt = server_test_root / "bin/apt-get";
  std::ofstream(apt) << "#!/bin/sh\nprintf '%s\n' \"$@\" >> " << across::shell_quote((server_test_root / "apt-log").string()) << "\nif [ -f " << across::shell_quote((server_test_root / "apt-failure").string()) << " ]; then exit 100; fi\n";
  fs::permissions(apt, fs::perms::owner_all);
  std::ofstream(server_test_root / "os-release") << "ID=ubuntu\nPRETTY_NAME=\"Ubuntu 24.04\"\n";
  const std::string original_path = std::getenv("PATH");
  setenv("PATH", ((server_test_root / "bin").string() + ":" + original_path).c_str(), 1);
  try {
    install_packages({"pm2", "docker", "htop"});
    const auto apt_log = read_file(server_test_root / "apt-log");
    check(apt_log.find("nodejs\nnpm\ndocker.io\nhtop") != std::string::npos, "apt receives mapped packages and PM2 dependencies");
    check(apt_log.find("nginx") == std::string::npos && apt_log.find("pm2") == std::string::npos, "deselected Nginx and npm-only PM2 do not enter apt command");
    std::ofstream(server_test_root / "os-release") << "ID=debian\nPRETTY_NAME=Debian\n";
    install_packages({"nginx"});
    check(read_file(server_test_root / "apt-log").find("nginx") != std::string::npos, "Debian package installation supported");
    std::ofstream(server_test_root / "os-release") << "ID=fedora\n";
    rejected([] { install_packages({"nginx"}); }, "unsupported OS blocks installation");
    std::ofstream(server_test_root / "os-release") << "ID=ubuntu\n";
    std::ofstream(server_test_root / "apt-failure") << "1";
    rejected([] { install_packages({"nginx"}); }, "failed apt command fails the operation");
    fs::remove(server_test_root / "apt-failure");
    rejected([&] { invoke(package_inventory, {"across-server", "packages", "--packages", "[\"--dangerous\"]"}); }, "unsafe package rejected before inventory commands");
    check(domain_matches("EXAMPLE.COM", "example.com"), "domain collision checks are case insensitive");
    ProjectManifest manifest;
    manifest.app_id = "sample"; manifest.type = "static"; manifest.project_root = fs::path(kAppsRoot) / "sample/current";
    manifest.nginx.domains = {"example.com"};
    const auto path = fs::path(kAppsRoot) / "sample/manifest.json";
    write_file_atomically(path, nlohmann::json(manifest).dump());
    write_file_atomically(config, "original config\n");
    invoke(website_settings, {"across-server", "website-settings", "--app", "sample", "--domains", "example.com www.example.com", "--body-mb", "40"});
    check(across::load_manifest(path).nginx.domains.size() == 2, "website domains persisted");
    check(read_file(config).find("client_max_body_size 40m") != std::string::npos, "Nginx settings rendered");
    auto redeployed = manifest; apply_website_overrides(redeployed);
    check(redeployed.nginx.domains.size() == 2 && redeployed.nginx.client_max_body_mb == 40, "website overrides survive redeploy");
    const auto previous_config = read_file(config), previous_manifest = read_file(path);
    rejected([&] { invoke(edit_nginx, {"across-server", "nginx-save", "--app", "sample", "--content", "reject configuration"}); }, "invalid Nginx rejected");
    check(read_file(config) == previous_config, "invalid Nginx restores original file");
    std::ofstream(server_test_root / "fail-reload") << "1";
    rejected([&] { invoke(website_settings, {"across-server", "website-settings", "--app", "sample", "--domains", "changed.example.com"}); }, "failed reload rejected");
    check(read_file(config) == previous_config && read_file(path) == previous_manifest, "failed reload preserves Nginx and website state");
    rejected([&] { invoke(firewall, {"across-server", "firewall", "--action", "deny", "--port", "2222", "--protocol", "tcp", "--ssh-port", "2222"}); }, "active SSH port cannot be blocked");
    rejected([&] { invoke(inspect_website, {"across-server", "website", "--app", "../sample"}); }, "website traversal rejected");
    rejected([&] { invoke(remove_app, {"across-server", "remove", "--app", ""}); }, "empty app id cannot delete all websites");
    check(fs::exists(path), "empty removal leaves website intact");
    fs::create_directories(default_site_path().parent_path());
    write_file_atomically(default_site_path(), "welcome page");
    std::ofstream(server_test_root / "fail-reload") << "1";
    rejected([&] { invoke(nginx_access, {"across-server", "nginx-access", "--mode", "domain-only"}); }, "access reload failure rejected");
    check(!domain_only_enabled() && read_file(default_site_path()) == "welcome page", "failed access change restores default site");
    invoke(nginx_access, {"across-server", "nginx-access", "--mode", "domain-only"});
    check(domain_only_enabled() && !fs::exists(default_site_path()), "domain-only access disables default welcome site");
    check(read_file(domain_only_config()).find("ssl_reject_handshake on") != std::string::npos, "unknown HTTPS hostname rejected");
    invoke(nginx_access, {"across-server", "nginx-access", "--mode", "domain-only"});
    auto ip_manifest = manifest; ip_manifest.nginx.domains = {"127.0.0.1"};
    rejected([&] { install_nginx_config(ip_manifest); }, "future IP deployment cannot bypass domain-only access");
    invoke(nginx_access, {"across-server", "nginx-access", "--mode", "restore"});
    check(!domain_only_enabled() && read_file(default_site_path()) == "welcome page", "default site restored without losing contents");
    check(valid_ip("203.0.113.1") && valid_ip("2001:db8::1") && !valid_ip("--help") && !valid_ip("1.2.3.999"), "IP controls accept only literal IPv4 and IPv6");
    rejected([&] { invoke(security_action, {"across-server", "security-action", "--action", "kill-process", "--pid", "1", "--start", "0", "--signal", "KILL"}); }, "PID 1 protected");
    rejected([&] { invoke(security_action, {"across-server", "security-action", "--action", "ban-ip", "--ip", "--help"}); }, "IP command injection rejected");
    rejected([&] { invoke(security_action, {"across-server", "security-action", "--action", "ping", "--mode", "invalid"}); }, "invalid ping mode rejected before changing settings");
    rejected([&] { invoke(monitoring, {"across-server", "monitoring", "--app", "../sample", "--hours", "24"}); }, "traffic project traversal rejected");
    rejected([&] { invoke(monitoring, {"across-server", "monitoring", "--app", "sample", "--hours", "49"}); }, "traffic time range bounded");
    const auto logs = server_test_root / "logs"; fs::create_directory(logs);
    const auto now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    nlohmann::json request{{"time", std::to_string(now - 5)}, {"timestamp", "now"}, {"ip", "203.0.113.1"}, {"path", "/"}, {"status", 200}, {"bytes", 100}, {"duration", "0.01"}, {"agent", "Mozilla Macintosh Safari/605"}};
    std::ofstream access(logs / "across-sample.access.jsonl"); access << request.dump() << '\n';
    request["time"] = std::to_string(now); request["path"] = "/missing"; request["status"] = 404; request["duration"] = "0.10"; access << request.dump() << '\n';
    request["time"] = std::to_string(now - 90000); access << request.dump() << '\n' << "invalid log line\n"; access.close();
    const auto traffic = traffic_report({"sample"}, 24, logs);
    check(traffic["summary"]["requests"] == 2 && traffic["summary"]["visitors"] == 1 && traffic["summary"]["errors"] == 1 && traffic["summary"]["bytes"] == 200, "traffic aggregates real requests, unique IPs, errors, and bytes in range");
    check(traffic["pages"].size() == 2 && traffic["invalid_lines"] == 1 && traffic["recent"].size() == 2, "per-page and recent request reports exclude old and malformed logs");
    check(traffic["summary"]["p95_ms"] == 100.0, "p95 latency calculated from samples");
    check(traffic["pages"][0]["name"] == "sample /missing" && traffic["recent"][0]["path"] == "/missing", "latest traffic sorts first in grouped and request reports");
    check(device_description("Mozilla Macintosh Safari/605") == "macOS / Safari", "device details are inferred from user agent");
    invoke(traffic_logging, {"across-server", "traffic-logging", "--app", "sample", "--action", "enable"});
    check(read_file(config).find("access_log /var/log/nginx/across-sample.access.jsonl across_json") != std::string::npos, "website logging persisted in Nginx");
    check(traffic_format().find("set_real_ip_from 173.245.48.0/20") != std::string::npos && traffic_format().find("set_real_ip_from 0.0.0.0") == std::string::npos, "visitor header trusted only for published Cloudflare peers");
    invoke(traffic_logging, {"across-server", "traffic-logging", "--app", "sample", "--action", "disable"});
    check(!fs::exists(traffic_marker("sample")), "website logging can be disabled");
    AppLock first("sample"); rejected([] { AppLock second("sample"); }, "parallel app mutation rejected");
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
  setenv("PATH", original_path.c_str(), 1);
  fs::remove_all(server_test_root);
  return failures == 0 ? 0 : 1;
}
