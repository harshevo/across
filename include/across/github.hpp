#pragma once
#include "across/models.hpp"
#include "across/management.hpp"
#include "across/process.hpp"

namespace across {
void validate_github_repository(const std::string& repository);
ProcessResult connect_github(const OutputCallback& output = {});
nlohmann::json github_repositories();
ProjectManifest load_github_manifest(const std::filesystem::path& repository, const std::string& manifest);
std::filesystem::path clone_github_project(const std::string& repository, const std::string& branch, const OutputCallback& output = {});
ProcessResult deploy_github_project(const ServerProfile& server, const std::string& repository,
    const std::string& branch, const std::filesystem::path& project,
    const std::string& manifest, const OutputCallback& output = {}, const EnvironmentKeys& environment = {});
ProcessResult github_runs(const std::string& repository, const std::string& branch);
}
