#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "across/models.hpp"
#include "across/process.hpp"
namespace across {
std::vector<std::string> parse_domains(const std::string& value);
void validate_domain(const std::string& value, bool wildcard = false);
void validate_firewall_rule(const std::string& action, const std::string& port, const std::string& protocol);
using EnvironmentKeys = std::map<std::string, std::string>;
void validate_environment_key(const std::string& name);
EnvironmentKeys load_environment_keys(const std::filesystem::path& path);
std::string pipeline_workflow(const std::string& branch, const std::string& manifest, const std::vector<std::string>& environment = {});
std::filesystem::path create_pipeline(const std::filesystem::path& project, const std::string& branch, const std::string& manifest, const std::vector<std::string>& environment = {});
ProcessResult point_cloudflare_domain(const std::string& zone, const std::string& domain, const std::string& address, const std::filesystem::path& token_file);
}
