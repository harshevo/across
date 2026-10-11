#include <arpa/inet.h>
#include <csignal>
#include <cmath>
#include <map>
#include <set>
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "across/config.hpp"
#include "across/management.hpp"
#include "across/packages.hpp"
#include "across/process.hpp"

namespace fs = std::filesystem;
using across::ProcessResult;
using across::ProjectManifest;

namespace {

#ifdef ACROSS_SERVER_TESTING
const std::string kStateRoot = server_test_root.string();
const std::string kAppsRoot = (server_test_root / "apps").string();
const std::string kLockRoot = (server_test_root / "locks").string();
const std::string kNginxRoot = (server_test_root / "nginx").string();
#else
constexpr const char* kStateRoot = "/var/lib/across";
constexpr const char* kAppsRoot = "/var/lib/across/apps";
constexpr const char* kLockRoot = "/var/lock/across";
constexpr const char* kNginxRoot = "/etc/nginx/conf.d";
#endif
constexpr const char* kServiceUser = "across";

class AppLock {
 public:
  explicit AppLock(const std::string& app_id) {
    fs::create_directories(kLockRoot);
    path_ = fs::path(kLockRoot) / (app_id + ".lock");
    descriptor_ = open(path_.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor_ < 0 || flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
      if (descriptor_ >= 0) {
        close(descriptor_);
      }
      throw std::runtime_error("Another operation is running for " + app_id);
    }
  }

  AppLock(const AppLock&) = delete;
  AppLock& operator=(const AppLock&) = delete;

  ~AppLock() {
    if (descriptor_ >= 0) {
      flock(descriptor_, LOCK_UN);
      close(descriptor_);
    }
  }

 private:
  fs::path path_;
  int descriptor_{-1};
};

void require_root() {
#ifdef ACROSS_SERVER_TESTING
  return;
#endif
  if (geteuid() != 0) {
    throw std::runtime_error("This command must run as root (use sudo)");
  }
}

void progress(const std::string& message) {
  std::cout << message << std::endl;
}

void ensure_ok(const ProcessResult& result, const std::string& action) {
  if (!result.ok()) {
    throw std::runtime_error(action + " failed (exit " +
                             std::to_string(result.exit_code) + "):\n" +
                             result.output);
  }
}

ProcessResult run(const std::vector<std::string>& arguments,
                  std::chrono::seconds timeout = std::chrono::minutes{10}) {
  return across::run_process(arguments, timeout, [](const std::string& chunk) {
    std::cout << chunk << std::flush;
  });
}

std::string read_file(const fs::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot read " + path.string());
  }
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void write_file_atomically(const fs::path& path, const std::string& contents,
                           fs::perms permissions =
                               fs::perms::owner_read | fs::perms::owner_write |
                               fs::perms::group_read | fs::perms::others_read) {
  fs::create_directories(path.parent_path());
  const fs::path temporary = path.string() + ".tmp." + std::to_string(getpid());
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
      throw std::runtime_error("Cannot write " + temporary.string());
    }
    output << contents;
    output.flush();
    if (!output) {
      throw std::runtime_error("Failed writing " + temporary.string());
    }
  }
  fs::permissions(temporary, permissions, fs::perm_options::replace);
  fs::rename(temporary, path);
}

std::string option_value(int argc, char** argv, const std::string& option,
                         bool required = true) {
  for (int index = 2; index + 1 < argc; ++index) {
    if (argv[index] == option) {
      return argv[index + 1];
    }
  }
  if (required) {
    throw std::runtime_error("Missing required option: " + option);
  }
  return {};
}

bool has_option(int argc, char** argv, const std::string& option) {
  for (int index = 1; index < argc; ++index) {
    if (argv[index] == option) {
      return true;
    }
  }
  return false;
}

std::string executable(const std::string& name) {
  const auto result = across::run_process({"sh", "-c", "command -v " + name},
                                          std::chrono::seconds{5});
  if (!result.ok()) {
    return {};
  }
  std::string path = result.output;
  path.erase(path.find_last_not_of("\r\n") + 1);
  return path;
}

nlohmann::json server_os() {
#ifdef ACROSS_SERVER_TESTING
  const auto path = server_test_root / "os-release";
#else
  const fs::path path = "/etc/os-release";
#endif
  return across::parse_os_release(read_file(path));
}

void require_supported_os() {
  const auto os = server_os();
  if (!os.at("supported").get<bool>()) throw std::runtime_error("Packages support Ubuntu and Debian; detected " + os.at("name").get<std::string>());
  if (executable("apt-get").empty()) throw std::runtime_error("apt-get is unavailable on this VPS");
}

std::vector<std::string> selected_packages(int argc, char** argv) {
  const auto encoded = option_value(argc, argv, "--packages", false);
  auto selected = encoded.empty() ? std::vector<std::string>{"nginx", "pm2", "node", "npm"} : nlohmann::json::parse(encoded).get<std::vector<std::string>>();
  (void)across::package_plan(selected);
  return selected;
}

