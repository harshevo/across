#include "across/packages.hpp"
#include <algorithm>
#include <regex>
#include <sstream>
#include <stdexcept>
namespace across {
void validate_package_name(const std::string& name) {
  static const std::regex package("^[a-z0-9][a-z0-9+.-]{0,99}(?::[a-z0-9][a-z0-9-]{0,19})?$");
  if (!std::regex_match(name, package)) throw std::runtime_error("Use an Ubuntu/Debian package name, without options or version expressions");
}
std::vector<std::string> parse_package_names(const std::string& value) {
  std::istringstream input(value); std::string name; std::vector<std::string> names;
  while (input >> name) {
    validate_package_name(name);
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    if (names.size() > 100) throw std::runtime_error("Select at most 100 packages");
  }
  return names;
}
PackagePlan package_plan(const std::vector<std::string>& selected) {
  if (selected.size() > 100) throw std::runtime_error("Select at most 100 packages");
  PackagePlan plan;
  auto add = [&](const std::string& name) { if (std::find(plan.apt_packages.begin(), plan.apt_packages.end(), name) == plan.apt_packages.end()) plan.apt_packages.push_back(name); };
  for (const auto& name : selected) {
    validate_package_name(name);
    if (name == "pm2") { plan.pm2 = true; add("nodejs"); add("npm"); }
    else if (name == "node") add("nodejs");
    else if (name == "docker") { plan.docker = true; add("docker.io"); }
    else { add(name); if (name == "nginx") plan.nginx = true; if (name == "docker.io") plan.docker = true; }
  }
  return plan;
}
nlohmann::json parse_os_release(const std::string& contents) {
  nlohmann::json values = nlohmann::json::object();
  std::istringstream input(contents); std::string line;
  while (std::getline(input, line)) {
    const auto equals = line.find('=');
    if (equals == std::string::npos) continue;
    auto key = line.substr(0, equals), value = line.substr(equals + 1);
    if (!value.empty() && value.back() == '\r') value.pop_back();
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) value = value.substr(1, value.size() - 2);
    if (key == "ID" || key == "VERSION_ID" || key == "PRETTY_NAME") values[key] = value;
  }
  const auto id = values.value("ID", "");
  return {{"id", id}, {"version", values.value("VERSION_ID", "")}, {"name", values.value("PRETTY_NAME", id)}, {"supported", id == "ubuntu" || id == "debian"}};
}
std::string bootstrap_prerequisites_script() {
  return "set -eu\n"
         "if [ ! -r /etc/os-release ]; then echo 'Cannot detect VPS OS' >&2; exit 20; fi\n"
         ". /etc/os-release\n"
         "case \"$ID\" in ubuntu|debian) ;; *) echo 'Across supports Ubuntu and Debian' >&2; exit 20;; esac\n"
         "if [ \"$(id -u)\" -eq 0 ]; then SUDO=''; else sudo -n true; SUDO=sudo; fi\n"
         "$SUDO apt-get -o DPkg::Lock::Timeout=120 update\n"
         "$SUDO env DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=120 install -y build-essential cmake ca-certificates rsync curl\n";
}
}
