#include "across/watcher.hpp"

#include <algorithm>
#include <fnmatch.h>
#include <optional>
#include <stdexcept>
#include <thread>

namespace across {

std::string change_kind_name(ChangeKind kind) {
  switch (kind) {
    case ChangeKind::added: return "added";
    case ChangeKind::modified: return "modified";
    case ChangeKind::removed: return "removed";
  }
  return "unknown";
}

FileWatcher::FileWatcher(std::filesystem::path root,
                         std::vector<std::string> excludes)
    : root_(std::filesystem::weakly_canonical(std::move(root))),
      excludes_(std::move(excludes)) {
  if (!std::filesystem::is_directory(root_)) {
    throw std::runtime_error("Watch root is not a directory: " + root_.string());
  }
  const std::vector<std::string> defaults{
      ".git", ".git/*", "node_modules", "node_modules/*", ".DS_Store",
      "build", "build/*", "build-*", "build-*/*", ".env", ".env.*"};
  excludes_.insert(excludes_.end(), defaults.begin(), defaults.end());
  previous_ = snapshot();
}

bool FileWatcher::excluded(const std::filesystem::path& relative) const {
  const std::string value = relative.generic_string();
  for (const auto& pattern : excludes_) {
    if (fnmatch(pattern.c_str(), value.c_str(), FNM_PATHNAME) == 0) {
      return true;
    }
    const auto first_component = relative.begin();
    if (first_component != relative.end() &&
        fnmatch(pattern.c_str(), first_component->string().c_str(), 0) == 0) {
      return true;
    }
  }
  return false;
}

std::unordered_map<std::string, FileWatcher::FileState>
FileWatcher::snapshot() const {
  std::unordered_map<std::string, FileState> result;
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      root_, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (iterator != end) {
    if (error) {
      error.clear();
      iterator.increment(error);
      continue;
    }
    const auto relative = std::filesystem::relative(iterator->path(), root_, error);
    if (error) {
      error.clear();
      iterator.increment(error);
      continue;
    }
    if (excluded(relative)) {
      if (iterator->is_directory(error)) {
        iterator.disable_recursion_pending();
      }
      iterator.increment(error);
      continue;
    }
    if (iterator->is_regular_file(error)) {
      const auto size = iterator->file_size(error);
      if (!error) {
        const auto modified = iterator->last_write_time(error);
        if (!error) {
          result.emplace(relative.generic_string(), FileState{size, modified});
        }
      }
    }
    error.clear();
    iterator.increment(error);
  }
  return result;
}

bool FileWatcher::scan_changed() {
  return !scan_changes().empty();
}

std::vector<FileChange> FileWatcher::scan_changes() {
  auto current = snapshot();
  std::vector<FileChange> changes;
  for (const auto& [path, state] : current) {
    const auto previous = previous_.find(path);
    if (previous == previous_.end()) {
      changes.push_back({ChangeKind::added, path});
    } else if (!(previous->second == state)) {
      changes.push_back({ChangeKind::modified, path});
    }
  }
  for (const auto& [path, unused] : previous_) {
    (void)unused;
    if (!current.contains(path)) changes.push_back({ChangeKind::removed, path});
  }
  previous_ = std::move(current);
  std::sort(changes.begin(), changes.end(), [](const auto& left, const auto& right) {
    return left.path < right.path;
  });
  return changes;
}

void FileWatcher::run(
                      const std::function<void(const std::vector<FileChange>&)>& on_change,
                      std::atomic_bool& stop,
                      std::chrono::milliseconds poll_interval,
                      std::chrono::milliseconds debounce) {
  std::optional<std::chrono::steady_clock::time_point> last_change;
  std::vector<FileChange> pending;
  while (!stop.load()) {
    auto changes = scan_changes();
    if (!changes.empty()) {
      pending.insert(pending.end(), changes.begin(), changes.end());
      last_change = std::chrono::steady_clock::now();
    }
    if (last_change &&
        std::chrono::steady_clock::now() - *last_change >= debounce) {
      last_change.reset();
      on_change(pending);
      pending.clear();
      previous_ = snapshot();
    }
    std::this_thread::sleep_for(poll_interval);
  }
}

}  // namespace across
