#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "across/models.hpp"
#include "across/process.hpp"
#include "across/transport.hpp"

namespace across {

class Deployer {
 public:
  explicit Deployer(ServerProfile profile);

  ProcessResult doctor(const OutputCallback& on_output = {}) const;
  ProcessResult bootstrap(const OutputCallback& on_output = {}) const;
  ProcessResult deploy(const ProjectManifest& manifest,
                       const OutputCallback& on_output = {}) const;
  ProcessResult remote_command(const std::vector<std::string>& command,
                               const OutputCallback& on_output = {}) const;
  ProcessResult server_command(const std::vector<std::string>& arguments,
                               const OutputCallback& on_output = {}) const;
  ProcessResult upload(const std::filesystem::path& local,
                       const std::string& remote,
                       const OutputCallback& on_output = {}) const;
  ProcessResult download(const std::string& remote,
                         const std::filesystem::path& local,
                         const OutputCallback& on_output = {}) const;

 private:
  SshTransport transport_;
};

std::string make_release_id();
std::filesystem::path server_source_directory();

}  // namespace across
