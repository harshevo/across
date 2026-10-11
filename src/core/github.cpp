#include "across/github.hpp"
#include "across/config.hpp"
#include "across/deployer.hpp"
#include "across/management.hpp"
#include "across/transport.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <regex>
#include <unistd.h>

namespace across {
namespace {
std::string gh_executable() {
  if (const auto* environment_path = std::getenv("PATH")) {
    std::istringstream paths(environment_path); std::string directory;
    while (std::getline(paths, directory, ':')) {
      if (directory.empty()) continue;
      const auto candidate = std::filesystem::path(directory) / "gh";
      if (access(candidate.c_str(), X_OK) == 0) return candidate.string();
    }
  }
  for (const auto* path : {"/opt/homebrew/bin/gh", "/usr/local/bin/gh", "/usr/bin/gh"})
    if (access(path, X_OK) == 0) return path;
  return "gh";
}
ProcessResult gh(std::vector<std::string> args, const OutputCallback& output = {}, int timeout = 120) {
  args.insert(args.begin(), gh_executable());
  auto result = run_process(args, std::chrono::seconds{timeout}, output);
  if (result.exit_code == 127) throw std::runtime_error("Install GitHub CLI first (macOS: brew install gh), then Connect GitHub.");
  return result;
}
void require(const ProcessResult& result, const std::string& operation) {
  if (!result.ok()) throw std::runtime_error(operation + ": " + result.output);
}
std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct PrivateDirectory {
  std::filesystem::path path;
  PrivateDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() / "across-github-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
    if (!mkdtemp(name.data())) throw std::runtime_error("Cannot create GitHub staging folder");
    path = name.data();
  }
  ~PrivateDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void secret(const std::string& repository, const std::string& name, const std::string& value, const std::filesystem::path& directory) {
  const auto path = directory / "secret";
  { std::ofstream output(path); output << value; if (!output) throw std::runtime_error("Cannot stage deployment secret"); }
  std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  const auto command = shell_quote(gh_executable()) + " secret set " + shell_quote(name) + " --repo " + shell_quote("https://github.com/" + repository) + " < " + shell_quote(path.string());
  const auto result = run_process({"sh", "-c", command}, std::chrono::seconds{60});
  std::filesystem::remove(path);
  if (!result.ok()) throw std::runtime_error("Failed to configure GitHub secret " + name + "; check repository administration permissions.");
}
void clone(const std::string& repository, const std::string& branch, const std::filesystem::path& path, const OutputCallback& output) {
  require(gh({"repo", "clone", "https://github.com/" + repository + ".git", path.string(), "--", "--single-branch", "--branch", branch}, output, 600), "Clone repository");
}
}
void validate_github_repository(const std::string& repository) {
  if (!std::regex_match(repository, std::regex("^[A-Za-z0-9][A-Za-z0-9-]{0,38}/[A-Za-z0-9_][A-Za-z0-9_.-]{0,99}$")) || repository.ends_with("/.") || repository.ends_with("/.."))
    throw std::runtime_error("Enter a GitHub repository as owner/repository");
}
ProcessResult connect_github(const OutputCallback& output) {
  std::string auth_output;
  bool browser_opened = false;
  const auto on_auth_output = [&](const std::string& chunk) {
    if (output) output(chunk);
    auth_output += chunk;
    if (browser_opened || auth_output.find("https://github.com/login/device") == std::string::npos) return;
    browser_opened = true;
#ifdef __APPLE__
    const std::vector<std::string> browser_command{"open", "https://github.com/login/device"};
#else
    const std::vector<std::string> browser_command{"xdg-open", "https://github.com/login/device"};
#endif
    const auto opened = run_process(browser_command, std::chrono::seconds{10});
    if (output) output(opened.ok()
        ? "Opened GitHub in your browser. Enter the one-time code to finish connecting.\n"
        : "Could not open your browser. Open https://github.com/login/device and enter the code shown above.\n");
  };
  return gh({"auth", "login", "--hostname", "github.com", "--web", "--clipboard", "--git-protocol", "https", "--skip-ssh-key", "--scopes", "repo,workflow"}, on_auth_output, 900);
}
nlohmann::json github_repositories() {
  const auto account = gh({"api", "--hostname", "github.com", "user"});
  require(account, "GitHub connection (use Connect GitHub)");
  const auto repositories = gh({"api", "--hostname", "github.com", "--paginate", "--slurp", "user/repos?per_page=100&sort=updated&affiliation=owner,collaborator,organization_member"});
  const auto login = nlohmann::json::parse(account.output).at("login");
  if (!repositories.ok()) return {{"account", login}, {"repositories", nlohmann::json::array()}, {"error", "Could not load repositories: " + repositories.output}};
  auto available = nlohmann::json::array();
  for (const auto& page : nlohmann::json::parse(repositories.output))
    for (const auto& repo : page)
      if (!repo.value("archived", false) && repo.value("permissions", nlohmann::json::object()).value("push", false))
        available.push_back({{"name", repo.at("full_name")}, {"branch", repo.at("default_branch")}});
  return {{"account", nlohmann::json::parse(account.output).at("login")}, {"repositories", available}};
}
ProjectManifest load_github_manifest(const std::filesystem::path& repository, const std::string& manifest) {
  pipeline_workflow("main", manifest);
  const auto root = std::filesystem::canonical(repository);
  const auto path = std::filesystem::canonical(root / manifest);
  const auto inside = [&](const std::filesystem::path& candidate) {
    return candidate == root || candidate.string().starts_with(root.string() + "/");
  };
  if (!inside(path)) throw std::runtime_error("GitHub manifest must be inside the repository");
  auto settings = nlohmann::json::parse(read_file(path)).get<ProjectManifest>();
  auto project = settings.project_root;
  if (project.is_absolute()) {
    project = std::filesystem::weakly_canonical(project);
    if (!inside(project)) project = path.parent_path();
  } else {
    project = std::filesystem::weakly_canonical(path.parent_path() / project);
  }
  if (!inside(project)) throw std::runtime_error("Manifest project_root must be inside the repository");
  settings.project_root = project;
  if (settings.install_command.empty() && std::filesystem::is_regular_file(project / "package.json"))
    settings.install_command = std::filesystem::exists(project / "package-lock.json") ? "npm ci" : "npm install";
  validate_manifest(settings);
  return settings;
}
std::filesystem::path clone_github_project(const std::string& repository, const std::string& branch, const OutputCallback& output) {
  validate_github_repository(repository); pipeline_workflow(branch, "across.json");
  const auto path = config_directory() / "github-projects" / repository / make_release_id();
  std::filesystem::create_directories(path.parent_path());
  clone(repository, branch, path, output);
  if (std::filesystem::is_regular_file(path / "across.json")) {
    auto settings = load_github_manifest(path, "across.json");
    settings.project_root = settings.project_root.lexically_relative(std::filesystem::canonical(path));
    std::ofstream manifest_file(path / "across.json");
    manifest_file << nlohmann::json(settings).dump(2) << '\n';
    if (!manifest_file) throw std::runtime_error("Cannot save portable GitHub manifest");
    if (output) output("Loaded repository deployment settings with a portable project path.\n");
  }
  return path;
}
ProcessResult deploy_github_project(const ServerProfile& server, const std::string& repository,
    const std::string& branch, const std::filesystem::path& project, const std::string& manifest, const OutputCallback& output, const EnvironmentKeys& environment) {
  std::vector<std::string> names;
  for (const auto& [name, value] : environment) {
    validate_environment_key(name);
    if (value.size() > 48 * 1024 || value.find('\0') != std::string::npos) throw std::runtime_error("Invalid environment value size");
    names.push_back(name);
  }
  pipeline_workflow(branch, manifest, names);
  validate_server_profile(server); validate_github_repository(repository); pipeline_workflow(branch, manifest);
  auto settings = load_github_manifest(project, manifest);
  const auto root = std::filesystem::canonical(project);
  const auto project_path = std::filesystem::canonical(settings.project_root);
  const auto relative = project_path.lexically_relative(std::filesystem::canonical((project / manifest).parent_path()));
  if (relative.is_absolute() || (project_path != root && !project_path.string().starts_with(root.string() + "/")))
    throw std::runtime_error("Manifest project_root must be inside the repository");
  settings.project_root = relative;
  if (output) output("Checking repository and VPS before configuring CI/CD...\n");
  const auto metadata = gh({"api", "--hostname", "github.com", "repos/" + repository});
  require(metadata, "Read repository");
  if (!nlohmann::json::parse(metadata.output).at("permissions").value("admin", false)) throw std::runtime_error("Repository admin access is required to configure deployment secrets");
  PrivateDirectory staging;
  const auto checkout = staging.path / "repository";
  clone(repository, branch, checkout, output);
  if (std::filesystem::exists(checkout / ".github/workflows/across-deploy.yml") || std::filesystem::exists(checkout / ".across/client"))
    throw std::runtime_error("This branch already has an Across pipeline. Use View runs or deploy directly; existing workflows are preserved.");
  for (auto parent = checkout / manifest; parent != checkout; parent = parent.parent_path())
    if (std::filesystem::is_symlink(parent)) throw std::runtime_error("Repository manifest path contains a symlink");
  if (std::filesystem::is_symlink(checkout / ".github") || std::filesystem::is_symlink(checkout / ".github/workflows") || std::filesystem::is_symlink(checkout / ".across"))
    throw std::runtime_error("Repository CI folders must not be symlinks");
  std::filesystem::create_directories((checkout / manifest).parent_path());
  { std::ofstream file(checkout / manifest); file << nlohmann::json(settings).dump(2) << '\n'; if (!file) throw std::runtime_error("Cannot stage manifest"); }
  create_pipeline(checkout, branch, manifest, names);
  const auto backend = Deployer(server).doctor(output);
  require(backend, "VPS backend check (install backend in Connection first)");
  if (!environment.empty()) {
    const auto capabilities = nlohmann::json::parse(backend.output, nullptr, false);
    if (capabilities.is_discarded() || !capabilities.value("environment_keys", false)) {
      if (output) output("Updating the VPS backend for environment-key support...\n");
      require(Deployer(server).bootstrap(output), "Update VPS backend for environment keys");
    }
  }
  const auto host = server.port == 22 ? server.host : "[" + server.host + "]:" + std::to_string(server.port);
  const auto host_keys = run_process({"ssh-keygen", "-F", host, "-f", known_hosts_path().string()}, std::chrono::seconds{10});
  require(host_keys, "Find verified VPS host key");
  const auto key = staging.path / "deploy-key";
  require(run_process({"ssh-keygen", "-t", "ed25519", "-N", "", "-C", "across-github-" + repository, "-f", key.string()}, std::chrono::seconds{10}), "Generate CI deployment key");
  const auto public_key = read_file(key.string() + ".pub");
  require(SshTransport(server).execute_shell("set -eu; umask 077; mkdir -p ~/.ssh; touch ~/.ssh/authorized_keys; chmod 700 ~/.ssh; chmod 600 ~/.ssh/authorized_keys; printf '\\n%s\\n' " + shell_quote(public_key) + " >> ~/.ssh/authorized_keys", std::chrono::seconds{30}), "Authorize dedicated CI key on VPS");
  auto ci_server = server; ci_server.identity_file = key.string();
  require(SshTransport(ci_server).execute({"true"}), "Verify CI key login");
  if (output) output("Configuring encrypted GitHub repository secrets...\n");
  for (const auto& [name, value] : environment) secret(repository, "ACROSS_ENV_" + name, value, staging.path);
  secret(repository, "ACROSS_HOST", server.host, staging.path);
  secret(repository, "ACROSS_USER", server.user, staging.path);
  secret(repository, "ACROSS_PORT", std::to_string(server.port), staging.path);
  secret(repository, "ACROSS_KNOWN_HOSTS", host_keys.output, staging.path);
  secret(repository, "ACROSS_SSH_KEY", read_file(key), staging.path);
  const auto user = gh({"api", "--hostname", "github.com", "user"}); require(user, "Read GitHub account");
  const auto login = nlohmann::json::parse(user.output).at("login").get<std::string>();
  require(run_process({"git", "-C", checkout.string(), "add", "-f", "--", manifest, ".github/workflows/across-deploy.yml", ".across/client"}), "Stage CI files");
  require(run_process({"git", "-C", checkout.string(), "-c", "user.name=Across", "-c", "user.email=" + login + "@users.noreply.github.com", "commit", "-m", "Configure Across VPS deployment"}, std::chrono::seconds{30}, output), "Commit workflow");
  if (output) output("Publishing workflow to " + repository + " (" + branch + ")...\n");
  require(run_process({"git", "-C", checkout.string(), "-c", "credential.helper=", "-c", "credential.helper=!" + shell_quote(gh_executable()) + " auth git-credential", "push", "origin", "HEAD:refs/heads/" + branch}, std::chrono::seconds{120}, output), "Publish workflow (branch protection or a concurrent push may require retry)");
  const auto message = "Workflow published: https://github.com/" + repository + "/actions\nGitHub Actions now deploys pushes to " + branch + ". Check View runs for the deployment result.\n";
  if (output) output(message);
  return {0, message, false};
}
ProcessResult github_runs(const std::string& repository, const std::string& branch) {
  validate_github_repository(repository); pipeline_workflow(branch, "across.json");
  return gh({"run", "list", "--repo", "https://github.com/" + repository, "--branch", branch, "--workflow", "across-deploy.yml", "--limit", "5"});
}
}
