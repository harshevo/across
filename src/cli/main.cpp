#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "across/config.hpp"
#include "across/management.hpp"
#include "across/deployer.hpp"
#include "across/watcher.hpp"

namespace {

std::atomic_bool interrupted{false};

void handle_signal(int) {
  interrupted.store(true);
}

std::string option_value(int argc, char** argv, const std::string& option,
                         bool required = true) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (argv[index] == option) return argv[index + 1];
  }
  if (required) throw std::runtime_error("Missing option: " + option);
  return {};
}

bool has_option(int argc, char** argv, const std::string& option) {
  for (int index = 1; index < argc; ++index) {
    if (argv[index] == option) return true;
  }
  return false;
}

int print_result(const across::ProcessResult& result) {
  if (!result.output.empty()) std::cout << result.output;
  if (result.timed_out) std::cerr << "Operation timed out\n";
  return result.exit_code;
}

int streamed_result(const across::ProcessResult& result) {
  if (result.timed_out) std::cerr << "Operation timed out\n";
  return result.exit_code;
}

across::ServerProfile server_from_args(int argc, char** argv,
                                       const std::string& id) {
  across::ServerProfile profile;
  profile.id = across::slugify(id);
  profile.label = option_value(argc, argv, "--label", false);
  if (profile.label.empty()) profile.label = id;
  profile.host = option_value(argc, argv, "--host");
  profile.user = option_value(argc, argv, "--user", false);
  if (profile.user.empty()) profile.user = "ubuntu";
  const auto port = option_value(argc, argv, "--port", false);
  if (!port.empty()) {
    const int parsed = std::stoi(port);
    if (parsed < 1 || parsed > 65535) throw std::runtime_error("Invalid SSH port");
    profile.port = static_cast<std::uint16_t>(parsed);
  }
  profile.identity_file = option_value(argc, argv, "--identity", false);
  const auto protocol = option_value(argc, argv, "--protocol", false);
  if (!protocol.empty()) profile.transfer_protocol = protocol;
  return profile;
}

int server_commands(int argc, char** argv, across::ConfigStore& store) {
  if (argc < 3) throw std::runtime_error("Missing server action");
  const std::string action = argv[2];
  if (action == "list") {
    nlohmann::json json = store.list_servers();
    std::cout << json.dump(2) << '\n';
    return 0;
  }
  if (action == "add") {
    if (argc < 4) throw std::runtime_error("Missing server id");
    auto profile = server_from_args(argc, argv, argv[3]);
    store.save_server(profile);
    std::cout << "Saved server " << profile.id << " (" << profile.user << '@'
              << profile.host << ")\n";
    return 0;
  }
  if (action == "remove") {
    if (argc < 4) throw std::runtime_error("Missing server id");
    store.remove_server(argv[3]);
    std::cout << "Removed server profile " << argv[3] << '\n';
    return 0;
  }
  throw std::runtime_error("Unknown server action: " + action);
}

std::vector<std::string> trailing_arguments(int argc, char** argv,
                                            const std::string& marker) {
  for (int index = 1; index < argc; ++index) {
    if (argv[index] == marker) {
      if (index + 1 >= argc) throw std::runtime_error("Missing command after " + marker);
      std::vector<std::string> result;
      for (int command_index = index + 1; command_index < argc; ++command_index) {
        result.emplace_back(argv[command_index]);
      }
      return result;
    }
  }
  throw std::runtime_error("Missing " + marker + " separator");
}

int watch(across::Deployer& deployer, const across::ProjectManifest& manifest) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  across::FileWatcher watcher(manifest.project_root, manifest.excludes);
  std::cout << "Watching " << manifest.project_root << ". Press Ctrl-C to stop.\n";
  watcher.run(
      [&](const std::vector<across::FileChange>& changes) {
        std::cout << "\nChange detected; deploying...\n";
        for (const auto& change : changes) {
          std::cout << "  " << across::change_kind_name(change.kind) << "  "
                    << change.path << '\n';
        }
        const auto result = deployer.deploy(manifest, [](const std::string& chunk) {
          std::cout << chunk << std::flush;
        });
        if (!result.ok()) {
          std::cerr << "Deployment failed with exit " << result.exit_code << '\n';
        } else {
          std::cout << "Deployment completed.\n";
        }
      },
      interrupted);
  std::cout << "Watcher stopped.\n";
  return 0;
}

