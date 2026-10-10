#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "across/models.hpp"
#include "across/process.hpp"
namespace across {
std::vector<std::string> parse_domains(const std::string& value);
void validate_domain(const std::string& value, bool wildcard = false);
void validate_firewall_rule(const std::string& action, const std::string& port, const std::string& protocol);
std::string pipeline_workflow(const std::string& branch, const std::string& manifest);
std::filesystem::path create_pipeline(const std::filesystem::path& project, const std::string& branch, const std::string& manifest);
ProcessResult point_cloudflare_domain(const std::string& zone, const std::string& domain, const std::string& address, const std::filesystem::path& token_file);
}
