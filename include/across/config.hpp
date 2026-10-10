#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "across/models.hpp"

namespace across {

std::filesystem::path config_directory();
std::filesystem::path known_hosts_path();

class ConfigStore {
 public:
  explicit ConfigStore(std::filesystem::path path = {});

  [[nodiscard]] std::vector<ServerProfile> list_servers() const;
  [[nodiscard]] ServerProfile get_server(const std::string& id) const;
  void save_server(const ServerProfile& profile);
  void remove_server(const std::string& id);

 private:
  std::filesystem::path path_;
};

ProjectManifest load_manifest(const std::filesystem::path& path);
void validate_server_profile(const ServerProfile& profile);
void validate_manifest(const ProjectManifest& manifest);
std::string slugify(const std::string& input);

}  // namespace across
