#include "across/github.hpp"
#include "across/config.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
namespace fs = std::filesystem;
namespace {
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string read(const fs::path& path) { std::ifstream input(path); return {std::istreambuf_iterator<char>(input), {}}; }
void write(const fs::path& path, const std::string& text, bool executable = false) {
  std::ofstream file(path); file << text; file.close();
  if (executable) fs::permissions(path, fs::perms::owner_all);
}
void run(const std::vector<std::string>& command) {
  const auto result = across::run_process(command, std::chrono::seconds{30}); require(result.ok(), result.output);
}
template<class F> void rejects(F function, const std::string& message) {
  bool rejected = false; try { function(); } catch (const std::exception&) { rejected = true; } require(rejected, message);
}
}
int main() {
  const auto root = fs::temp_directory_path() / ("across-github-tests-" + std::to_string(getpid()));
  fs::remove_all(root); fs::create_directories(root / "bin"); fs::create_directories(root / "source/frontend");
  const std::string previous_path = std::getenv("PATH");
  const auto* original_config = std::getenv("ACROSS_CONFIG_DIR");
  const std::string previous_config = original_config ? original_config : "";
  int status = 0;
  try {
    setenv("ACROSS_CONFIG_DIR", (root / "config").c_str(), 1);
    setenv("ACROSS_TEST_ROOT", root.c_str(), 1);
    write(root / "bin/gh", R"SH(#!/bin/sh
set -eu
case "$1 $2" in
  'auth login')
    case " $* " in *" --web "*) ;; *) exit 2;; esac
    case " $* " in *" --clipboard "*) ;; *) exit 2;; esac
    printf 'Device code: ABCD-1234\nOpen this URL to continue in your web browser: https://github.com/login/'
    sleep 0.1
    printf 'device\nOpen this URL to continue in your web browser: https://github.com/login/device\n'
    exit 0;;
  'repo clone')
    if [ -f "$ACROSS_TEST_ROOT/fail-clone" ]; then exit 1; fi
    exec git clone --single-branch --branch "$8" "$ACROSS_TEST_ROOT/remote.git" "$4";;
  'secret set')
    if [ -f "$ACROSS_TEST_ROOT/fail-secret" ]; then exit 1; fi
    cat > /dev/null
    printf '%s\n' "$3" >> "$ACROSS_TEST_ROOT/secrets"
    exit 0;;
  'run list') printf 'completed success Deploy with Across\n'; exit 0;;
esac
case "$*" in
  *user/repos*) if [ -f "$ACROSS_TEST_ROOT/fail-repos" ]; then printf 'Repository lookup failed'; exit 1; fi; printf '[[{"full_name":"owner/site","default_branch":"main","permissions":{"push":true}},{"full_name":"owner/archived","archived":true,"permissions":{"push":true}}]]';;
  *repos/owner/site*)
    if [ -f "$ACROSS_TEST_ROOT/no-admin" ]; then printf '{"permissions":{"admin":false}}'; else printf '{"permissions":{"admin":true}}'; fi;;
  *user*) printf '{"login":"tester"}';;
  *) exit 2;;
esac
)SH", true);
    const auto browser_script = R"SH(#!/bin/sh
