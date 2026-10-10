#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "across/models.hpp"
#include "across/process.hpp"

namespace across {

class SshTransport {
 public:
  explicit SshTransport(ServerProfile profile);

  [[nodiscard]] const ServerProfile& profile() const { return profile_; }
  ProcessResult execute(const std::vector<std::string>& command,
                        std::chrono::seconds timeout = std::chrono::seconds{60},
                        const OutputCallback& on_output = {}) const;
  ProcessResult execute_shell(const std::string& script,
                              std::chrono::seconds timeout,
                              const OutputCallback& on_output = {}) const;
  ProcessResult execute_privileged(
      const std::vector<std::string>& command,
      std::chrono::seconds timeout = std::chrono::seconds{60},
      const OutputCallback& on_output = {}) const;
  ProcessResult sync_directory(const std::filesystem::path& local_directory,
                               const std::string& remote_directory,
                               const std::vector<std::string>& excludes,
                               const OutputCallback& on_output = {}) const;
  ProcessResult upload(const std::filesystem::path& local_path,
                       const std::string& remote_path,
                       const OutputCallback& on_output = {}) const;
  ProcessResult download(const std::string& remote_path,
                         const std::filesystem::path& local_path,
                         const OutputCallback& on_output = {}) const;

 private:
  [[nodiscard]] std::vector<std::string> ssh_arguments() const;
  [[nodiscard]] std::string rsync_shell() const;
  [[nodiscard]] std::string destination(const std::string& path) const;

  ProcessResult copy_path(const std::string& local, const std::string& remote, bool upload, const OutputCallback& on_output) const;
  ServerProfile profile_;
};

}  // namespace across
