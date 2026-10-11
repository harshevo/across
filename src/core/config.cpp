#include "across/config.hpp"
#include "across/management.hpp"
#include "across/packages.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace across {
namespace {

std::filesystem::path home_directory() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    throw std::runtime_error("HOME is not set");
  }
  return home;
}

void make_private_directory(const std::filesystem::path& path) {
  std::filesystem::create_directories(path);
  std::filesystem::permissions(
      path, std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace);
}

nlohmann::json read_json_or_empty(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    return nlohmann::json{{"servers", nlohmann::json::array()}};
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot open configuration: " + path.string());
  }
  auto json = nlohmann::json::parse(input);
  if (!json.contains("servers") || !json["servers"].is_array()) {
    throw std::runtime_error("Invalid configuration: servers must be an array");
  }
  return json;
}

void write_json_atomically(const std::filesystem::path& path,
                           const nlohmann::json& json) {
  make_private_directory(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
      throw std::runtime_error("Cannot write configuration: " + temporary);
    }
    output << json.dump(2) << '\n';
    output.flush();
    if (!output) {
      throw std::runtime_error("Failed writing configuration: " + temporary);
    }
  }
  std::filesystem::permissions(
      temporary, std::filesystem::perms::owner_read |
                     std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);
  std::filesystem::rename(temporary, path);
}

}  // namespace

std::filesystem::path config_directory() {
  if (const char* custom = std::getenv("ACROSS_CONFIG_DIR");
      custom != nullptr && *custom != '\0') {
    return custom;
  }
#ifdef __APPLE__
  return home_directory() / "Library" / "Application Support" / "Across";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME");
      xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path(xdg) / "across";
  }
  return home_directory() / ".config" / "across";
#endif
}

std::filesystem::path known_hosts_path() {
  const auto directory = config_directory();
  make_private_directory(directory);
  const auto path = directory / "known_hosts";
#ifdef __APPLE__
  const auto legacy = home_directory() / "Library/Application";
  if (directory == home_directory() / "Library/Application Support/Across" &&
      !std::filesystem::exists(path) && std::filesystem::is_regular_file(legacy)) {
    std::filesystem::copy_file(legacy, path);
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  }
#endif
  return path;
}

ConfigStore::ConfigStore(std::filesystem::path path)
    : path_(path.empty() ? config_directory() / "servers.json"
                         : std::move(path)) {}

std::vector<ServerProfile> ConfigStore::list_servers() const {
  const auto json = read_json_or_empty(path_);
  return json.at("servers").get<std::vector<ServerProfile>>();
}

ServerProfile ConfigStore::get_server(const std::string& id) const {
  const auto servers = list_servers();
  const auto found = std::find_if(
      servers.begin(), servers.end(),
      [&id](const ServerProfile& profile) { return profile.id == id; });
  if (found == servers.end()) {
    throw std::runtime_error("Unknown server profile: " + id);
  }
  return *found;
}

void ConfigStore::save_server(const ServerProfile& profile) {
  validate_server_profile(profile);
  auto json = read_json_or_empty(path_);
  auto servers = json.at("servers").get<std::vector<ServerProfile>>();
  const auto found = std::find_if(
      servers.begin(), servers.end(), [&profile](const ServerProfile& candidate) {
        return candidate.id == profile.id;
      });
  if (found == servers.end()) {
    servers.push_back(profile);
  } else {
    *found = profile;
  }
  json["servers"] = servers;
  write_json_atomically(path_, json);
}

void ConfigStore::remove_server(const std::string& id) {
  auto json = read_json_or_empty(path_);
  auto servers = json.at("servers").get<std::vector<ServerProfile>>();
  const auto original_size = servers.size();
  std::erase_if(servers,
                [&id](const ServerProfile& profile) { return profile.id == id; });
  if (servers.size() == original_size) {
    throw std::runtime_error("Unknown server profile: " + id);
  }
  json["servers"] = servers;
  write_json_atomically(path_, json);
}

ProjectManifest load_manifest(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot open manifest: " + path.string());
  }
  auto manifest = nlohmann::json::parse(input).get<ProjectManifest>();
  if (manifest.project_root.is_relative()) {
    manifest.project_root =
        std::filesystem::weakly_canonical(path.parent_path() / manifest.project_root);
  }
  validate_manifest(manifest);
  return manifest;
}

std::string slugify(const std::string& input) {
  std::string slug;
  slug.reserve(input.size());
  for (const char value : input) {
    const auto character = static_cast<unsigned char>(value);
    if (std::isalnum(character) != 0) {
      slug.push_back(static_cast<char>(std::tolower(character)));
    } else if ((character == '-' || character == '_' || character == ' ') &&
               !slug.empty() && slug.back() != '-') {
      slug.push_back('-');
    }
  }
  while (!slug.empty() && slug.back() == '-') {
    slug.pop_back();
  }
  return slug;
}

