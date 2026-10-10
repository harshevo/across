#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#include "across/config.hpp"
#include "across/deployer.hpp"
#include "across/management.hpp"
#include "across/packages.hpp"
#include "across/transport.hpp"
#include "across/process.hpp"
#include "across/watcher.hpp"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const std::string& label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << '\n';
    ++failures;
  }
}

template <typename Function>
void check_throws(Function function, const std::string& label) {
  try {
    function();
    check(false, label);
  } catch (const std::exception&) {
    check(true, label);
  }
}

fs::path test_directory() {
  const fs::path path = fs::temp_directory_path() /
                        ("across-tests-" + std::to_string(getpid()));
  fs::remove_all(path);
  fs::create_directories(path);
  return path;
}

void test_slug_and_validation(const fs::path& directory) {
  check(across::slugify("My Production_Server") == "my-production-server",
        "slug normalization");

  across::ServerProfile profile{"prod", "Production", "203.0.113.8", "ubuntu", 22,
                                ""};
  across::validate_server_profile(profile);
  profile.host = "host; shutdown";
  check_throws([&] { across::validate_server_profile(profile); },
               "unsafe host rejected");

  across::ProjectManifest manifest;
  manifest.app_id = "sample";
  manifest.project_root = directory;
  manifest.nginx.domains = {"sample.example.com"};
  across::validate_manifest(manifest);
  manifest.artifact_dir = "../escape";
  check_throws([&] { across::validate_manifest(manifest); },
               "artifact traversal rejected");
  manifest.artifact_dir = "dist";
  manifest.nginx.tls_certificate = "/etc/certs/site.pem; include /tmp/bad";
  manifest.nginx.tls_certificate_key = "/etc/certs/site.key";
  check_throws([&] { across::validate_manifest(manifest); },
               "unsafe TLS path rejected");
  manifest.nginx.tls_certificate.clear();
  manifest.nginx.tls_certificate_key.clear();
  manifest.nginx.upstreams = {"127.0.0.1:4000"};
  check_throws([&] { across::validate_manifest(manifest); },
               "node upstream ambiguity rejected");
  manifest.type = "proxy";
  across::validate_manifest(manifest);
  manifest.build_location = "invalid";
  check_throws([&] { across::validate_manifest(manifest); }, "invalid build location rejected");
  manifest.build_location = "local";
  manifest.deployment_dir = "../escape";
  check_throws([&] { across::validate_manifest(manifest); }, "deployment directory traversal rejected");
  manifest.deployment_dir = ".next/standalone";
  across::validate_manifest(manifest);
  const auto restored = nlohmann::json(manifest).get<across::ProjectManifest>();
  check(restored.build_location == "local" && restored.deployment_dir == ".next/standalone", "local build settings round trip");
  auto legacy = nlohmann::json(manifest); legacy.erase("build_location"); legacy.erase("deployment_dir");
  check(legacy.get<across::ProjectManifest>().build_location == "vps", "legacy manifests build on VPS");
}

void test_config_store(const fs::path& directory) {
  across::ConfigStore store(directory / "servers.json");
  across::ServerProfile first{"one", "First", "server.example.com", "deploy", 2222,
                              ""};
  store.save_server(first);
  check(store.list_servers().size() == 1, "server saved");
  first.label = "Updated";
  store.save_server(first);
  check(store.get_server("one").label == "Updated", "server updated");
  store.remove_server("one");
  check(store.list_servers().empty(), "server removed");
}

void test_process() {
  const auto result = across::run_process({"sh", "-c", "printf 'hello world'"},
                                          std::chrono::seconds{2});
  check(result.ok() && result.output == "hello world", "process output captured");
  const auto timeout = across::run_process({"sh", "-c", "sleep 2"},
                                           std::chrono::seconds{1});
  check(timeout.timed_out && timeout.exit_code == 124, "process timeout enforced");
  check(across::shell_quote("a'b") == "'a'\\''b'", "shell quoting");
}

void test_watcher(const fs::path& directory) {
  const fs::path root = directory / "watch";
  fs::create_directories(root / ".git");
  std::ofstream(root / "index.txt") << "one";
  std::ofstream(root / ".git" / "ignored") << "one";
  across::FileWatcher watcher(root, {});
  check(!watcher.scan_changed(), "unchanged snapshot stable");
  std::this_thread::sleep_for(std::chrono::milliseconds{10});
  std::ofstream(root / ".git" / "ignored") << "two";
  check(!watcher.scan_changed(), "excluded file ignored");
  std::ofstream(root / "index.txt", std::ios::app) << "two";
  const auto modified = watcher.scan_changes();
  check(modified.size() == 1 &&
            modified.front().kind == across::ChangeKind::modified &&
            modified.front().path == "index.txt",
        "changed file identified");
  fs::remove(root / "index.txt");
  check(watcher.scan_changed(), "deleted file detected");
}

