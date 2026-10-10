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
    invoke(nginx_access, {"across-server", "nginx-access", "--mode", "restore"});
    check(!domain_only_enabled() && read_file(default_site_path()) == "welcome page", "default site restored without losing contents");
    AppLock first("sample"); rejected([] { AppLock second("sample"); }, "parallel app mutation rejected");
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
  setenv("PATH", original_path.c_str(), 1);
  fs::remove_all(server_test_root);
  return failures == 0 ? 0 : 1;
}
