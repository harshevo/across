#include "across/process.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>

extern char** environ;
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace across {

ProcessResult run_process(const std::vector<std::string>& arguments,
                          std::chrono::seconds timeout,
                          const OutputCallback& on_output) {
  if (arguments.empty()) {
    throw std::invalid_argument("Cannot run an empty command");
  }

  int output_pipe[2];
  if (pipe(output_pipe) != 0) {
    throw std::runtime_error("pipe failed: " + std::string(std::strerror(errno)));
  }

  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_init(&attributes);
  posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes, 0);
  pid_t child;
  const int spawned = posix_spawnp(&child, argv.front(), &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (spawned != 0) {
    close(output_pipe[0]); close(output_pipe[1]);
    return ProcessResult{127, "exec failed: " + std::string(std::strerror(spawned)) + "\n", false};
  }

  close(output_pipe[1]);
  const int original_flags = fcntl(output_pipe[0], F_GETFL, 0);
  fcntl(output_pipe[0], F_SETFL, original_flags | O_NONBLOCK);

  ProcessResult result;
  constexpr std::size_t kMaximumCapturedOutput = 4 * 1024 * 1024;
  const auto remember_output = [&](const std::string& chunk) {
    result.output += chunk;
    if (result.output.size() > kMaximumCapturedOutput) {
      result.output.erase(0, result.output.size() - kMaximumCapturedOutput);
    }
  };
  std::array<char, 4096> buffer{};
  const auto started = std::chrono::steady_clock::now();
  bool child_finished = false;
  int status = 0;

  while (!child_finished) {
    pollfd descriptor{output_pipe[0], POLLIN, 0};
    poll(&descriptor, 1, 50);
    while (true) {
      const ssize_t count = read(output_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        std::string chunk(buffer.data(), static_cast<std::size_t>(count));
        remember_output(chunk);
        if (on_output) {
          on_output(chunk);
        }
      } else {
        break;
      }
    }

    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      child_finished = true;
    } else if (waited < 0 && errno != EINTR) {
      close(output_pipe[0]);
      throw std::runtime_error("waitpid failed: " +
                               std::string(std::strerror(errno)));
    }

    if (!child_finished && timeout.count() > 0 &&
        std::chrono::steady_clock::now() - started >= timeout) {
      result.timed_out = true;
      kill(-child, SIGTERM);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      kill(-child, SIGKILL);
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      child_finished = true;
    }
  }

  while (true) {
    const ssize_t count = read(output_pipe[0], buffer.data(), buffer.size());
    if (count <= 0) {
      break;
    }
    std::string chunk(buffer.data(), static_cast<std::size_t>(count));
    remember_output(chunk);
    if (on_output) {
      on_output(chunk);
    }
  }
  close(output_pipe[0]);

  if (result.timed_out) {
    result.exit_code = 124;
  } else if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
}

std::string shell_quote(const std::string& value) {
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

}  // namespace across