void test_management(const fs::path& directory) {
  const auto domains = across::parse_domains("example.com, www.example.com example.com");
  check(domains.size() == 2, "domain list deduplicated");
  for (const auto& domain : {"bad..example.com", "-bad.example.com", "bad-.example.com", "example.com;include", "example.com\nX"})
    check_throws([&] { across::validate_domain(domain); }, "invalid domain rejected");
  across::validate_domain("*.example.com", true);
  check_throws([] { across::validate_domain("*.example.com"); }, "wildcard certificate rejected");
  across::validate_firewall_rule("allow", "443", "tcp");
  check_throws([] { across::validate_firewall_rule("allow", "65536", "tcp"); }, "invalid firewall port rejected");
  check_throws([] { across::validate_firewall_rule("enable", "22", "tcp"); }, "unsupported firewall mutation rejected");
  check_throws([] { across::validate_firewall_rule("allow", "80;reboot", "tcp"); }, "firewall injection rejected");
  check_throws([] { across::pipeline_workflow("main'", "across.json"); }, "workflow branch injection rejected");
  check_throws([] { across::pipeline_workflow("main", "../across.json"); }, "workflow manifest traversal rejected");
  const auto project = directory / "pipeline";
  fs::create_directories(project); std::ofstream(project / "across.json") << "{}";
  const auto workflow = across::create_pipeline(project, "main", "across.json");
  check(fs::exists(workflow) && fs::exists(project / ".across/client/src/cli/main.cpp"), "workflow includes C++ client source");
  check_throws([&] { across::create_pipeline(project, "main", "across.json"); }, "existing pipeline preserved");
  const auto content = across::pipeline_workflow("main", "across.json");
  check(content.find("ACROSS_KNOWN_HOSTS") != std::string::npos && content.find("set -euo pipefail") != std::string::npos, "pipeline verifies host key and stops on failure");
  check_throws([&] { across::point_cloudflare_domain("wrong", "example.com", "1.2.3.4", directory / "token"); }, "invalid DNS zone rejected before network");
}

void test_transfers(const fs::path& directory) {
  const auto bin = directory / "bin"; fs::create_directories(bin);
  for (const auto& command : {"ssh", "scp", "sftp", "rsync"}) {
    const auto executable = bin / command;
    std::ofstream output(executable);
    output << "#!/bin/sh\nprintf '%s\\n' \"$@\"\n";
    if (std::string(command) == "sftp") output << "if [ \"$1\" = '-b' ]; then cat \"$2\"; fi\n";
    output.close(); fs::permissions(executable, fs::perms::owner_all);
  }
  const std::string previous_path = std::getenv("PATH");
  setenv("PATH", (bin.string() + ":" + previous_path).c_str(), 1);
  const auto local = directory / "a file.txt"; std::ofstream(local) << "hello";
  across::ServerProfile profile{"test", "Test", "server.example.com", "deploy", 2222, ""};
  for (const auto& protocol : {"rsync", "scp", "sftp"}) {
    profile.transfer_protocol = protocol;
    across::SshTransport transport(profile);
    const auto result = transport.upload(local, "/tmp/a-file.txt");
    check(result.ok(), std::string(protocol) + " upload command succeeds");
    if (std::string(protocol) == "sftp") check(result.output.find("put -R") != std::string::npos && result.output.find("a file.txt\"") != std::string::npos, "SFTP batch quotes local spaces");
    if (std::string(protocol) == "scp") check(result.output.find("-O\n-r") != std::string::npos && result.output.find("-P\n2222") != std::string::npos, "SCP uses explicit protocol and SSH port");
    check(transport.download("/tmp/a-file.txt", directory / "download").ok(), std::string(protocol) + " download command succeeds");
    check_throws([&] { transport.upload(local, "/tmp/../etc/key"); }, "transfer traversal rejected");
  }
  setenv("ACROSS_REQUIRE_KNOWN_HOST", "1", 1);
  check(across::SshTransport(profile).execute({"true"}).output.find("StrictHostKeyChecking=yes") != std::string::npos, "CI requires an existing verified host key");
  unsetenv("ACROSS_REQUIRE_KNOWN_HOST");
  profile.transfer_protocol = "rsync";
  const auto folder = directory / "folder"; fs::create_directories(folder);
  const auto uploaded_folder = across::SshTransport(profile).upload(folder, "/tmp/existing-folder");
  check(uploaded_folder.ok() && uploaded_folder.output.find(folder.string() + "/\n") != std::string::npos && uploaded_folder.output.find("/tmp/existing-folder/\n") != std::string::npos, "directory upload copies contents into exact destination");
  const auto downloaded_folder = across::SshTransport(profile).download("/tmp/existing-folder", directory / "folder-download");
  check(downloaded_folder.ok() && downloaded_folder.output.find("/tmp/existing-folder/\n") != std::string::npos && downloaded_folder.output.find((directory / "folder-download").string() + "/\n") != std::string::npos, "directory download never nests an existing destination");
  across::ProjectManifest manifest;
  manifest.app_id = "local-test"; manifest.project_root = directory / "local project";
  fs::create_directories(manifest.project_root);
  manifest.nginx.domains = {"local.example.com"}; manifest.build_location = "local";
  manifest.deployment_dir = "bundle";
  manifest.install_command = "printf installed > installed.txt";
  manifest.build_command = "test -f installed.txt && mkdir -p bundle/node_modules && printf built > bundle/server.js";
  std::string output;
  const auto deployed = across::Deployer(profile).deploy(manifest, [&](const std::string& chunk) { output += chunk; });
  check(deployed.ok() && fs::exists(manifest.project_root / "bundle/server.js"), "local install and build run in project folder with spaces");
  check(output.find((manifest.project_root / "bundle").string()) != std::string::npos && output.find("--exclude=node_modules") == std::string::npos, "local runtime bundle includes dependencies");
  manifest.build_command = "exit 7"; output.clear();
  check(across::Deployer(profile).deploy(manifest, [&](const std::string& chunk) { output += chunk; }).exit_code == 7 && output.find("BatchMode") == std::string::npos, "failed local build never connects or deploys");
  manifest.build_command = ""; manifest.install_command = "";
  manifest.deployment_dir = "missing";
  check_throws([&] { across::Deployer(profile).deploy(manifest); }, "missing local bundle rejected");
  manifest.deployment_dir = "outside";
  fs::create_directory_symlink(fs::temp_directory_path(), manifest.project_root / "outside");
  check_throws([&] { across::Deployer(profile).deploy(manifest); }, "local bundle symlink escape rejected");
  setenv("PATH", previous_path.c_str(), 1);
}

