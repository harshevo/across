#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace across {

struct ServerProfile {
  std::string id;
  std::string label;
  std::string host;
  std::string user{"ubuntu"};
  std::uint16_t port{22};
  std::string identity_file;
  std::string transfer_protocol{"rsync"};
  std::vector<std::string> packages{"nginx", "pm2", "node", "npm"};
  std::vector<std::string> custom_packages{};
};

struct NginxConfig {
  std::vector<std::string> domains;
  std::vector<std::string> upstreams;
  int rate_limit_rps{0};
  int rate_limit_burst{20};
  int client_max_body_mb{20};
  std::string tls_certificate;
  std::string tls_certificate_key;
  bool redirect_http_to_https{false};
};

struct ProjectManifest {
  int version{1};
  std::string app_id;
  std::string name;
  std::string type{"node"};
  std::filesystem::path project_root;
  std::string install_command{"npm ci"};
  std::string build_command{"npm run build"};
  std::string build_location{"vps"};
  std::string deployment_dir{"."};
  std::string start_command{"npm start"};
  std::string artifact_dir{"dist"};
  int port{3000};
  std::string health_path{"/"};
  int health_timeout_seconds{30};
  int keep_releases{5};
  std::vector<std::string> excludes;
  NginxConfig nginx;
};

inline void to_json(nlohmann::json& json, const ServerProfile& profile) {
  json = nlohmann::json{{"id", profile.id},
                        {"label", profile.label},
                        {"host", profile.host},
                        {"user", profile.user},
                        {"port", profile.port},
                        {"identity_file", profile.identity_file},
                        {"transfer_protocol", profile.transfer_protocol},
                        {"packages", profile.packages},
                        {"custom_packages", profile.custom_packages}};
}

inline void from_json(const nlohmann::json& json, ServerProfile& profile) {
  json.at("id").get_to(profile.id);
  json.at("label").get_to(profile.label);
  json.at("host").get_to(profile.host);
  profile.user = json.value("user", "ubuntu");
  profile.port = json.value("port", static_cast<std::uint16_t>(22));
  profile.identity_file = json.value("identity_file", "");
  profile.transfer_protocol = json.value("transfer_protocol", "rsync");
  profile.packages = json.value("packages", std::vector<std::string>{"nginx", "pm2", "node", "npm"});
  profile.custom_packages = json.value("custom_packages", std::vector<std::string>{});
}

inline void to_json(nlohmann::json& json, const NginxConfig& nginx) {
  json = nlohmann::json{{"domains", nginx.domains},
                        {"upstreams", nginx.upstreams},
                        {"rate_limit_rps", nginx.rate_limit_rps},
                        {"rate_limit_burst", nginx.rate_limit_burst},
                        {"client_max_body_mb", nginx.client_max_body_mb},
                        {"tls_certificate", nginx.tls_certificate},
                        {"tls_certificate_key", nginx.tls_certificate_key},
                        {"redirect_http_to_https", nginx.redirect_http_to_https}};
}

inline void from_json(const nlohmann::json& json, NginxConfig& nginx) {
  nginx.domains = json.value("domains", std::vector<std::string>{});
  nginx.upstreams = json.value("upstreams", std::vector<std::string>{});
  nginx.rate_limit_rps = json.value("rate_limit_rps", 0);
  nginx.rate_limit_burst = json.value("rate_limit_burst", 20);
  nginx.client_max_body_mb = json.value("client_max_body_mb", 20);
  nginx.tls_certificate = json.value("tls_certificate", "");
  nginx.tls_certificate_key = json.value("tls_certificate_key", "");
  nginx.redirect_http_to_https = json.value("redirect_http_to_https", false);
}

inline void to_json(nlohmann::json& json, const ProjectManifest& manifest) {
  json = nlohmann::json{{"version", manifest.version},
                        {"app_id", manifest.app_id},
                        {"name", manifest.name},
                        {"type", manifest.type},
                        {"project_root", manifest.project_root.string()},
                        {"install_command", manifest.install_command},
                        {"build_command", manifest.build_command},
                        {"build_location", manifest.build_location},
                        {"deployment_dir", manifest.deployment_dir},
                        {"start_command", manifest.start_command},
                        {"artifact_dir", manifest.artifact_dir},
                        {"port", manifest.port},
                        {"health_path", manifest.health_path},
                        {"health_timeout_seconds", manifest.health_timeout_seconds},
                        {"keep_releases", manifest.keep_releases},
                        {"excludes", manifest.excludes},
                        {"nginx", manifest.nginx}};
}

inline void from_json(const nlohmann::json& json, ProjectManifest& manifest) {
  manifest.version = json.value("version", 1);
  json.at("app_id").get_to(manifest.app_id);
  manifest.name = json.value("name", manifest.app_id);
  manifest.type = json.value("type", "node");
  manifest.project_root = json.value("project_root", ".");
  manifest.install_command = json.value("install_command", "npm ci");
  manifest.build_command = json.value("build_command", "npm run build");
  manifest.build_location = json.value("build_location", "vps");
  manifest.deployment_dir = json.value("deployment_dir", ".");
  manifest.start_command = json.value("start_command", "npm start");
  manifest.artifact_dir = json.value("artifact_dir", "dist");
  manifest.port = json.value("port", 3000);
  manifest.health_path = json.value("health_path", "/");
  manifest.health_timeout_seconds = json.value("health_timeout_seconds", 30);
  manifest.keep_releases = json.value("keep_releases", 5);
  manifest.excludes = json.value("excludes", std::vector<std::string>{});
  if (json.contains("nginx")) {
    manifest.nginx = json.at("nginx").get<NginxConfig>();
  }
}

}  // namespace across