void install_packages(const std::vector<std::string>& selected) {
  require_supported_os();
  const auto plan = across::package_plan(selected);
  ensure_ok(run({"apt-get", "-o", "DPkg::Lock::Timeout=120", "update"}), "Package index refresh");
  std::vector<std::string> arguments{"env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "-o", "DPkg::Lock::Timeout=120", "install", "-y", "rsync", "curl", "ca-certificates"};
  arguments.insert(arguments.end(), plan.apt_packages.begin(), plan.apt_packages.end());
  ensure_ok(run(arguments, std::chrono::minutes{20}), "Ubuntu/Debian package installation");
}

void ensure_service_user() {
  if (getpwnam(kServiceUser) == nullptr) {
    ensure_ok(run({"useradd", "--system", "--user-group", "--home-dir", kStateRoot,
                   "--create-home", "--shell", "/usr/sbin/nologin", kServiceUser}),
              "service user creation");
  }
  fs::create_directories(kAppsRoot);
  fs::create_directories("/var/log/across");
  fs::create_directories(kLockRoot);
  ensure_ok(run({"chown", "-R", "across:across", kStateRoot, "/var/log/across"}),
            "state ownership");
  fs::permissions(kStateRoot,
                  fs::perms::owner_all | fs::perms::group_read |
                      fs::perms::group_exec | fs::perms::others_read |
                      fs::perms::others_exec,
                  fs::perm_options::replace);
  fs::permissions(kAppsRoot,
                  fs::perms::owner_all | fs::perms::group_read |
                      fs::perms::group_exec | fs::perms::others_read |
                      fs::perms::others_exec,
                  fs::perm_options::replace);
  fs::permissions(kLockRoot, fs::perms::owner_all,
                  fs::perm_options::replace);
}

void install_pm2_service() {
  auto pm2 = executable("pm2");
  if (pm2.empty()) {
    ensure_ok(run({"npm", "install", "--global", "--no-audit", "--no-fund", "pm2"},
                  std::chrono::minutes{10}),
              "PM2 installation");
    pm2 = executable("pm2");
  }
  if (pm2.empty()) {
    throw std::runtime_error("PM2 was installed but is not on PATH");
  }
  const std::string service =
      "[Unit]\n"
      "Description=Across managed PM2 applications\n"
      "After=network.target\n\n"
      "[Service]\n"
      "Type=forking\n"
      "User=across\n"
      "Environment=HOME=/var/lib/across\n"
      "Environment=USER=across\n"
      "Environment=LOGNAME=across\n"
      "Environment=PM2_HOME=/var/lib/across/.pm2\n"
      "Environment=PATH=/usr/local/bin:/usr/bin:/bin\n"
      "WorkingDirectory=/var/lib/across\n"
      "PIDFile=/var/lib/across/.pm2/pm2.pid\n"
      "ExecStart=" + pm2 + " resurrect\n"
      "ExecReload=" + pm2 + " reload all\n"
      "ExecStop=" + pm2 + " kill\n"
      "Restart=on-failure\n\n"
      "[Install]\n"
      "WantedBy=multi-user.target\n";
  write_file_atomically("/etc/systemd/system/across-pm2.service", service);
  ensure_ok(run({"systemctl", "daemon-reload"}), "systemd reload");
  ensure_ok(run({"systemctl", "enable", "across-pm2.service"}),
            "PM2 service enable");
  const auto active = across::run_process(
      {"systemctl", "is-active", "--quiet", "across-pm2.service"},
      std::chrono::seconds{10});
  if (!active.ok()) {
    if (fs::exists("/var/lib/across/.pm2/pm2.pid")) {
      run({"runuser", "-u", kServiceUser, "--", "env", "-i",
           "HOME=/var/lib/across", "USER=across", "LOGNAME=across",
           "PM2_HOME=/var/lib/across/.pm2", "PATH=/usr/local/bin:/usr/bin:/bin",
           pm2, "kill"},
          std::chrono::minutes{2});
    }
    if (!fs::exists("/var/lib/across/.pm2/dump.pm2")) {
      const std::string seed =
          "cd /var/lib/across && " + across::shell_quote(pm2) +
          " ping && " + across::shell_quote(pm2) + " save --force && " +
          across::shell_quote(pm2) + " kill";
      ensure_ok(run({"runuser", "-u", kServiceUser, "--", "env", "-i",
                     "HOME=/var/lib/across", "USER=across", "LOGNAME=across",
                     "PM2_HOME=/var/lib/across/.pm2",
                     "PATH=/usr/local/bin:/usr/bin:/bin", "/bin/sh", "-c", seed},
                    std::chrono::minutes{2}),
                "empty PM2 state initialization");
    }
    ensure_ok(run({"systemctl", "start", "across-pm2.service"},
                  std::chrono::minutes{2}),
              "PM2 service start");
  }
}

int provision(int argc, char** argv, bool json_output) {
  require_root();
  const auto selected = selected_packages(argc, argv);
  const auto plan = across::package_plan(selected);
  AppLock lock(".packages");
  progress("Installing selected packages...");
  install_packages(selected);
  ensure_service_user();
  if (plan.pm2) install_pm2_service();
  if (plan.nginx) {
    AppLock nginx_lock(".nginx-global");
    ensure_ok(run({"nginx", "-t"}), "Nginx validation");
    ensure_ok(run({"systemctl", "enable", "--now", "nginx"}), "Nginx enable");
  }
  if (plan.docker) ensure_ok(run({"systemctl", "enable", "--now", "docker"}), "Docker enable");
  write_file_atomically(fs::path(kStateRoot) / "package-selection.json", nlohmann::json(selected).dump(2));
  if (json_output) std::cout << nlohmann::json{{"ok", true}, {"os", server_os()}, {"selected", selected}, {"apt_packages", plan.apt_packages}, {"pm2", plan.pm2}, {"version", ACROSS_VERSION}}.dump(2) << '\n';
  return 0;
}

nlohmann::json command_check(const std::string& command,
                             const std::vector<std::string>& version_arguments) {
  const auto path = executable(command);
  if (path.empty()) {
    return {{"installed", false}};
  }
  auto arguments = version_arguments;
  arguments.insert(arguments.begin(), path);
  const auto result = across::run_process(arguments, std::chrono::seconds{10});
  std::string version = result.output;
  version.erase(version.find_last_not_of("\r\n") + 1);
  if (const auto newline = version.find('\n'); newline != std::string::npos) {
    version.resize(newline);
  }
  return {{"installed", true}, {"path", path}, {"version", version}};
}

nlohmann::json pm2_check() {
  const auto path = executable("pm2");
  if (path.empty()) return {{"installed", false}};
  const auto root = across::run_process({"npm", "root", "--global"},
                                        std::chrono::seconds{10});
  if (!root.ok()) return {{"installed", true}, {"path", path}};
  std::string npm_root = root.output;
  npm_root.erase(npm_root.find_last_not_of("\r\n") + 1);
  try {
    const auto package =
        nlohmann::json::parse(read_file(fs::path(npm_root) / "pm2" / "package.json"));
    return {{"installed", true}, {"path", path},
            {"version", package.value("version", "unknown")}};
  } catch (...) {
    return {{"installed", true}, {"path", path}, {"version", "unknown"}};
  }
}

int package_inventory(int argc, char** argv) {
  const auto os = server_os();
  auto names = selected_packages(argc, argv);
  for (const auto& preset : {"nginx", "pm2", "node", "npm", "docker"}) if (std::find(names.begin(), names.end(), preset) == names.end()) names.push_back(preset);
  nlohmann::json packages = nlohmann::json::object();
  for (const auto& name : names) {
    if (name == "pm2") packages[name] = pm2_check();
    else if (name == "nginx") packages[name] = command_check("nginx", {"-v"});
    else if (name == "node" || name == "nodejs") packages[name] = command_check("node", {"--version"});
    else if (name == "npm") packages[name] = command_check("npm", {"--version"});
    else if (name == "docker" || name == "docker.io") packages[name] = command_check("docker", {"--version"});
    else {
      const auto queried = across::run_process({"dpkg-query", "--show", "--showformat=${Status}\t${Version}", name}, std::chrono::seconds{10});
      const bool installed = queried.ok() && queried.output.starts_with("install ok installed\t");
      packages[name] = {{"installed", installed}};
      if (installed) packages[name]["version"] = queried.output.substr(21);
    }
  }
  std::cout << nlohmann::json{{"ok", true}, {"os", os}, {"packages", packages}}.dump(2) << '\n';
  return 0;
}

int doctor() {
  const bool nginx_valid =
      across::run_process({"nginx", "-t"}, std::chrono::seconds{10}).ok();
  const bool state_ready = fs::is_directory(kAppsRoot) && getpwnam(kServiceUser);
  nlohmann::json result{
      {"ok", state_ready && (executable("nginx").empty() || nginx_valid)},
      {"version", ACROSS_VERSION},
      {"environment_keys", true},
      {"user", std::string(geteuid() == 0 ? "root" : "non-root")},
      {"state_ready", state_ready},
      {"nginx_config_valid", nginx_valid},
      {"nginx", command_check("nginx", {"-v"})},
      {"node", command_check("node", {"--version"})},
      {"npm", command_check("npm", {"--version"})},
      {"pm2", pm2_check()},
      {"rsync", command_check("rsync", {"--version"})}};
  std::cout << result.dump(2) << '\n';
  return result.at("ok").get<bool>() ? 0 : 2;
}

std::string js_quote(const std::string& value) {
  std::string result{"'"};
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
      case '\\': result += "\\\\"; break;
      case '\'': result += "\\'"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (character < 0x20) {
          throw std::runtime_error("Command contains an unsupported control character");
        }
        result.push_back(static_cast<char>(character));
    }
  }
  result.push_back('\'');
  return result;
}

