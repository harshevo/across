#include "across/management.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include "across/deployer.hpp"
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <nlohmann/json.hpp>
namespace across {
void validate_domain(const std::string& value, bool wildcard) {
  std::string domain = value;
  if (wildcard && domain.starts_with("*.")) domain.erase(0, 2);
  if (domain.empty() || domain.size() > 253) throw std::runtime_error("Invalid domain length");
  static const std::regex label("^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$");
  std::istringstream stream(domain);
  std::string part;
  while (std::getline(stream, part, '.')) {
    if (!std::regex_match(part, label)) throw std::runtime_error("Invalid domain: " + value);
  }
  if (domain.ends_with('.')) throw std::runtime_error("Domain must not end with a dot");
}
std::vector<std::string> parse_domains(const std::string& value) {
  std::string normalized = value;
  for (auto& character : normalized) if (character == ',') character = ' ';
  std::istringstream stream(normalized);
  std::vector<std::string> domains;
  std::string domain;
  while (stream >> domain) {
    validate_domain(domain, true);
    if (std::find(domains.begin(), domains.end(), domain) == domains.end()) domains.push_back(domain);
  }
  if (domains.empty()) throw std::runtime_error("Enter at least one domain");
  return domains;
}
void validate_firewall_rule(const std::string& action, const std::string& port, const std::string& protocol) {
  if (action != "allow" && action != "deny" && action != "delete") throw std::runtime_error("Invalid firewall action");
  if (protocol != "tcp" && protocol != "udp") throw std::runtime_error("Protocol must be tcp or udp");
  if (!std::regex_match(port, std::regex("^[0-9]{1,5}$")) || std::stoi(port) < 1 || std::stoi(port) > 65535)
    throw std::runtime_error("Port must be between 1 and 65535");
}
void validate_environment_key(const std::string& name) {
  static const std::regex valid("^[A-Z_][A-Z0-9_]{0,99}$");
  if (!std::regex_match(name, valid)) throw std::runtime_error("Environment key must use uppercase letters, digits and underscores, and cannot start with a digit");
  for (const auto* prefix : {"GITHUB_", "RUNNER_", "ACTIONS_", "ACROSS_", "DEPLOY_"})
    if (name.starts_with(prefix)) throw std::runtime_error("Environment key uses a reserved deployment prefix");
  for (const auto* reserved : {"PATH", "HOME", "USER", "LOGNAME", "SHELL", "BASH_ENV", "ENV", "IFS", "LD_PRELOAD", "LD_LIBRARY_PATH", "SHELLOPTS", "BASHOPTS", "PM2_HOME", "PORT", "CI"})
    if (name == reserved) throw std::runtime_error("Environment key is reserved by the deployment tools");
}
EnvironmentKeys load_environment_keys(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Cannot open environment file");
  const auto data = nlohmann::json::parse(input, nullptr, false);
  if (!data.is_object() || data.size() > 95) throw std::runtime_error("Environment must be an object with at most 95 keys");
  EnvironmentKeys values;
  for (const auto& [name, value] : data.items()) {
    validate_environment_key(name);
    if (!value.is_string()) throw std::runtime_error("Environment values must be strings");
    auto text = value.get<std::string>();
    if (text.size() > 48 * 1024 || text.find('\0') != std::string::npos) throw std::runtime_error("Environment value exceeds 48 KB or contains a null character");
    values.emplace(name, std::move(text));
  }
  return values;
}
std::string pipeline_workflow(const std::string& branch, const std::string& manifest, const std::vector<std::string>& environment) {
  if (!std::regex_match(branch, std::regex("^[A-Za-z0-9][A-Za-z0-9._/-]{0,99}$")) || branch.find("..") != std::string::npos || branch.find("//") != std::string::npos || branch.ends_with("/") || branch.ends_with(".") || branch.ends_with(".lock")) throw std::runtime_error("Invalid branch");
  if (!std::regex_match(manifest, std::regex("^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$")) || manifest.find("..") != std::string::npos)
    throw std::runtime_error("Manifest must be a safe relative repository path");
  if (environment.size() > 95) throw std::runtime_error("At most 95 application environment keys are supported");
  std::string env;
  for (const auto& key : environment) {
    validate_environment_key(key);
    env += "          " + key + ": ${{ secrets.ACROSS_ENV_" + key + " }}\n";
  }
  std::ostringstream workflow;
  workflow << "name: Deploy with Across\non:\n  push:\n    branches: ['" << branch << "']\n  workflow_dispatch:\n"
      << R"YAML(permissions:
  contents: read
concurrency:
  group: across-production
  cancel-in-progress: false
jobs:
  deploy:
    runs-on: ubuntu-latest
    environment: production
    steps:
      - uses: actions/checkout@v4
      - name: Build C++ deployment client
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake g++ rsync
          cmake -S .across/client -B .across/build -DACROSS_BUILD_DESKTOP=OFF -DACROSS_BUILD_TESTS=OFF -DACROSS_BUILD_SERVER=OFF
          cmake --build .across/build --parallel 2
      - name: Deploy
        env:
          ACROSS_REQUIRE_KNOWN_HOST: '1'
          DEPLOY_HOST: ${{ secrets.ACROSS_HOST }}
          DEPLOY_USER: ${{ secrets.ACROSS_USER }}
          DEPLOY_PORT: ${{ secrets.ACROSS_PORT }}
          DEPLOY_KEY: ${{ secrets.ACROSS_SSH_KEY }}
          DEPLOY_HOST_KEY: ${{ secrets.ACROSS_KNOWN_HOSTS }}
)YAML" << env << R"YAML(        shell: bash
        run: |
          set -euo pipefail
          umask 077
          test -n "$DEPLOY_HOST_KEY"
          mkdir -p ~/.config/across
          chmod 700 ~/.config/across
          trap 'rm -f ~/.config/across/ci-key ~/.config/across/environment.json' EXIT
          printf '%s\n' "$DEPLOY_KEY" > ~/.config/across/ci-key
          printf '%s\n' "$DEPLOY_HOST_KEY" > ~/.config/across/known_hosts
)YAML";
  if (!environment.empty()) {
    workflow << "          export ACROSS_ENV_FILE=~/.config/across/environment.json\n"
             << "          python3 - <<'PY'\n"
             << "          import json, os\n"
             << "          keys = " << nlohmann::json(environment).dump() << "\n"
             << "          with open(os.environ['ACROSS_ENV_FILE'], 'w') as output:\n"
             << "              json.dump({key: os.environ[key] for key in keys}, output)\n"
             << "          PY\n";
  }
  workflow << "          .across/build/across server add ci --host \"$DEPLOY_HOST\" --user \"${DEPLOY_USER:-ubuntu}\" --port \"${DEPLOY_PORT:-22}\" --identity ~/.config/across/ci-key\n"
           << "          .across/build/across deploy ci " << manifest << "\n";
  return workflow.str();
}
std::filesystem::path create_pipeline(const std::filesystem::path& project, const std::string& branch, const std::string& manifest, const std::vector<std::string>& environment) {
  const auto workflow = pipeline_workflow(branch, manifest, environment);
  if (!std::filesystem::is_regular_file(project / manifest)) throw std::runtime_error("Project manifest does not exist");
  const auto path = project / ".github/workflows/across-deploy.yml";
  if (std::filesystem::exists(path) || std::filesystem::exists(project / ".across/client")) throw std::runtime_error("Pipeline already exists; move the existing workflow and .across/client before regenerating");
  const auto source = server_source_directory();
  std::filesystem::create_directories(project / ".across/client/src");
  for (const auto& directory : {"core", "cli"}) std::filesystem::copy(source / "src" / directory, project / ".across/client/src" / directory, std::filesystem::copy_options::recursive);
  std::filesystem::copy(source / "include", project / ".across/client/include", std::filesystem::copy_options::recursive);
  std::filesystem::copy_file(source / "CMakeLists.txt", project / ".across/client/CMakeLists.txt");
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << workflow;
  if (!output) throw std::runtime_error("Failed to write workflow");
  return path;
}
ProcessResult point_cloudflare_domain(const std::string& zone, const std::string& domain, const std::string& address, const std::filesystem::path& token_file) {
  validate_domain(domain);
  if (!std::regex_match(zone, std::regex("^[a-fA-F0-9]{32}$"))) throw std::runtime_error("Cloudflare zone ID must be 32 hexadecimal characters");
  unsigned char buffer[16];
  const bool ipv4 = inet_pton(AF_INET, address.c_str(), buffer) == 1;
  if (!ipv4 && inet_pton(AF_INET6, address.c_str(), buffer) != 1) throw std::runtime_error("DNS target must be an IPv4 or IPv6 address");
  std::ifstream input(token_file);
  std::string token; std::getline(input, token);
  if (!input || !std::regex_match(token, std::regex("^[A-Za-z0-9_-]{20,200}$"))) throw std::runtime_error("Token file must contain a Cloudflare API token");
  std::string temporary = (std::filesystem::temp_directory_path() / "across-dns-XXXXXX").string();
  std::vector<char> name(temporary.begin(), temporary.end()); name.push_back('\0');
  const int descriptor = mkstemp(name.data());
  if (descriptor < 0) throw std::runtime_error("Cannot create private DNS credential file");
  close(descriptor);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{name.data()};
  std::ofstream header(cleanup.path); header << "Authorization: Bearer " << token << '\n'; header.close();
  const std::string url = "https://api.cloudflare.com/client/v4/zones/" + zone + "/dns_records";
  auto request = [&](const std::vector<std::string>& extra) {
    std::vector<std::string> args{"curl", "--silent", "--show-error", "--fail-with-body", "--max-time", "30", "--header", "@" + cleanup.path.string(), "--header", "Content-Type: application/json"};
    args.insert(args.end(), extra.begin(), extra.end());
    return run_process(args, std::chrono::seconds{40});
  };
  const auto listed = request({url + "?name=" + domain + "&type=" + (ipv4 ? "A" : "AAAA")});
  if (!listed.ok()) return listed;
  const auto response = nlohmann::json::parse(listed.output);
  if (!response.value("success", false)) throw std::runtime_error("Cloudflare rejected DNS lookup");
  const auto records = response.at("result");
  if (records.size() > 1) throw std::runtime_error("Multiple DNS records exist; resolve them before pointing this domain");
  const auto payload = nlohmann::json{{"type", ipv4 ? "A" : "AAAA"}, {"name", domain}, {"content", address}, {"ttl", 1}, {"proxied", false}}.dump();
  auto result = request({"--request", records.empty() ? "POST" : "PUT", "--data", payload, records.empty() ? url : url + "/" + records.at(0).at("id").get<std::string>()});
  if (result.ok() && !nlohmann::json::parse(result.output).value("success", false)) result.exit_code = 1;
  return result;
}
}