void validate_server_profile(const ServerProfile& profile) {
  (void)package_plan(profile.packages);
  (void)package_plan(profile.custom_packages);
  auto catalog = profile.packages;
  for (const auto& name : profile.custom_packages) {
    if (std::find(catalog.begin(), catalog.end(), name) == catalog.end()) catalog.push_back(name);
  }
  (void)package_plan(catalog);
  static const std::regex identifier("^[a-z0-9][a-z0-9-]{0,62}$");
  static const std::regex hostname("^[A-Za-z0-9][A-Za-z0-9.:_-]{0,252}$");
  static const std::regex username("^[A-Za-z_][A-Za-z0-9_.-]{0,31}$");
  if (!std::regex_match(profile.id, identifier)) {
    throw std::runtime_error("Server id must be a lowercase slug");
  }
  if (profile.label.empty() || profile.label.size() > 100) {
    throw std::runtime_error("Server label must contain 1-100 characters");
  }
  if (!std::regex_match(profile.host, hostname)) {
    throw std::runtime_error("Invalid server hostname or address");
  }
  if (!std::regex_match(profile.user, username)) {
    throw std::runtime_error("Invalid SSH username");
  }
  if (profile.transfer_protocol != "rsync" && profile.transfer_protocol != "scp" && profile.transfer_protocol != "sftp") throw std::runtime_error("Transfer protocol must be rsync, scp, or sftp");
  if (profile.port == 0) {
    throw std::runtime_error("SSH port must be between 1 and 65535");
  }
  if (!profile.identity_file.empty() &&
      !std::filesystem::is_regular_file(profile.identity_file)) {
    throw std::runtime_error("SSH identity file does not exist");
  }
}

void validate_manifest(const ProjectManifest& manifest) {
  static const std::regex identifier("^[a-z0-9][a-z0-9-]{0,62}$");
  static const std::regex upstream(
      "^(?:[A-Za-z0-9.-]+|\\[[0-9A-Fa-f:]+\\]):[1-9][0-9]{0,4}$");
  static const std::regex remote_path(
      "^/[A-Za-z0-9._/@+:-]+(?:/[A-Za-z0-9._@+:-]+)*$");
  if (manifest.version != 1) {
    throw std::runtime_error("Unsupported manifest version");
  }
  if (!std::regex_match(manifest.app_id, identifier)) {
    throw std::runtime_error("app_id must be a lowercase slug");
  }
  if (manifest.type != "node" && manifest.type != "static" &&
      manifest.type != "proxy") {
    throw std::runtime_error("type must be node, static, or proxy");
  }
  if (!std::filesystem::is_directory(manifest.project_root)) {
    throw std::runtime_error("project_root is not a directory: " +
                             manifest.project_root.string());
  }
  if (manifest.build_location != "local" && manifest.build_location != "vps") {
    throw std::runtime_error("build_location must be local or vps");
  }
  const std::filesystem::path deployment_dir{manifest.deployment_dir};
  if (deployment_dir.empty() || deployment_dir.is_absolute()) {
    throw std::runtime_error("deployment_dir must be relative to the project");
  }
  for (const auto& component : deployment_dir) {
    if (component == "..") throw std::runtime_error("deployment_dir must stay inside the project");
  }
  if (manifest.port < 1024 || manifest.port > 65535) {
    throw std::runtime_error("Application port must be between 1024 and 65535");
  }
  if (manifest.health_path.empty() || manifest.health_path.front() != '/' ||
      manifest.health_path.find_first_of("\r\n") != std::string::npos) {
    throw std::runtime_error("health_path must begin with / and fit on one line");
  }
  if (manifest.keep_releases < 2 || manifest.keep_releases > 50) {
    throw std::runtime_error("keep_releases must be between 2 and 50");
  }
  if (manifest.health_timeout_seconds < 1 ||
      manifest.health_timeout_seconds > 600) {
    throw std::runtime_error("health_timeout_seconds must be between 1 and 600");
  }
  if (manifest.nginx.domains.empty()) {
    throw std::runtime_error("At least one nginx domain is required");
  }
  for (const auto& value : manifest.nginx.domains) {
    validate_domain(value, true);
  }
  for (const auto& value : manifest.nginx.upstreams) {
    if (!std::regex_match(value, upstream)) {
      throw std::runtime_error("Invalid upstream: " + value);
    }
    const auto separator = value.rfind(':');
    const int upstream_port = std::stoi(value.substr(separator + 1));
    if (upstream_port > 65535) {
      throw std::runtime_error("Invalid upstream port: " + value);
    }
  }
  if (manifest.type == "proxy" && manifest.nginx.upstreams.empty()) {
    throw std::runtime_error("proxy applications require at least one upstream");
  }
  if (manifest.type != "proxy" && !manifest.nginx.upstreams.empty()) {
    throw std::runtime_error("upstreams are only valid for proxy applications");
  }
  if (manifest.nginx.rate_limit_rps < 0 ||
      manifest.nginx.rate_limit_rps > 100000 ||
      manifest.nginx.rate_limit_burst < 1 ||
      manifest.nginx.rate_limit_burst > 100000) {
    throw std::runtime_error("Invalid Nginx rate limit");
  }
  if (manifest.nginx.client_max_body_mb < 1 ||
      manifest.nginx.client_max_body_mb > 10240) {
    throw std::runtime_error("client_max_body_mb must be between 1 and 10240");
  }
  if (manifest.nginx.tls_certificate.empty() !=
      manifest.nginx.tls_certificate_key.empty()) {
    throw std::runtime_error("Both TLS certificate and key must be configured");
  }
  if (!manifest.nginx.tls_certificate.empty() &&
      (!std::regex_match(manifest.nginx.tls_certificate, remote_path) ||
       !std::regex_match(manifest.nginx.tls_certificate_key, remote_path))) {
    throw std::runtime_error("TLS certificate paths must be safe absolute paths");
  }
  const auto artifact = std::filesystem::path(manifest.artifact_dir);
  if (artifact.is_absolute() || manifest.artifact_dir.find("..") != std::string::npos) {
    throw std::runtime_error("artifact_dir must stay inside the release");
  }
}

}  // namespace across
