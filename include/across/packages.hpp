#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace across {
struct PackagePlan {
  std::vector<std::string> apt_packages;
  bool pm2{false};
  bool nginx{false};
  bool docker{false};
};
void validate_package_name(const std::string& name);
std::vector<std::string> parse_package_names(const std::string& value);
PackagePlan package_plan(const std::vector<std::string>& selected);
nlohmann::json parse_os_release(const std::string& contents);
std::string bootstrap_prerequisites_script();
}
