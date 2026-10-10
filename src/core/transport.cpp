#include "across/transport.hpp"

#include <regex>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include <stdexcept>

#include "across/config.hpp"

namespace across {
namespace {

void validate_remote_path(const std::string& path) {
  static const std::regex safe_path("^/[A-Za-z0-9._/@+:-]+(?:/[A-Za-z0-9._@+:-]+)*/*$");
  if (!std::regex_match(path, safe_path) || path.find("/../") != std::string::npos ||
      path.ends_with("/..")) {
    throw std::runtime_error("Remote path must be absolute and contain safe characters");
  }
}

std::string command_string(const std::vector<std::string>& command) {
  if (command.empty()) {
    throw std::invalid_argument("Remote command cannot be empty");
  }
  std::string result;
  for (const auto& argument : command) {
    if (!result.empty()) {
      result.push_back(' ');
    }
    result += shell_quote(argument);
  }
  return result;
}

}  // namespace

SshTransport::SshTransport(ServerProfile profile) : profile_(std::move(profile)) {
  validate_server_profile(profile_);
}

std::vector<std::string> SshTransport::ssh_arguments() const {
  std::vector<std::string> arguments{
      "ssh",
      "-o", "BatchMode=yes",
      "-o", "ConnectTimeout=10",
      "-o", "ServerAliveInterval=15",
      "-o", "ServerAliveCountMax=3",
      "-o", std::getenv("ACROSS_REQUIRE_KNOWN_HOST") ? "StrictHostKeyChecking=yes" : "StrictHostKeyChecking=accept-new",
      "-o", "UserKnownHostsFile=" + known_hosts_path().string(),
      "-p", std::to_string(profile_.port)};
  if (!profile_.identity_file.empty()) {
    arguments.emplace_back("-o");
    arguments.emplace_back("IdentitiesOnly=yes");
    arguments.emplace_back("-i");
    arguments.push_back(profile_.identity_file);
  }
  arguments.push_back(profile_.user + "@" + profile_.host);
  return arguments;
}

ProcessResult SshTransport::execute(const std::vector<std::string>& command,
                                    std::chrono::seconds timeout,
                                    const OutputCallback& on_output) const {
  auto arguments = ssh_arguments();
  arguments.push_back(command_string(command));
  return run_process(arguments, timeout, on_output);
}

ProcessResult SshTransport::execute_shell(const std::string& script,
                                          std::chrono::seconds timeout,
                                          const OutputCallback& on_output) const {
  if (script.empty()) {
    throw std::invalid_argument("Remote script cannot be empty");
  }
  auto arguments = ssh_arguments();
  arguments.push_back("sh -c " + shell_quote(script));
  return run_process(arguments, timeout, on_output);
}

ProcessResult SshTransport::execute_privileged(
    const std::vector<std::string>& command, std::chrono::seconds timeout,
    const OutputCallback& on_output) const {
  const std::string encoded = command_string(command);
  const std::string script =
      "if [ \"$(id -u)\" -eq 0 ]; then exec " + encoded +
      "; else exec sudo -n -- " + encoded + "; fi";
  return execute_shell(script, timeout, on_output);
}

std::string SshTransport::rsync_shell() const {
  std::string command =
      "ssh -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 "
      "-o ServerAliveCountMax=3 -o " +
      (std::getenv("ACROSS_REQUIRE_KNOWN_HOST") ? std::string("StrictHostKeyChecking=yes") : std::string("StrictHostKeyChecking=accept-new")) + " -o " +
      shell_quote("UserKnownHostsFile=" + known_hosts_path().string()) + " -p " +
      std::to_string(profile_.port);
  if (!profile_.identity_file.empty()) {
    command += " -o IdentitiesOnly=yes -i " + shell_quote(profile_.identity_file);
  }
  return command;
}

std::string SshTransport::destination(const std::string& path) const {
  validate_remote_path(path);
  return profile_.user + "@" + profile_.host + ":" + path;
}

ProcessResult SshTransport::sync_directory(
    const std::filesystem::path& local_directory,
    const std::string& remote_directory,
    const std::vector<std::string>& excludes,
    const OutputCallback& on_output) const {
  validate_remote_path(remote_directory);
  if (!std::filesystem::is_directory(local_directory)) {
    throw std::runtime_error("Local directory does not exist: " +
                             local_directory.string());
  }
  auto prepared = execute({"mkdir", "-p", remote_directory},
                          std::chrono::seconds{30}, on_output);
  if (!prepared.ok()) {
    return prepared;
  }
  std::vector<std::string> arguments{
      "rsync", "-az", "--delete", "--safe-links", "--human-readable",
      "-e", rsync_shell()};
  for (const auto& pattern : excludes) {
    arguments.push_back("--exclude=" + pattern);
  }
  auto source = local_directory.string();
  if (!source.ends_with('/')) {
    source.push_back('/');
  }
  auto remote = remote_directory;
  if (!remote.ends_with('/')) {
    remote.push_back('/');
  }
  arguments.push_back(source);
  arguments.push_back(destination(remote));
  return run_process(arguments, std::chrono::minutes{30}, on_output);
}

ProcessResult SshTransport::upload(const std::filesystem::path& local_path,
                                   const std::string& remote_path,
                                   const OutputCallback& on_output) const {
  validate_remote_path(remote_path);
  if (!std::filesystem::exists(local_path)) {
    throw std::runtime_error("Local path does not exist: " + local_path.string());
  }
  const auto parent = std::filesystem::path(remote_path).parent_path().string();
  auto prepared = execute({"mkdir", "-p", parent}, std::chrono::seconds{30},
                          on_output);
  if (!prepared.ok()) {
    return prepared;
  }
  if (profile_.transfer_protocol != "rsync") return copy_path(std::filesystem::absolute(local_path).string(), remote_path, true, on_output);
  auto source = local_path.string();
  auto target = remote_path;
  if (std::filesystem::is_directory(local_path)) {
    prepared = execute({"mkdir", "-p", remote_path}, std::chrono::seconds{30}, on_output);
    if (!prepared.ok()) return prepared;
    if (!source.ends_with('/')) source += '/';
    if (!target.ends_with('/')) target += '/';
  }
  return run_process({"rsync", "-az", "--safe-links", "--human-readable", "-e",
                      rsync_shell(), source, destination(target)},
                     std::chrono::minutes{30}, on_output);
}

ProcessResult SshTransport::download(const std::string& remote_path,
                                     const std::filesystem::path& local_path,
                                     const OutputCallback& on_output) const {
  validate_remote_path(remote_path);
  if (!local_path.parent_path().empty()) {
    std::filesystem::create_directories(local_path.parent_path());
  }
  if (profile_.transfer_protocol != "rsync") return copy_path(std::filesystem::absolute(local_path).string(), remote_path, false, on_output);
  auto source = remote_path;
  auto target = local_path.string();
  const auto directory = execute({"test", "-d", remote_path}, std::chrono::seconds{30});
  if (directory.timed_out || (directory.exit_code != 0 && directory.exit_code != 1)) return directory;
  if (directory.ok()) {
    std::filesystem::create_directories(local_path);
    if (!source.ends_with('/')) source += '/';
    if (!target.ends_with('/')) target += '/';
  }
  return run_process({"rsync", "-az", "--safe-links", "--human-readable", "-e",
                      rsync_shell(), destination(source), target},
                     std::chrono::minutes{30}, on_output);
}

ProcessResult SshTransport::copy_path(const std::string& local, const std::string& remote, bool uploading, const OutputCallback& on_output) const {
  auto options = ssh_arguments();
  options.pop_back();
  options.erase(options.begin());
  for (std::size_t index = 0; index < options.size(); ++index) if (options[index] == "-p") options[index] = "-P";
  if (profile_.transfer_protocol == "scp") {
    options.insert(options.begin(), {"scp", "-O", "-r"});
    options.push_back("--");
    options.push_back(uploading ? local : destination(remote));
    options.push_back(uploading ? destination(remote) : local);
    return run_process(options, std::chrono::minutes{30}, on_output);
  }
  auto quote = [](const std::string& path) {
    if (path.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("SFTP paths must not contain newlines");
    std::string result{"\""};
    for (const char character : path) {
      if (character == '\\' || character == '\"' || character == '*' || character == '?' || character == '[' || character == ']') result += '\\';
      result += character;
    }
    return result + "\"";
  };
  auto pattern = (std::filesystem::temp_directory_path() / "across-sftp-XXXXXX").string();
  std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
  const int descriptor = mkstemp(name.data());
  if (descriptor < 0) throw std::runtime_error("Cannot create SFTP batch file");
  close(descriptor);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{name.data()};
  std::ofstream batch(cleanup.path);
  batch << (uploading ? "put -R " + quote(local) + " " + quote(remote) : "get -R " + quote(remote) + " " + quote(local)) << '\n';
  batch.close();
  options.insert(options.begin(), {"sftp", "-b", cleanup.path.string()});
  options.push_back(profile_.user + "@" + profile_.host);
  return run_process(options, std::chrono::minutes{30}, on_output);
}

}  // namespace across
