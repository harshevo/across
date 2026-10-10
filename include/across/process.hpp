#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace across {

struct ProcessResult {
  int exit_code{-1};
  std::string output;
  bool timed_out{false};

  [[nodiscard]] bool ok() const { return exit_code == 0 && !timed_out; }
};

using OutputCallback = std::function<void(const std::string&)>;

ProcessResult run_process(
    const std::vector<std::string>& arguments,
    std::chrono::seconds timeout = std::chrono::seconds{0},
    const OutputCallback& on_output = {});

std::string shell_quote(const std::string& value);

}  // namespace across