int watch_files(const std::filesystem::path& path) {
  interrupted.store(false);
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  across::FileWatcher watcher(path, {});
  std::cout << "Watching " << path << " for file changes. Press Ctrl-C to stop.\n";
  watcher.run(
      [](const std::vector<across::FileChange>& changes) {
        for (const auto& change : changes) {
          std::cout << across::change_kind_name(change.kind) << "\t" << change.path
                    << '\n';
        }
        std::cout << std::flush;
      },
      interrupted);
  return 0;
}

void usage() {
  std::cout
      << "Across " << ACROSS_VERSION << "\n\n"
      << "Server profiles:\n"
      << "  across server add ID --host HOST [--user USER] [--port PORT] [--identity PATH] [--protocol rsync|scp|sftp]\n"
      << "  across server list\n"
      << "  across server remove ID\n\n"
      << "Deployment:\n"
      << "  across doctor SERVER\n"
      << "  across bootstrap SERVER\n"
      << "  across deploy SERVER MANIFEST\n"
      << "  across watch SERVER MANIFEST\n"
      << "  across filewatch PATH\n"
      << "  across apps SERVER\n"
      << "  across rollback SERVER APP [RELEASE]\n"
      << "  across remove-app SERVER APP --yes\n\n"
      << "SSH and transfer:\n"
      << "  across manage SERVER -- SERVER_COMMAND [ARG...]\n"
      << "  across exec SERVER -- COMMAND [ARG...]\n"
      << "  across upload SERVER LOCAL REMOTE\n"
      << "  across download SERVER REMOTE LOCAL\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      usage();
      return 1;
    }
    const std::string command = argv[1];
    if (command == "--version" || command == "version") {
      std::cout << ACROSS_VERSION << '\n';
      return 0;
    }
    if (command == "pipeline") {
      if (argc < 3) throw std::runtime_error("Usage: across pipeline PROJECT [--branch main] [--manifest across.json]");
      const auto branch = option_value(argc, argv, "--branch", false);
      const auto manifest = option_value(argc, argv, "--manifest", false);
      std::cout << across::create_pipeline(argv[2], branch.empty() ? "main" : branch, manifest.empty() ? "across.json" : manifest) << '\n';
      return 0;
    }
    across::ConfigStore store;
    if (command == "server") return server_commands(argc, argv, store);
    if (command == "filewatch") {
      if (argc < 3) throw std::runtime_error("Missing directory to watch");
      return watch_files(argv[2]);
    }
    if (argc < 3) throw std::runtime_error("Missing server profile id");
    const auto profile = store.get_server(argv[2]);
    across::Deployer deployer(profile);

    if (command == "doctor") return print_result(deployer.doctor());
    if (command == "bootstrap") {
      return streamed_result(deployer.bootstrap(
          [](const std::string& chunk) { std::cout << chunk << std::flush; }));
    }
    if (command == "deploy" || command == "watch") {
      if (argc < 4) throw std::runtime_error("Missing manifest path");
      const auto manifest = across::load_manifest(argv[3]);
      if (command == "watch") return watch(deployer, manifest);
      return streamed_result(deployer.deploy(manifest, [](const std::string& chunk) {
        std::cout << chunk << std::flush;
      }));
    }
    if (command == "apps") {
      return print_result(deployer.server_command({"list", "--json"}));
    }
    if (command == "rollback") {
      if (argc < 4) throw std::runtime_error("Missing application id");
      std::vector<std::string> arguments{"rollback", "--app", argv[3], "--json"};
      if (argc >= 5) arguments.insert(arguments.end(), {"--release", argv[4]});
      return print_result(deployer.server_command(arguments));
    }
    if (command == "remove-app") {
      if (argc < 4) throw std::runtime_error("Missing application id");
      if (!has_option(argc, argv, "--yes")) {
        throw std::runtime_error("Removal is destructive; repeat with --yes");
      }
      return print_result(deployer.server_command(
          {"remove", "--app", argv[3], "--json"}));
    }
    if (command == "manage") return print_result(deployer.server_command(trailing_arguments(argc, argv, "--")));
    if (command == "exec") {
      return print_result(deployer.remote_command(trailing_arguments(argc, argv, "--")));
    }
    if (command == "upload") {
      if (argc < 5) throw std::runtime_error("Usage: across upload SERVER LOCAL REMOTE");
      return print_result(deployer.upload(argv[3], argv[4]));
    }
    if (command == "download") {
      if (argc < 5) throw std::runtime_error("Usage: across download SERVER REMOTE LOCAL");
      return print_result(deployer.download(argv[3], argv[4]));
    }
    usage();
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