std::string nginx_configuration(const ProjectManifest& manifest) {
  const std::string internal = "across_" + manifest.app_id;
  std::ostringstream output;
  output << "# Managed by Across. Local changes will be replaced.\n";
  if (manifest.nginx.rate_limit_rps > 0) {
    output << "limit_req_zone $binary_remote_addr zone=" << internal
           << "_rate:10m rate=" << manifest.nginx.rate_limit_rps << "r/s;\n\n";
  }
  if (!manifest.nginx.upstreams.empty()) {
    output << "upstream " << internal << "_upstream {\n  least_conn;\n";
    for (const auto& upstream : manifest.nginx.upstreams) {
      output << "  server " << upstream << " max_fails=3 fail_timeout=10s;\n";
    }
    output << "  keepalive 32;\n}\n\n";
  }

  const bool tls = !manifest.nginx.tls_certificate.empty();
  if (tls && manifest.nginx.redirect_http_to_https) {
    output << "server {\n  listen 80;\n  listen [::]:80;\n  server_name";
    for (const auto& domain : manifest.nginx.domains) output << ' ' << domain;
    output << ";\n";
    if (fs::exists(fs::path(kAppsRoot) / manifest.app_id / "traffic-enabled")) output << "  access_log /var/log/nginx/across-" << manifest.app_id << ".access.jsonl across_json;\n";
    output << "  return 301 https://$host$request_uri;\n}\n\n";
  }
  output << "server {\n";
  if (tls) {
    if (!manifest.nginx.redirect_http_to_https) {
      output << "  listen 80;\n  listen [::]:80;\n";
    }
    output << "  listen 443 ssl;\n  listen [::]:443 ssl;\n"
           << "  ssl_certificate " << manifest.nginx.tls_certificate << ";\n"
           << "  ssl_certificate_key " << manifest.nginx.tls_certificate_key << ";\n"
           << "  ssl_protocols TLSv1.2 TLSv1.3;\n"
           << "  ssl_session_cache shared:SSL:10m;\n";
  } else {
    output << "  listen 80;\n  listen [::]:80;\n";
  }
  output << "  server_name";
  for (const auto& domain : manifest.nginx.domains) output << ' ' << domain;
  output << ";\n"
         << "  client_max_body_size " << manifest.nginx.client_max_body_mb << "m;\n"
         << "  add_header X-Content-Type-Options nosniff always;\n"
         << "  add_header Referrer-Policy strict-origin-when-cross-origin always;\n\n";
  if (fs::exists(fs::path(kAppsRoot) / manifest.app_id / "traffic-enabled"))
    output << "  access_log /var/log/nginx/across-" << manifest.app_id << ".access.jsonl across_json;\n";
  const auto blocked = fs::path(kAppsRoot) / manifest.app_id / "blocked-ips.json";
  if (fs::is_regular_file(blocked)) for (const auto& ip : nlohmann::json::parse(read_file(blocked)).get<std::vector<std::string>>()) output << "  deny " << ip << ";\n";
  if (manifest.type == "static") {
    output << "  root " << kAppsRoot << '/' << manifest.app_id << "/current/"
           << manifest.artifact_dir << ";\n"
           << "  index index.html;\n"
           << "  location / {\n"
           << "    try_files $uri $uri/ /index.html;\n";
  } else {
    output << "  location / {\n";
    if (!manifest.nginx.upstreams.empty()) {
      output << "    proxy_pass http://" << internal << "_upstream;\n";
    } else {
      output << "    proxy_pass http://127.0.0.1:" << manifest.port << ";\n";
    }
    output << "    proxy_http_version 1.1;\n"
           << "    proxy_set_header Host $host;\n"
           << "    proxy_set_header X-Real-IP $remote_addr;\n"
           << "    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\n"
           << "    proxy_set_header X-Forwarded-Proto $scheme;\n"
           << "    proxy_set_header Upgrade $http_upgrade;\n"
           << "    proxy_set_header Connection \"upgrade\";\n"
           << "    proxy_connect_timeout 5s;\n"
           << "    proxy_read_timeout 60s;\n";
  }
  if (manifest.nginx.rate_limit_rps > 0) {
    output << "    limit_req zone=" << internal << "_rate burst="
           << manifest.nginx.rate_limit_burst << " nodelay;\n";
  }
  output << "  }\n}\n";
  return output.str();
}

fs::path nginx_config_path(const std::string& app_id) {
  return fs::path(kNginxRoot) / ("across-" + app_id + ".conf");
}