printf '%s\n' "$1" >> "$ACROSS_TEST_ROOT/browser-log"
)SH";
    write(root / "bin/open", browser_script, true);
    write(root / "bin/xdg-open", browser_script, true);
    write(root / "bin/ssh", R"SH(#!/bin/sh
if [ -f "$ACROSS_TEST_ROOT/fail-ssh" ]; then exit 1; fi
printf 'ssh checked\n' >> "$ACROSS_TEST_ROOT/ssh-log"
printf '{"ok":true,"environment_keys":true}\n'
)SH", true);
    setenv("PATH", ((root / "bin").string() + ":" + previous_path).c_str(), 1);
    run({"git", "init", "-b", "main", (root / "source").string()});
    write(root / "source/README.md", "preserve this file\n");
    across::ProjectManifest manifest;
    manifest.app_id = "site"; manifest.type = "static"; manifest.project_root = root / "source/frontend";
    manifest.nginx.domains = {"example.com"};
    manifest.install_command.clear();
    manifest.project_root = "/old-machine/projects/site";
    write(root / "source/across.json", nlohmann::json(manifest).dump());
    write(root / "source/frontend/across.json", nlohmann::json(manifest).dump());
    write(root / "source/frontend/package.json", "{}"); write(root / "source/frontend/package-lock.json", "{}");
    const auto imported = across::load_github_manifest(root / "source", "frontend/across.json");
    require(imported.project_root == fs::canonical(root / "source/frontend") && imported.install_command == "npm ci", "Old absolute root must rebase before validation and install dependencies for CI");
    manifest.project_root = ".";
    write(root / "source/frontend/across.json", nlohmann::json(manifest).dump());
    require(across::load_github_manifest(root / "source", "frontend/across.json").project_root == fs::canonical(root / "source/frontend"), "Relative subproject root changed");
    manifest.project_root = "..";
    write(root / "source/frontend/across.json", nlohmann::json(manifest).dump());
    require(across::load_github_manifest(root / "source", "frontend/across.json").project_root == fs::canonical(root / "source"), "Valid relative repository root must be preserved");
    manifest.project_root = "/old-machine/projects/site";
    write(root / "source/frontend/across.json", nlohmann::json(manifest).dump());
    run({"git", "-C", (root / "source").string(), "add", "."});
    run({"git", "-C", (root / "source").string(), "-c", "user.name=Test", "-c", "user.email=test@example.com", "commit", "-m", "Initial"});
    run({"git", "clone", "--bare", (root / "source").string(), (root / "remote.git").string()});
    std::string log;
    require(across::connect_github([&](const auto& chunk) { log += chunk; }).ok() && log.find("ABCD-1234") != std::string::npos, "Auth code must reach Activity");
    require(read(root / "browser-log") == "https://github.com/login/device\n", "Connect must open GitHub once, even when the URL is split between output chunks");
    const auto repos = across::github_repositories();
    require(repos.at("account") == "tester" && repos.at("repositories").size() == 1, "Repository discovery filters archives");
    write(root / "fail-repos", "");
    const auto failed_repos = across::github_repositories();
    require(failed_repos.at("account") == "tester" && failed_repos.contains("error"), "Repository failure must retain connected account and report its error");
    fs::remove(root / "fail-repos");
    for (const auto* repo : {"-bad/repo", "owner/../bad", "owner/repo;bad", "owner/..", "https://github.com/owner/repo"})
      rejects([&] { across::validate_github_repository(repo); }, "Invalid repository accepted");
    const auto cloned = across::clone_github_project("owner/site", "main");
    require(fs::exists(cloned / "README.md"), "Authenticated cloning failed");
    require(nlohmann::json::parse(read(cloned / "across.json")).at("project_root") == ".", "Cloning must save a portable manifest before Projects loads it");
    across::ServerProfile server{"test", "Test", "example.com", "ubuntu", 2222, ""};
    fs::create_directories(across::config_directory());
    write(across::known_hosts_path(), "[example.com]:2222 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBpoyMEeqzrFCJiEmSrrBGzbSmAIc51OgTWVB9Cd3xln\n");
    auto deploy = [&] { return across::deploy_github_project(server, "owner/site", "main", root / "source", "frontend/across.json", [&](const auto& chunk) { log += chunk; }, {{"DATABASE_URL", "private-test-value\nsecond-line"}, {"NEXT_PUBLIC_API_URL", "https://example.com"}}); };
    write(root / "no-admin", ""); rejects(deploy, "Missing administration permission accepted"); fs::remove(root / "no-admin");
    require(!fs::exists(root / "ssh-log"), "Permissions must be checked before VPS mutation");
    write(root / "fail-ssh", ""); rejects(deploy, "Failed VPS check accepted"); fs::remove(root / "fail-ssh");
    require(!fs::exists(root / "secrets"), "Failed VPS check must not write secrets");
    write(root / "fail-secret", ""); rejects(deploy, "Failed secret upload accepted"); fs::remove(root / "fail-secret");
    require(!across::run_process({"git", "--git-dir", (root / "remote.git").string(), "cat-file", "-e", "main:.github/workflows/across-deploy.yml"}).ok(), "Secret failure published workflow");
    require(deploy().ok(), "CI publication failed");
    run({"git", "clone", (root / "remote.git").string(), (root / "published").string()});
    require(fs::exists(root / "published/.across/client/src/core/github.cpp"), "Published client sources are incomplete");
    require(read(root / "published/README.md") == "preserve this file\n", "Unrelated repository content changed");
    const auto published = nlohmann::json::parse(read(root / "published/frontend/across.json"));
    require(published.at("project_root") == ".", "CI manifest retained desktop absolute path");
    const auto workflow = read(root / "published/.github/workflows/across-deploy.yml");
    require(workflow.find("frontend/across.json") != std::string::npos && workflow.find("ACROSS_REQUIRE_KNOWN_HOST") != std::string::npos, "Workflow missing manifest or host verification");
    std::istringstream lines(workflow); std::string line, deploy_script;
    bool in_deploy = false, in_script = false;
    while (std::getline(lines, line)) {
      if (line == "      - name: Deploy") in_deploy = true;
      if (in_deploy && line == "        run: |") { in_script = true; continue; }
      if (in_script && line.starts_with("          ")) deploy_script += line.substr(10) + "\n";
    }
    write(root / "deploy.sh", deploy_script);
    run({"bash", "-n", (root / "deploy.sh").string()});
    const auto python_start = deploy_script.find("python3 - <<'PY'\n");
    const auto python_end = deploy_script.find("\nPY\n", python_start);
    require(python_start != std::string::npos && python_end != std::string::npos, "Environment payload generator is missing");
    write(root / "environment.sh", deploy_script.substr(python_start, python_end + 4 - python_start));
    setenv("ACROSS_ENV_FILE", (root / "environment.json").c_str(), 1);
    setenv("DATABASE_URL", "private-test-value\nsecond-line", 1);
    setenv("NEXT_PUBLIC_API_URL", "https://example.com", 1);
    run({"bash", (root / "environment.sh").string()});
    require(across::load_environment_keys(root / "environment.json").at("DATABASE_URL") == "private-test-value\nsecond-line", "Workflow environment payload loses multiline secrets");
    unsetenv("ACROSS_ENV_FILE"); unsetenv("DATABASE_URL"); unsetenv("NEXT_PUBLIC_API_URL");
    const auto secrets = read(root / "secrets");
    require(secrets.find("ACROSS_SSH_KEY") != std::string::npos && secrets.find("ACROSS_KNOWN_HOSTS") != std::string::npos, "CI secrets incomplete");
    require(workflow.find("DATABASE_URL: ${{ secrets.ACROSS_ENV_DATABASE_URL }}") != std::string::npos && workflow.find("ACROSS_ENV_FILE") != std::string::npos, "Application keys are missing from deployment step");
    require(secrets.find("ACROSS_ENV_DATABASE_URL") != std::string::npos, "Application secret was not uploaded");
    require(workflow.find("private-test-value") == std::string::npos && log.find("private-test-value") == std::string::npos, "Application secret leaked into workflow or Activity");
    require(log.find("BEGIN OPENSSH PRIVATE KEY") == std::string::npos, "Private key leaked to Activity");
    const auto previous_secrets = secrets;
    rejects(deploy, "Existing pipeline was overwritten");
    require(read(root / "secrets") == previous_secrets, "Existing pipeline changed secrets");
    require(across::github_runs("owner/site", "main").ok(), "Runs cannot be inspected");
    manifest.project_root = "../.."; write(root / "source/frontend/across.json", nlohmann::json(manifest).dump());
    rejects(deploy, "Manifest outside repository accepted");
    std::cout << "GitHub integration tests passed (simulated GitHub and SSH; real git and key generation)\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; status = 1; }
  setenv("PATH", previous_path.c_str(), 1);
  if (original_config) setenv("ACROSS_CONFIG_DIR", previous_config.c_str(), 1); else unsetenv("ACROSS_CONFIG_DIR");
  unsetenv("ACROSS_TEST_ROOT"); fs::remove_all(root);
  return status;
}
