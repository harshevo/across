#include "across/deployer.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "across/config.hpp"
#include "across/packages.hpp"

namespace across {
namespace {

ProcessResult failure_from(const ProcessResult& result,
                           const std::string& context) {
  ProcessResult failure = result;
  failure.output = context + "\n" + failure.output;
  return failure;
}

std::filesystem::path executable_path() {
#ifdef __APPLE__
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
  buffer.resize(std::char_traits<char>::length(buffer.c_str()));
  return std::filesystem::weakly_canonical(buffer);
#else
  std::vector<char> buffer(4096);
  const auto count = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (count <= 0) return {};
  return std::filesystem::path(std::string(buffer.data(),
                                           static_cast<std::size_t>(count)));
#endif
}

std::filesystem::path find_server_source_directory() {
  if (const char* custom = std::getenv("ACROSS_SERVER_BUNDLE");
      custom != nullptr && *custom != '\0') {
    const std::filesystem::path candidate{custom};
    if (std::filesystem::is_regular_file(candidate / "CMakeLists.txt")) {
      return candidate;
    }
    throw std::runtime_error("ACROSS_SERVER_BUNDLE is not a valid server source bundle");
  }

  const auto executable = executable_path();
  if (!executable.empty()) {
    const auto resources = executable.parent_path().parent_path() / "Resources" /
                           "server";
    if (std::filesystem::is_regular_file(resources / "CMakeLists.txt")) {
      return std::filesystem::weakly_canonical(resources);
    }
    const auto installed = executable.parent_path().parent_path() / "share" /
                           "across" / "server";
    if (std::filesystem::is_regular_file(installed / "CMakeLists.txt")) {
      return std::filesystem::weakly_canonical(installed);
    }
  }

  const std::filesystem::path development{ACROSS_SOURCE_DIR};
  if (std::filesystem::is_regular_file(development / "CMakeLists.txt")) {
    return development;
  }
  throw std::runtime_error("The bundled across-server source could not be located");
}

std::filesystem::path temporary_manifest(const ProjectManifest& manifest,
                                         const std::string& remote_root) {
  const auto local = std::filesystem::temp_directory_path() /
                     ("across-" + manifest.app_id + "-" + make_release_id() + ".json");
  ProjectManifest remote_manifest = manifest;
  remote_manifest.project_root = remote_root + "/source";
  std::ofstream output(local, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Cannot create temporary deployment manifest");
  }
  nlohmann::json json = remote_manifest;
  output << json.dump(2) << '\n';
  output.close();
  std::filesystem::permissions(local,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);
  return local;
}

}  // namespace

std::filesystem::path server_source_directory() { return find_server_source_directory(); }

Deployer::Deployer(ServerProfile profile) : transport_(std::move(profile)) {}

ProcessResult Deployer::doctor(const OutputCallback& on_output) const {
  auto result = transport_.execute_privileged(
      {"/usr/local/bin/across-server", "doctor", "--json"},
      std::chrono::seconds{30}, on_output);
  if (result.exit_code == 127) {
    result.output = "across-server is not installed; run bootstrap first\n" +
                    result.output;
  }
  return result;
}

ProcessResult Deployer::bootstrap(const OutputCallback& on_output) const {
  const std::string remote_root =
      "/tmp/across-bootstrap-" + transport_.profile().id;
  auto prerequisites = transport_.execute_shell(bootstrap_prerequisites_script(), std::chrono::minutes{20}, on_output);
  if (!prerequisites.ok()) return failure_from(prerequisites, "Could not install Ubuntu/Debian prerequisites");
  auto prepared = transport_.execute_shell(
      "set -eu; rm -rf " + shell_quote(remote_root) + "; mkdir -m 700 " +
          shell_quote(remote_root),
      std::chrono::seconds{30}, on_output);
  if (!prepared.ok()) {
    return failure_from(prepared, "Could not prepare the remote build directory");
  }

  const std::filesystem::path source_root = server_source_directory();
  const std::vector<std::string> excludes{
      ".git", "build", "build-*", ".cache", ".DS_Store"};
  auto uploaded = transport_.sync_directory(source_root, remote_root, excludes,
                                            on_output);
  if (!uploaded.ok()) {
    return failure_from(uploaded, "Could not upload the server CLI source");
  }

  const std::string script =
      "set -eu\n"
      "if [ \"$(id -u)\" -eq 0 ]; then SUDO=''; else sudo -n true; SUDO=sudo; fi\n"
      "cmake -S " + shell_quote(remote_root) + " -B " +
      shell_quote(remote_root + "/build") +
      " -DACROSS_BUILD_CLIENT=OFF -DACROSS_BUILD_DESKTOP=OFF -DACROSS_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release\n"
      "cmake --build " + shell_quote(remote_root + "/build") + " --parallel 1\n"
      "$SUDO install -m 0755 " + shell_quote(remote_root + "/build/across-server") +
      " /usr/local/bin/across-server\n"
      "$SUDO /usr/local/bin/across-server provision --packages " +
      shell_quote(nlohmann::json(transport_.profile().packages).dump()) + " --json\n";
  auto installed = transport_.execute_shell(script, std::chrono::minutes{45},
                                             on_output);
  transport_.execute({"rm", "-rf", remote_root}, std::chrono::seconds{30});
  return installed;
}

ProcessResult Deployer::deploy(const ProjectManifest& manifest,
                               const OutputCallback& on_output) const {
  validate_manifest(manifest);
  auto upload_root = manifest.project_root;
  if (manifest.build_location == "local" && manifest.type != "proxy") {
    for (const auto& step : {std::pair{"Installing dependencies locally", manifest.install_command},
                             std::pair{"Building application locally", manifest.build_command}}) {
      if (step.second.empty()) continue;
      if (on_output) on_output(std::string(step.first) + "...\n");
      const auto built = run_process({"sh", "-c", "cd " + shell_quote(manifest.project_root.string()) + " && " + step.second},
                                     std::chrono::minutes{30}, on_output);
      if (!built.ok()) return failure_from(built, step.first);
    }
    upload_root = std::filesystem::weakly_canonical(manifest.project_root / manifest.deployment_dir);
    const auto project_root = std::filesystem::weakly_canonical(manifest.project_root);
    const auto relative = upload_root.lexically_relative(project_root);
    if (relative.empty() || *relative.begin() == ".." || !std::filesystem::is_directory(upload_root)) {
      throw std::runtime_error("Local deployment directory must exist inside the project after building");
    }
  }
  const std::string release = make_release_id();
  const std::string remote_root =
      "/tmp/across-upload-" + manifest.app_id + "-" + release;
  auto prepared = transport_.execute_shell(
      "set -eu; rm -rf " + shell_quote(remote_root) + "; mkdir -m 700 " +
          shell_quote(remote_root) + " " + shell_quote(remote_root + "/source"),
      std::chrono::seconds{30}, on_output);
  if (!prepared.ok()) {
    return failure_from(prepared, "Could not create deployment staging area");
  }

  std::vector<std::string> excludes{
      ".git", ".github", ".across", ".DS_Store", ".env",
      ".env.*", "*.pem", "*.key"};
  if (manifest.build_location != "local") {
    excludes.insert(excludes.end(), {"node_modules", "build", "build-*"});
  }
  excludes.insert(excludes.end(), manifest.excludes.begin(), manifest.excludes.end());
  auto synced = transport_.sync_directory(upload_root,
                                          remote_root + "/source", excludes,
                                          on_output);
  if (!synced.ok()) {
    transport_.execute({"rm", "-rf", remote_root}, std::chrono::seconds{30});
    return failure_from(synced, "File synchronization failed");
  }

  const auto local_manifest = temporary_manifest(manifest, remote_root);
  auto manifest_upload = transport_.upload(local_manifest,
                                           remote_root + "/across.json", on_output);
  std::error_code remove_error;
  std::filesystem::remove(local_manifest, remove_error);
  if (!manifest_upload.ok()) {
    transport_.execute({"rm", "-rf", remote_root}, std::chrono::seconds{30});
    return failure_from(manifest_upload, "Manifest upload failed");
  }

  auto deployed = transport_.execute_privileged(
      {"/usr/local/bin/across-server", "deploy", "--manifest",
       remote_root + "/across.json", "--source", remote_root + "/source",
       "--release", release, "--json"},
      std::chrono::minutes{30}, on_output);
  transport_.execute({"rm", "-rf", remote_root}, std::chrono::seconds{30});
  return deployed;
}

ProcessResult Deployer::remote_command(
    const std::vector<std::string>& command,
    const OutputCallback& on_output) const {
  return transport_.execute(command, std::chrono::minutes{10}, on_output);
}

ProcessResult Deployer::server_command(
    const std::vector<std::string>& arguments,
    const OutputCallback& on_output) const {
  std::vector<std::string> command{"/usr/local/bin/across-server"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto timeout = !arguments.empty() && (arguments.front() == "packages-install" || arguments.front() == "provision") ? std::chrono::minutes{30} : std::chrono::minutes{10};
  return transport_.execute_privileged(command, timeout, on_output);
}

ProcessResult Deployer::upload(const std::filesystem::path& local,
                               const std::string& remote,
                               const OutputCallback& on_output) const {
  return transport_.upload(local, remote, on_output);
}

ProcessResult Deployer::download(const std::string& remote,
                                 const std::filesystem::path& local,
                                 const OutputCallback& on_output) const {
  return transport_.download(remote, local, on_output);
}

std::string make_release_id() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  std::random_device device;
  std::uniform_int_distribution<int> distribution(0, 0xffff);
  std::ostringstream id;
  id << std::put_time(&utc, "%Y%m%dT%H%M%SZ") << '-' << std::hex
     << std::setw(4) << std::setfill('0') << distribution(device);
  return id.str();
}

}  // namespace across