bool domain_matches(std::string configured, std::string requested) {
  for (auto* domain : {&configured, &requested}) std::transform(domain->begin(), domain->end(), domain->begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  if (configured == requested) return true;
  const auto wildcard_matches = [](const std::string& wildcard,
                                   const std::string& domain) {
    if (!wildcard.starts_with("*.") || domain.size() <= wildcard.size() - 1) {
      return false;
    }
    const std::string suffix = wildcard.substr(1);
    return domain.ends_with(suffix);
  };
  return wildcard_matches(configured, requested) ||
         wildcard_matches(requested, configured);
}

std::vector<std::string> server_names_in(const fs::path& path) {
  std::istringstream input(read_file(path));
  std::string uncommented;
  std::string line;
  while (std::getline(input, line)) {
    if (const auto comment = line.find('#'); comment != std::string::npos) {
      line.resize(comment);
    }
    uncommented += line + '\n';
  }
  const std::regex declaration("server_name\\s+([^;]+);");
  std::vector<std::string> names;
  for (auto match = std::sregex_iterator(uncommented.begin(), uncommented.end(),
                                         declaration);
       match != std::sregex_iterator(); ++match) {
    std::istringstream values((*match)[1].str());
    std::string name;
    while (values >> name) names.push_back(name);
  }
  return names;
}

void ensure_domains_available(const ProjectManifest& manifest,
                              const fs::path& target) {
  const std::vector<fs::path> roots{"/etc/nginx/conf.d",
                                     "/etc/nginx/sites-enabled"};
  for (const auto& root : roots) {
    if (!fs::is_directory(root)) continue;
    for (const auto& entry : fs::directory_iterator(root)) {
      std::error_code error;
      if (!entry.is_regular_file(error) || entry.path() == target) continue;
      std::vector<std::string> configured;
      try {
        configured = server_names_in(entry.path());
      } catch (...) {
        continue;
      }
      for (const auto& requested : manifest.nginx.domains) {
        const auto conflict = std::find_if(
            configured.begin(), configured.end(), [&](const auto& existing) {
              return domain_matches(existing, requested);
            });
        if (conflict != configured.end()) {
          throw std::runtime_error("Domain " + requested +
                                   " is already configured in " +
                                   entry.path().string());
        }
      }
    }
  }
}

void restore_nginx_config(const fs::path& path,
                          const std::optional<std::string>& previous) {
  if (previous) write_file_atomically(path, *previous);
  else fs::remove(path);
  ensure_ok(run({"nginx", "-t"}, std::chrono::seconds{15}),
            "restored Nginx validation");
  ensure_ok(run({"systemctl", "reload", "nginx"}, std::chrono::seconds{30}),
            "restored Nginx reload");
}

void install_nginx_config(const ProjectManifest& manifest) {
  if (fs::is_regular_file(fs::path(kNginxRoot) / "00-across-domain-only.conf")) {
    for (const auto& domain : manifest.nginx.domains)
      if (domain.find(':') != std::string::npos || std::regex_match(domain, std::regex("^[0-9.]+$")))
        throw std::runtime_error("Domain-only access is enabled; use a hostname instead of an IP");
  }
  const fs::path path = nginx_config_path(manifest.app_id);
  ensure_domains_available(manifest, path);
  std::optional<std::string> previous;
  if (fs::exists(path)) {
    previous = read_file(path);
  }
  write_file_atomically(path, nginx_configuration(manifest));
  const auto tested = run({"nginx", "-t"}, std::chrono::seconds{15});
  if (!tested.ok()) {
    if (previous) {
      write_file_atomically(path, *previous);
    } else {
      fs::remove(path);
    }
    throw std::runtime_error("Generated Nginx configuration was rejected:\n" +
                             tested.output);
  }
  const auto reloaded = run({"systemctl", "reload", "nginx"},
                            std::chrono::seconds{30});
  if (!reloaded.ok()) {
    restore_nginx_config(path, previous);
    throw std::runtime_error("Nginx reload failed:\n" + reloaded.output);
  }
}

void switch_current(const fs::path& app_root, const fs::path& release) {
  const fs::path next = app_root / "current.next";
  const fs::path current = app_root / "current";
  std::error_code error;
  fs::remove(next, error);
  fs::create_directory_symlink(release, next);
  fs::rename(next, current);
}

ProcessResult run_as_service(const std::string& command,
                             std::chrono::seconds timeout =
                                 std::chrono::minutes{15}) {
  return run({"runuser", "-u", kServiceUser, "--", "env",
              "-i", "HOME=/var/lib/across", "USER=across", "LOGNAME=across",
              "SHELL=/bin/sh", "PM2_HOME=/var/lib/across/.pm2",
              "PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8",
              "LC_ALL=C.UTF-8", "/bin/sh", "-c",
              "cd /var/lib/across && " + command},
             timeout);
}

fs::path release_environment_path(const fs::path& release) {
  return release.parent_path().parent_path() / "environments" / (release.filename().string() + ".json");
}

void write_release_environment(const fs::path& release, const fs::path& environment_source) {
  if (!fs::is_regular_file(environment_source)) return;
  const auto keys = across::load_environment_keys(environment_source);
  const auto environment_file = release_environment_path(release);
  const auto exports_file = fs::path(environment_file.string() + ".sh");
  fs::create_directories(environment_file.parent_path());
  fs::permissions(environment_file.parent_path(), fs::perms::owner_all);
  ensure_ok(run({"chown", "across:across", environment_file.parent_path().string()}), "Environment directory ownership");
  write_file_atomically(environment_file, nlohmann::json(keys).dump(), fs::perms::owner_read | fs::perms::owner_write);
  std::string exports;
  for (const auto& [name, value] : keys) exports += "export " + name + "=" + across::shell_quote(value) + "\n";
  write_file_atomically(exports_file, exports, fs::perms::owner_read | fs::perms::owner_write);
  ensure_ok(run({"chown", "across:across", environment_file.string(), exports_file.string()}), "Environment file ownership");
}

void run_build_step(const fs::path& release, const std::string& label,
                    const std::string& command) {
  if (command.empty()) return;
  progress(label + "...");
  std::string setup;
  const auto environment = fs::path(release_environment_path(release).string() + ".sh");
  if (fs::is_regular_file(environment)) setup = ". " + across::shell_quote(environment.string()) + " && ";
  const auto result = run_as_service(setup + "cd " + across::shell_quote(release.string()) + " && " + command);
  ensure_ok(result, label);
}

fs::path write_ecosystem(const ProjectManifest& manifest, const fs::path& app_root) {
  const fs::path ecosystem = app_root / "ecosystem.config.cjs";
  const std::string name = "across-" + manifest.app_id;
  nlohmann::json environment = {{"NODE_ENV", "production"}, {"PORT", std::to_string(manifest.port)}};
  const auto environment_file = release_environment_path(fs::canonical(app_root / "current"));
  if (fs::is_regular_file(environment_file)) {
    for (const auto& [key, value] : across::load_environment_keys(environment_file)) environment[key] = value;
  }
  std::ostringstream contents;
  contents << "module.exports = { apps: [{\n"
           << "  name: " << js_quote(name) << ",\n"
           << "  cwd: " << js_quote((app_root / "current").string()) << ",\n"
           << "  script: '/bin/sh',\n"
           << "  interpreter: 'none',\n"
           << "  args: ['-lc', " << js_quote(manifest.start_command) << "],\n"
           << "  env: " << environment.dump() << ",\n"
           << "  autorestart: true, max_restarts: 10, restart_delay: 1000,\n"
           << "  kill_timeout: 5000, listen_timeout: 10000\n"
           << "}] };\n";
  write_file_atomically(ecosystem, contents.str(),
                        fs::perms::owner_read | fs::perms::owner_write);
  ensure_ok(run({"chown", "across:across", ecosystem.string()}),
            "ecosystem ownership");
  return ecosystem;
}

void start_node(const ProjectManifest& manifest, const fs::path& app_root) {
  const auto ecosystem = write_ecosystem(manifest, app_root);
  ensure_ok(run_as_service("pm2 startOrReload " +
                           across::shell_quote(ecosystem.string()) + " --update-env"),
            "PM2 start/reload");
  ensure_ok(run_as_service("pm2 save --force", std::chrono::minutes{2}),
            "PM2 state save");
}

void stop_node(const std::string& app_id) {
  run_as_service("pm2 delete " + across::shell_quote("across-" + app_id),
                 std::chrono::minutes{2});
  run_as_service("pm2 save --force", std::chrono::minutes{2});
}

bool health_check(const ProjectManifest& manifest) {
  std::string url;
  std::vector<std::string> arguments{"curl", "--fail", "--silent", "--show-error",
                                      "--max-time", "3"};
  if (manifest.type == "node" && manifest.nginx.upstreams.empty()) {
    url = "http://127.0.0.1:" + std::to_string(manifest.port) + manifest.health_path;
  } else {
    arguments.insert(arguments.end(), {"-H", "Host: " + manifest.nginx.domains.front()});
    url = "http://127.0.0.1" + manifest.health_path;
  }
  arguments.push_back(url);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(manifest.health_timeout_seconds);
  int consecutive_successes = 0;
  do {
    if (across::run_process(arguments, std::chrono::seconds{5}).ok()) {
      ++consecutive_successes;
      if (consecutive_successes >= 3) return true;
    } else {
      consecutive_successes = 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

bool port_is_listening(int port) {
  const auto result =
      across::run_process({"ss", "-H", "-ltn"}, std::chrono::seconds{10});
  if (!result.ok()) {
    throw std::runtime_error("Could not inspect listening TCP ports");
  }
  const std::regex listener(":" + std::to_string(port) + "\\s");
  return std::regex_search(result.output, listener);
}

void remove_old_releases(const fs::path& releases, int keep) {
  std::vector<fs::directory_entry> entries;
  for (const auto& entry : fs::directory_iterator(releases)) {
    if (entry.is_directory()) entries.push_back(entry);
  }
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    return left.path().filename().string() > right.path().filename().string();
  });
  for (std::size_t index = static_cast<std::size_t>(keep); index < entries.size();
       ++index) {
    fs::remove(release_environment_path(entries[index].path()));
    fs::remove_all(entries[index]);
  }
}

ProjectManifest load_remote_manifest(const fs::path& path,
                                     const fs::path& expected_source) {
  auto manifest = across::load_manifest(path);
  const auto actual = fs::weakly_canonical(manifest.project_root);
  const auto expected = fs::weakly_canonical(expected_source);
  if (actual != expected) {
    throw std::runtime_error("Manifest project_root does not match uploaded source");
  }
  return manifest;
}

void validate_release_id(const std::string& release) {
  static const std::regex pattern("^[0-9]{8}T[0-9]{6}Z-[0-9a-f]{4}$");
  if (!std::regex_match(release, pattern)) {
    throw std::runtime_error("Invalid release id");
  }
}

void apply_website_overrides(ProjectManifest& manifest) {
  const auto path = fs::path(kAppsRoot) / manifest.app_id / "website-settings.json";
  if (!fs::is_regular_file(path)) return;
  auto nginx = nlohmann::json(manifest.nginx);
  nginx.update(nlohmann::json::parse(read_file(path)));
  manifest.nginx = nginx.get<across::NginxConfig>();
  across::validate_manifest(manifest);
}

void persist_website(const ProjectManifest& manifest) {
  const auto root = fs::path(kAppsRoot) / manifest.app_id;
  const auto path = root / "manifest.json";
  const auto settings = root / "website-settings.json";
  const auto original = read_file(path);
  std::optional<std::string> previous_settings;
  if (fs::is_regular_file(settings)) previous_settings = read_file(settings);
  auto overrides = nlohmann::json(manifest.nginx);
  overrides.erase("upstreams");
  try {
    write_file_atomically(settings, overrides.dump(2));
    write_file_atomically(path, nlohmann::json(manifest).dump(2));
  } catch (...) {
    write_file_atomically(path, original);
    if (previous_settings) write_file_atomically(settings, *previous_settings); else fs::remove(settings);
    throw;
  }
}

int deploy(int argc, char** argv) {
  require_root();
  const fs::path manifest_path = option_value(argc, argv, "--manifest");
  const fs::path source = option_value(argc, argv, "--source");
  const std::string release_id = option_value(argc, argv, "--release");
  validate_release_id(release_id);
  if (!fs::is_directory(source)) {
    throw std::runtime_error("Uploaded source directory does not exist");
  }
  auto manifest = load_remote_manifest(manifest_path, source);
  const auto environment_option = option_value(argc, argv, "--environment", false);
  if (!environment_option.empty()) across::load_environment_keys(environment_option);
  AppLock lock(manifest.app_id);
  AppLock nginx_lock(".nginx-global");
  apply_website_overrides(manifest);
  ensure_service_user();

  const fs::path app_root = fs::path(kAppsRoot) / manifest.app_id;
  const fs::path current = app_root / "current";
  const fs::path stored_manifest = app_root / "manifest.json";
  std::optional<ProjectManifest> previous_manifest;
  if (fs::is_regular_file(stored_manifest)) {
    previous_manifest = across::load_manifest(stored_manifest);
  }
  if (manifest.type == "node" &&
      (!previous_manifest || previous_manifest->type != "node") &&
      port_is_listening(manifest.port)) {
    throw std::runtime_error("Application port " + std::to_string(manifest.port) +
                             " is already in use");
  }
  const fs::path releases = app_root / "releases";
  const fs::path release = releases / release_id;
  const bool new_application = !previous_manifest && !fs::is_symlink(current);
  const auto clean_failed_release = [&]() {
    fs::remove(release_environment_path(release));
    fs::remove(fs::path(release_environment_path(release).string() + ".sh"));
    fs::remove_all(release);
    if (new_application) fs::remove_all(app_root);
  };
  fs::create_directories(releases);
  if (fs::exists(release)) {
    throw std::runtime_error("Release already exists: " + release_id);
  }
  fs::create_directories(release);

  try {
    progress("Copying uploaded files into an isolated release...");
    auto source_with_slash = source.string();
    if (!source_with_slash.ends_with('/')) source_with_slash.push_back('/');
    auto release_with_slash = release.string();
    if (!release_with_slash.ends_with('/')) release_with_slash.push_back('/');
    ensure_ok(run({"rsync", "--archive", "--delete", "--safe-links",
                   "--chown=across:across", source_with_slash, release_with_slash},
                  std::chrono::minutes{10}),
              "release copy");

    const auto environment_source = environment_option.empty() ?
        (fs::exists(current) ? release_environment_path(fs::canonical(current)) : fs::path{}) : fs::path(environment_option);
    write_release_environment(release, environment_source);
    if (manifest.type != "proxy" && manifest.build_location == "vps") {
      run_build_step(release, "Installing dependencies", manifest.install_command);
      run_build_step(release, "Building application", manifest.build_command);
    } else if (manifest.type != "proxy") {
      progress("Using locally built application; skipping VPS install and build...");
    }
    fs::remove(fs::path(release_environment_path(release).string() + ".sh"));
    if (manifest.type == "static" &&
        !fs::is_directory(release / manifest.artifact_dir)) {
      throw std::runtime_error("Static artifact directory was not produced: " +
                               manifest.artifact_dir);
    }
  } catch (...) {
    clean_failed_release();
    throw;
  }

  std::optional<fs::path> previous;
  if (fs::is_symlink(current)) previous = fs::read_symlink(current);
  const fs::path nginx_path = nginx_config_path(manifest.app_id);
  std::optional<std::string> previous_nginx;
  if (fs::is_regular_file(nginx_path)) previous_nginx = read_file(nginx_path);
  bool nginx_changed = false;

  auto rollback = [&]() {
    if (previous && fs::exists(*previous)) {
      switch_current(app_root, *previous);
      if (previous_manifest && previous_manifest->type == "node") {
        try {
          start_node(*previous_manifest, app_root);
        } catch (...) {
          progress("Warning: the prior PM2 process could not be restarted");
        }
      } else if (manifest.type == "node") {
        try {
          stop_node(manifest.app_id);
        } catch (...) {
          progress("Warning: the failed PM2 process could not be stopped");
        }
      }
    } else {
      std::error_code error;
      fs::remove(current, error);
      if (manifest.type == "node") {
        try {
          stop_node(manifest.app_id);
        } catch (...) {
          progress("Warning: the failed PM2 process could not be stopped");
        }
      }
    }
    if (nginx_changed) {
      try {
        restore_nginx_config(nginx_path, previous_nginx);
      } catch (...) {
        progress("Warning: the prior Nginx configuration could not be restored");
      }
    }
  };

  try {
    progress("Switching the current release atomically...");
    switch_current(app_root, release);
    if (manifest.type == "node") {
      progress("Starting application with PM2...");
      start_node(manifest, app_root);
    } else if (previous_manifest && previous_manifest->type == "node") {
      progress("Stopping the prior PM2 application...");
      stop_node(manifest.app_id);
    }
    progress("Installing validated Nginx configuration...");
    install_nginx_config(manifest);
    nginx_changed = true;
    progress("Checking application health...");
    if (!health_check(manifest)) {
      throw std::runtime_error("Health check did not pass before the timeout");
    }
  } catch (...) {
    rollback();
    clean_failed_release();
    throw;
  }

  manifest.project_root = current;
  const std::string manifest_contents = nlohmann::json(manifest).dump(2) + "\n";
  const auto manifest_permissions =
      fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;
  write_file_atomically(stored_manifest, manifest_contents, manifest_permissions);
  write_file_atomically(release / ".across-manifest.json", manifest_contents,
                        manifest_permissions);
  ensure_ok(run({"chown", "-R", "across:across", app_root.string()}),
            "application ownership");
  remove_old_releases(releases, manifest.keep_releases);
  std::cout << nlohmann::json{{"ok", true}, {"app_id", manifest.app_id},
                              {"release", release_id},
                              {"current", release.string()}}
                   .dump()
            << '\n';
  return 0;
}

int list_apps() {
  nlohmann::json apps = nlohmann::json::array();
  if (fs::is_directory(kAppsRoot)) {
    for (const auto& entry : fs::directory_iterator(kAppsRoot)) {
      if (!entry.is_directory()) continue;
      if (!fs::is_regular_file(entry.path() / "manifest.json") &&
          !fs::is_symlink(entry.path() / "current")) {
        continue;
      }
      nlohmann::json app{{"app_id", entry.path().filename().string()}};
      const auto current = entry.path() / "current";
      if (fs::is_symlink(current)) {
        app["release"] = fs::read_symlink(current).filename().string();
      }
      const auto manifest = entry.path() / "manifest.json";
      if (fs::is_regular_file(manifest)) {
        try {
          const auto parsed = nlohmann::json::parse(read_file(manifest));
          app["type"] = parsed.value("type", "unknown");
          app["https"] = !parsed.value("nginx", nlohmann::json::object()).value("tls_certificate", std::string{}).empty();
          app["domains"] = parsed.value("nginx", nlohmann::json::object())
                               .value("domains", std::vector<std::string>{});
        } catch (...) {
          app["manifest_error"] = true;
        }
      }
      apps.push_back(app);
    }
  }
  std::cout << nlohmann::json{{"ok", true}, {"apps", apps}}.dump(2) << '\n';
  return 0;
}

int rollback_app(int argc, char** argv) {
  require_root();
  const std::string app_id = option_value(argc, argv, "--app");
  if (!std::regex_match(app_id, std::regex("^[a-z0-9][a-z0-9-]{0,62}$"))) throw std::runtime_error("Invalid app id");
  AppLock lock(app_id);
  AppLock nginx_lock(".nginx-global");
  const fs::path app_root = fs::path(kAppsRoot) / app_id;
  const fs::path manifest_path = app_root / "manifest.json";
  if (!fs::is_regular_file(manifest_path)) throw std::runtime_error("Unknown app");
  const auto active_manifest = across::load_manifest(manifest_path);
  const fs::path current = app_root / "current";
  if (!fs::is_symlink(current)) throw std::runtime_error("App has no current release");
  const fs::path original = fs::read_symlink(current);
  const std::string requested = option_value(argc, argv, "--release", false);
  fs::path target;
  if (!requested.empty()) {
    validate_release_id(requested);
    target = app_root / "releases" / requested;
  } else {
    std::vector<fs::path> releases;
    for (const auto& entry : fs::directory_iterator(app_root / "releases")) {
      if (entry.is_directory()) releases.push_back(entry.path());
    }
    std::sort(releases.begin(), releases.end(), std::greater<>());
    const auto active = original.filename();
    const auto found = std::find_if(releases.begin(), releases.end(),
                                    [&](const auto& value) {
                                      return value.filename() != active;
                                    });
    if (found == releases.end()) throw std::runtime_error("No prior release available");
    target = *found;
  }
  if (!fs::is_directory(target)) throw std::runtime_error("Release does not exist");
  ProjectManifest target_manifest = active_manifest;
  const fs::path release_manifest = target / ".across-manifest.json";
  if (fs::is_regular_file(release_manifest)) {
    target_manifest = across::load_manifest(release_manifest);
  }
  if (target_manifest.type == "node" && active_manifest.type != "node" &&
      port_is_listening(target_manifest.port)) {
    throw std::runtime_error("Application port " +
                             std::to_string(target_manifest.port) +
                             " is already in use");
  }
  try {
    switch_current(app_root, target);
    if (target_manifest.type == "node") {
      start_node(target_manifest, app_root);
    } else if (active_manifest.type == "node") {
      stop_node(app_id);
    }
    apply_website_overrides(target_manifest);
    install_nginx_config(target_manifest);
    if (!health_check(target_manifest)) {
      throw std::runtime_error("Rollback health check failed");
    }
  } catch (...) {
    switch_current(app_root, original);
    try {
      if (active_manifest.type == "node") start_node(active_manifest, app_root);
      else if (target_manifest.type == "node") stop_node(app_id);
      install_nginx_config(active_manifest);
    } catch (...) {
      progress("Warning: recovery of the original release was incomplete");
    }
    throw;
  }
  target_manifest.project_root = current;
  write_file_atomically(manifest_path,
                        nlohmann::json(target_manifest).dump(2) + "\n",
                        fs::perms::owner_read | fs::perms::owner_write |
                            fs::perms::group_read);
  ensure_ok(run({"chown", "across:across", manifest_path.string()}),
            "manifest ownership");
  std::cout << nlohmann::json{{"ok", true}, {"app_id", app_id},
                              {"release", target.filename().string()}}
                   .dump()
            << '\n';
  return 0;
}

int remove_app(int argc, char** argv) {
  require_root();
  const std::string app_id = option_value(argc, argv, "--app");
  if (!std::regex_match(app_id, std::regex("^[a-z0-9][a-z0-9-]{0,62}$"))) throw std::runtime_error("Invalid app id");
  AppLock lock(app_id);
  AppLock nginx_lock(".nginx-global");
  const fs::path app_root = fs::path(kAppsRoot) / app_id;
  if (!fs::exists(app_root)) throw std::runtime_error("Unknown app");
  const fs::path manifest_path = app_root / "manifest.json";
  std::optional<ProjectManifest> manifest;
  if (fs::is_regular_file(manifest_path)) {
    manifest = across::load_manifest(manifest_path);
  }
  const fs::path nginx = nginx_config_path(app_id);
  std::optional<std::string> previous_nginx;
  if (fs::is_regular_file(nginx)) previous_nginx = read_file(nginx);
  if (previous_nginx) {
    fs::remove(nginx);
    const auto tested = run({"nginx", "-t"}, std::chrono::seconds{15});
    if (!tested.ok()) {
      restore_nginx_config(nginx, previous_nginx);
      throw std::runtime_error("Nginx validation after removal failed:\n" +
                               tested.output);
    }
    const auto reloaded = run({"systemctl", "reload", "nginx"},
                              std::chrono::seconds{30});
    if (!reloaded.ok()) {
      restore_nginx_config(nginx, previous_nginx);
      throw std::runtime_error("Nginx reload after removal failed:\n" +
                               reloaded.output);
    }
  }
  if (manifest && manifest->type == "node") stop_node(app_id);
  fs::remove_all(app_root);
  std::cout << nlohmann::json{{"ok", true}, {"removed", app_id}}.dump() << '\n';
  return 0;
}

std::string managed_app_id(int argc, char** argv) {
  const auto id = option_value(argc, argv, "--app");
  if (!std::regex_match(id, std::regex("^[a-z0-9][a-z0-9-]{0,62}$"))) throw std::runtime_error("Invalid app id");
  return id;
}

int website_settings(int argc, char** argv) {
  require_root();
  const auto id = managed_app_id(argc, argv);
  AppLock lock(id);
  AppLock nginx_lock(".nginx-global");
  const fs::path path = fs::path(kAppsRoot) / id / "manifest.json";
  auto manifest = across::load_manifest(path);
  const auto domains = option_value(argc, argv, "--domains", false);
  if (!domains.empty()) manifest.nginx.domains = across::parse_domains(domains);
  for (const auto& option : {"--body-mb", "--rate", "--burst"}) {
    const auto value = option_value(argc, argv, option, false);
    if (value.empty()) continue;
    if (!std::regex_match(value, std::regex("^[0-9]{1,6}$"))) throw std::runtime_error("Invalid numeric Nginx setting");
    const int number = std::stoi(value);
    if (std::string{option} == "--body-mb") manifest.nginx.client_max_body_mb = number;
    if (std::string{option} == "--rate") manifest.nginx.rate_limit_rps = number;
    if (std::string{option} == "--burst") manifest.nginx.rate_limit_burst = number;
  }
  across::validate_manifest(manifest);
  const fs::path config = nginx_config_path(id);
  const auto previous = read_file(config);
  install_nginx_config(manifest);
  try { persist_website(manifest); }
  catch (...) { restore_nginx_config(config, previous); throw; }
  std::cout << nlohmann::json{{"ok", true}, {"manifest", manifest}, {"nginx", read_file(config)}}.dump(2) << '\n';
  return 0;
}

fs::path domain_only_config() { return fs::path(kNginxRoot) / "00-across-domain-only.conf"; }
fs::path default_site_path() {
#ifdef ACROSS_SERVER_TESTING
  return server_test_root / "sites-enabled/default";
#else
  return "/etc/nginx/sites-enabled/default";
#endif
}
bool domain_only_enabled() { return fs::is_regular_file(domain_only_config()); }

int nginx_access(int argc, char** argv) {
  require_root();
  const auto mode = option_value(argc, argv, "--mode");
  if (mode != "status" && mode != "domain-only" && mode != "restore") throw std::runtime_error("Invalid access mode");
  AppLock lock(".nginx-global");
  const auto config = domain_only_config(), original_site = default_site_path();
  const auto backup = fs::path(kStateRoot) / "default-site.backup";
  const auto exists = [](const fs::path& path) { return fs::exists(path) || fs::is_symlink(path); };
  if (mode != "status") {
    std::optional<std::string> previous;
    if (fs::is_regular_file(config)) previous = read_file(config);
    bool moved = false;
    try {
      if (mode == "domain-only") {
        for (const auto& entry : fs::directory_iterator(kAppsRoot)) {
          const auto manifest_path = entry.path() / "manifest.json";
          if (!fs::is_regular_file(manifest_path)) continue;
          for (const auto& domain : across::load_manifest(manifest_path).nginx.domains) {
            if (domain.find(':') != std::string::npos || std::regex_match(domain, std::regex("^[0-9.]+$")))
              throw std::runtime_error("Replace the IP hostname for " + entry.path().filename().string() + " with a domain first");
          }
        }
        if (exists(original_site)) {
          if (exists(backup)) throw std::runtime_error("Default site backup already exists; resolve it before changing access");
          fs::rename(original_site, backup); moved = true;
        }
        write_file_atomically(config, "# Managed by Across: domain-only access\nserver {\n  listen 80 default_server;\n  listen [::]:80 default_server;\n  server_name _;\n  return 403;\n}\nserver {\n  listen 443 ssl default_server;\n  listen [::]:443 ssl default_server;\n  server_name _;\n  ssl_reject_handshake on;\n  return 403;\n}\n");
      } else {
        fs::remove(config);
        if (exists(backup)) {
          if (exists(original_site)) throw std::runtime_error("Default site already exists; cannot overwrite it");
          fs::rename(backup, original_site); moved = true;
        }
      }
      ensure_ok(across::run_process({"nginx", "-t"}, std::chrono::seconds{15}), "Nginx validation");
      ensure_ok(across::run_process({"systemctl", "reload", "nginx"}, std::chrono::seconds{30}), "Nginx reload");
    } catch (...) {
      if (previous) write_file_atomically(config, *previous); else fs::remove(config);
      if (moved) { if (mode == "domain-only") fs::rename(backup, original_site); else fs::rename(original_site, backup); }
      across::run_process({"systemctl", "reload", "nginx"}, std::chrono::seconds{30});
      throw;
    }
  }
  std::cout << nlohmann::json{{"ok", true}, {"domain_only", domain_only_enabled()}}.dump() << '\n';
  return 0;
}

int inspect_website(int argc, char** argv) {
  const auto id = managed_app_id(argc, argv);
  const auto path = fs::path(kAppsRoot) / id;
  std::cout << nlohmann::json{{"ok", true}, {"manifest", nlohmann::json::parse(read_file(path / "manifest.json"))}, {"nginx", read_file(nginx_config_path(id))}, {"domain_only", domain_only_enabled()}}.dump(2) << '\n';
  return 0;
}

int edit_nginx(int argc, char** argv) {
  require_root();
  const auto id = managed_app_id(argc, argv);
  AppLock lock(id);
  AppLock nginx_lock(".nginx-global");
  if (!fs::is_regular_file(fs::path(kAppsRoot) / id / "manifest.json")) throw std::runtime_error("Unknown website");
  const auto contents = option_value(argc, argv, "--content");
  if (contents.empty() || contents.size() > 65536) throw std::runtime_error("Nginx config must contain 1-65536 bytes");
  const auto path = nginx_config_path(id);
  const auto previous = read_file(path);
  write_file_atomically(path, contents);
  const auto tested = run({"nginx", "-t"}, std::chrono::seconds{15});
  if (!tested.ok()) { write_file_atomically(path, previous); throw std::runtime_error("Nginx rejected changes: " + tested.output); }
  if (!run({"systemctl", "reload", "nginx"}, std::chrono::seconds{30}).ok()) {
    restore_nginx_config(path, previous); throw std::runtime_error("Nginx reload failed; previous config restored");
  }
  std::cout << "Nginx configuration saved and reloaded. Custom edits are replaced by a future deployment or settings update.\n";
  return 0;
}

int manage_tls(int argc, char** argv, bool renew) {
  require_root();
  const auto id = managed_app_id(argc, argv);
  AppLock lock(id);
  AppLock nginx_lock(".nginx-global");
  const fs::path path = fs::path(kAppsRoot) / id / "manifest.json";
  auto manifest = across::load_manifest(path);
  if (executable("certbot").empty()) {
    if (!executable("apt-get").empty()) ensure_ok(run({"env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "install", "-y", "certbot", "python3-certbot-nginx"}), "Certbot installation");
    else if (!executable("dnf").empty()) ensure_ok(run({"dnf", "install", "-y", "certbot", "python3-certbot-nginx"}), "Certbot installation");
    else throw std::runtime_error("Install certbot and its nginx plugin on this distribution first");
  }
  std::vector<std::string> arguments{"certbot"};
  if (renew) {
    if (manifest.nginx.tls_certificate != "/etc/letsencrypt/live/across-" + id + "/fullchain.pem") throw std::runtime_error("Issue an Across certificate before renewing it");
    arguments.insert(arguments.end(), {"renew", "--cert-name", "across-" + id, "--non-interactive"});
  } else {
    const auto email = option_value(argc, argv, "--email");
    if (!std::regex_match(email, std::regex("^[A-Za-z0-9._+%-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}$"))) throw std::runtime_error("Enter a valid certificate account email");
    arguments.insert(arguments.end(), {"certonly", "--nginx", "--non-interactive", "--agree-tos", "--email", email, "--cert-name", "across-" + id});
    for (const auto& domain : manifest.nginx.domains) {
      across::validate_domain(domain);
      arguments.insert(arguments.end(), {"-d", domain});
    }
  }
  ensure_ok(run(arguments, std::chrono::minutes{5}), "Certificate operation");
  manifest.nginx.tls_certificate = "/etc/letsencrypt/live/across-" + id + "/fullchain.pem";
  manifest.nginx.tls_certificate_key = "/etc/letsencrypt/live/across-" + id + "/privkey.pem";
  manifest.nginx.redirect_http_to_https = true;
  across::validate_manifest(manifest);
  const auto config = nginx_config_path(id);
  const auto previous = read_file(config);
  install_nginx_config(manifest);
  try { persist_website(manifest); }
  catch (...) { restore_nginx_config(config, previous); throw; }
  // Certbot's scheduler reloads Nginx after a successful renewal.
  write_file_atomically("/etc/letsencrypt/renewal-hooks/deploy/across-nginx", "#!/bin/sh\nnginx -t && systemctl reload nginx\n", fs::perms::owner_all);
  write_file_atomically("/etc/systemd/system/across-certbot.service", "[Unit]\nDescription=Renew Across HTTPS certificates\n[Service]\nType=oneshot\nExecStart=" + executable("certbot") + " renew --non-interactive\n");
  write_file_atomically("/etc/systemd/system/across-certbot.timer", "[Unit]\nDescription=Across HTTPS renewal schedule\n[Timer]\nOnCalendar=*-*-* 00,12:00:00\nRandomizedDelaySec=3600\nPersistent=true\n[Install]\nWantedBy=timers.target\n");
  ensure_ok(run({"systemctl", "daemon-reload"}), "TLS scheduler reload");
  ensure_ok(run({"systemctl", "enable", "--now", "across-certbot.timer"}), "TLS renewal scheduler");
  std::cout << "HTTPS configured with automatic renewal. Renewal only issues a replacement when due.\n";
  return 0;
}

int server_metrics() {
  nlohmann::json metrics{{"ok", true}};
  for (const auto& item : std::vector<std::pair<std::string, std::vector<std::string>>>{{"uptime", {"uptime"}}, {"memory", {"free", "-b"}}, {"disk", {"df", "-h", "/"}}, {"services", {"systemctl", "show", "nginx.service", "across-pm2.service", "--property=ActiveState,MemoryCurrent,MemoryMax,CPUQuotaPerSecUSec"}}}) {
    const auto result = across::run_process(item.second, std::chrono::seconds{10});
    metrics[item.first] = {{"available", result.ok()}, {"output", result.output}};
  }
  if (fs::exists("/proc/loadavg")) metrics["loadavg"] = read_file("/proc/loadavg");
  metrics["cpu_cores"] = std::thread::hardware_concurrency();
  if (fs::exists("/proc/uptime")) {
    std::istringstream stream(read_file("/proc/uptime")); double seconds = 0; stream >> seconds; metrics["uptime_seconds"] = seconds;
  }
  if (fs::exists("/proc/meminfo")) {
    std::istringstream stream(read_file("/proc/meminfo")); std::string line;
    while (std::getline(stream, line)) {
      std::istringstream row(line); std::string key; std::uint64_t kilobytes = 0; row >> key >> kilobytes;
      if (key == "MemTotal:") metrics["memory_total_bytes"] = kilobytes * 1024;
      if (key == "MemAvailable:") metrics["memory_available_bytes"] = kilobytes * 1024;
    }
  }
  const auto disk = fs::space("/");
  metrics["disk_total_bytes"] = disk.capacity; metrics["disk_free_bytes"] = disk.free;
  for (const auto& service : {"nginx.service", "across-pm2.service"}) {
    auto state = across::run_process({"systemctl", "is-active", service}, std::chrono::seconds{10}).output;
    state.erase(std::remove(state.begin(), state.end(), '\n'), state.end());
    metrics["service_states"][service] = state.empty() ? "unavailable" : state;
  }
  std::cout << metrics.dump(2) << '\n';
  return 0;
}

int resource_limits(int argc, char** argv) {
  require_root();
  const auto service = option_value(argc, argv, "--service");
  if (service != "nginx.service" && service != "across-pm2.service") throw std::runtime_error("Select nginx.service or across-pm2.service");
  const auto cpu = option_value(argc, argv, "--cpu");
  const auto memory = option_value(argc, argv, "--memory-mb");
  if (!std::regex_match(cpu, std::regex("^[0-9]{1,5}$")) || std::stoi(cpu) < 1 || std::stoi(cpu) > 10000 || !std::regex_match(memory, std::regex("^[0-9]{1,8}$")) || std::stoi(memory) < 64) throw std::runtime_error("CPU must be 1-10000 percent; memory must be at least 64 MiB");
  ensure_ok(run({"systemctl", "set-property", service, "CPUQuota=" + cpu + "%", "MemoryMax=" + memory + "M"}), "Resource limits");
  return server_metrics();
}

int firewall(int argc, char** argv) {
  require_root();
  const auto action = option_value(argc, argv, "--action");
  if (action == "install") {
    if (!executable("apt-get").empty()) ensure_ok(run({"env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "install", "-y", "ufw"}), "UFW installation");
    else throw std::runtime_error("Automatic UFW installation requires apt; install UFW manually on this distribution");
    return 0;
  }
  if (executable("ufw").empty()) throw std::runtime_error("Install UFW before managing firewall rules");
  if (action == "enable") {
    const auto ssh_port = option_value(argc, argv, "--ssh-port");
    across::validate_firewall_rule("allow", ssh_port, "tcp");
    ensure_ok(run({"ufw", "--force", "delete", "deny", ssh_port + "/tcp"}), "Remove conflicting SSH block");
    ensure_ok(run({"ufw", "prepend", "allow", ssh_port + "/tcp"}), "Preserve SSH access");
    if (fs::is_directory(kAppsRoot) && !fs::is_empty(kAppsRoot)) {
      ensure_ok(run({"ufw", "allow", "80/tcp"}), "Preserve website HTTP access");
      ensure_ok(run({"ufw", "allow", "443/tcp"}), "Preserve website HTTPS access");
    }
    ensure_ok(run({"ufw", "--force", "enable"}), "Enable firewall");
    ensure_ok(run({"ufw", "status", "numbered"}), "Firewall status");
    return 0;
  }
  if (action == "status") { ensure_ok(run({"ufw", "status", "numbered"}), "Firewall status"); return 0; }
  const auto port = option_value(argc, argv, "--port");
  const auto protocol = option_value(argc, argv, "--protocol");
  across::validate_firewall_rule(action == "unblock" ? "delete" : action, port, protocol);
  const auto ssh_port = option_value(argc, argv, "--ssh-port");
  across::validate_firewall_rule("allow", ssh_port, "tcp");
  if (port == ssh_port && protocol == "tcp" && action != "allow") throw std::runtime_error("Cannot block or delete the active SSH port");
  if (action == "delete") ensure_ok(run({"ufw", "--force", "delete", "allow", port + "/" + protocol}), "Delete firewall allow rule");
  else if (action == "unblock") ensure_ok(run({"ufw", "--force", "delete", "deny", port + "/" + protocol}), "Remove port block");
  else {
    ensure_ok(run({"ufw", "--force", "delete", action == "allow" ? "deny" : "allow", port + "/" + protocol}), "Remove conflicting port rule");
    ensure_ok(run({"ufw", "prepend", action, port + "/" + protocol}), "Firewall rule");
  }
  ensure_ok(run({"ufw", "status", "numbered"}), "Firewall status");
  return 0;
}

#include "security.hpp"
#include "monitoring.hpp"

void usage() {
  std::cout
      << "across-server " << ACROSS_VERSION << "\n"
      << "Usage:\n"
      << "  across-server doctor --json\n"
      << "  across-server provision --json\n"
      << "  across-server list --json\n"
      << "  across-server deploy --manifest PATH --source PATH --release ID --json\n"
      << "  across-server rollback --app ID [--release ID] --json\n"
      << "  across-server remove --app ID --json\n"
      << "  across-server website --app ID\n"
      << "  across-server nginx-access --mode status|domain-only|restore [--json]\n"
      << "  across-server website-settings --app ID [--domains HOSTNAMES] [--body-mb N] [--rate N] [--burst N]\n"
      << "  across-server nginx-save --app ID --content CONFIG\n"
      << "  across-server tls-issue --app ID --email EMAIL\n"
      << "  across-server tls-renew --app ID\n"
      << "  across-server packages [--packages JSON_ARRAY]\n"
      << "  across-server packages-install --packages JSON_ARRAY\n"
      << "  across-server metrics\n"
      << "  across-server resource-limits --service SERVICE --cpu PERCENT --memory-mb MIB\n"
      << "  across-server firewall --action status|install|enable|allow|deny|delete [--port N --protocol tcp|udp --ssh-port N]\n";
}

}  // namespace

int main(int argc, char** argv) {
  const bool json_output = has_option(argc, argv, "--json");
  try {
    if (argc < 2) {
      usage();
      return 1;
    }
    const std::string command = argv[1];
    if (command == "version" || command == "--version") {
      std::cout << ACROSS_VERSION << '\n';
      return 0;
    }
    if (command == "doctor") return doctor();
    if (command == "provision" || command == "packages-install") return provision(argc, argv, json_output);
    if (command == "packages") return package_inventory(argc, argv);
    if (command == "list" || command == "status") return list_apps();
    if (command == "deploy") return deploy(argc, argv);
    if (command == "rollback") return rollback_app(argc, argv);
    if (command == "remove") return remove_app(argc, argv);
    if (command == "website") return inspect_website(argc, argv);
    if (command == "nginx-access") return nginx_access(argc, argv);
    if (command == "website-settings") return website_settings(argc, argv);
    if (command == "nginx-save") return edit_nginx(argc, argv);
    if (command == "tls-issue") return manage_tls(argc, argv, false);
    if (command == "tls-renew") return manage_tls(argc, argv, true);
    if (command == "security") return security_status();
    if (command == "security-action") return security_action(argc, argv);
    if (command == "monitoring") return monitoring(argc, argv);
    if (command == "traffic-logging") return traffic_logging(argc, argv);
    if (command == "metrics") return server_metrics();
    if (command == "resource-limits") return resource_limits(argc, argv);
    if (command == "firewall") return firewall(argc, argv);
    usage();
    return 1;
  } catch (const std::exception& error) {
    if (json_output) {
      std::cerr << nlohmann::json{{"ok", false}, {"error", error.what()}}.dump()
                << '\n';
    } else {
      std::cerr << "error: " << error.what() << '\n';
    }
    return 1;
  }
}