void test_packages(const fs::path& directory) {
  const auto ubuntu = across::parse_os_release("ID=ubuntu\nVERSION_ID=\"24.04\"\nPRETTY_NAME=\"Ubuntu 24.04 LTS\"\n");
  check(ubuntu.at("supported").get<bool>() && ubuntu.at("id") == "ubuntu", "Ubuntu OS detected");
  check(across::parse_os_release("ID=debian\nVERSION_ID=13\n").at("supported").get<bool>(), "Debian OS detected");
  check(!across::parse_os_release("ID=fedora\nID_LIKE=debian\n").at("supported").get<bool>(), "unsupported OS rejected despite ID_LIKE");
  check(!across::parse_os_release("").at("supported").get<bool>(), "missing OS ID not treated as supported");
  const auto plan = across::package_plan({"pm2", "node", "npm", "docker", "htop", "htop"});
  check(plan.pm2 && plan.docker && !plan.nginx, "deselected Nginx omitted");
  check(plan.apt_packages == std::vector<std::string>{"nodejs", "npm", "docker.io", "htop"}, "preset mapping and dependency deduplication");
  check(across::package_plan({}).apt_packages.empty(), "empty selection preserves deselection");
  check(across::parse_package_names("htop curl htop") == std::vector<std::string>{"htop", "curl"}, "custom package list deduplicated");
  for (const auto& name : {"--allow-unauthenticated", "curl;reboot", "pkg=1.0", "$(reboot)", "../pkg", "https://example.com/pkg"})
    check_throws([&] { across::validate_package_name(name); }, "unsafe package expression rejected");
  across::validate_package_name("libssl-dev"); across::validate_package_name("g++"); across::validate_package_name("libc6:amd64");
  across::ConfigStore store(directory / "package-profile.json");
  across::ServerProfile profile{"packages", "Packages", "server.example.com", "ubuntu", 22, ""};
  profile.packages = {"docker", "htop"}; profile.custom_packages = {"htop", "curl"}; store.save_server(profile);
  check(store.get_server("packages").packages == profile.packages && store.get_server("packages").custom_packages == profile.custom_packages, "package choices and deselected custom packages persist");
  auto json = nlohmann::json(profile); json.erase("packages"); json.erase("custom_packages");
  check(json.get<across::ServerProfile>().packages.size() == 4, "existing profiles receive default package choices");
  const auto bootstrap = across::bootstrap_prerequisites_script();
  check(bootstrap.find("ubuntu|debian") != std::string::npos && bootstrap.find("DEBIAN_FRONTEND=noninteractive") != std::string::npos && bootstrap.find("rsync") != std::string::npos, "bootstrap detects OS and installs transfer prerequisites");
}

}  // namespace

int main() {
  const fs::path directory = test_directory();
  try {
    test_slug_and_validation(directory);
    test_config_store(directory);
    test_process();
    test_watcher(directory);
    test_management(directory);
    test_transfers(directory);
    test_packages(directory);
  } catch (const std::exception& error) {
    std::cerr << "Unexpected exception: " << error.what() << '\n';
    ++failures;
  }
  fs::remove_all(directory);
  if (failures == 0) {
    std::cout << "All tests passed\n";
    return 0;
  }
  std::cerr << failures << " test(s) failed\n";
  return 1;
}
