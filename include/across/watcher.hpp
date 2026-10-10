#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace across {

enum class ChangeKind { added, modified, removed };

struct FileChange {
  ChangeKind kind;
  std::string path;
};

std::string change_kind_name(ChangeKind kind);

class FileWatcher {
 public:
  FileWatcher(std::filesystem::path root, std::vector<std::string> excludes);

  void run(const std::function<void(const std::vector<FileChange>&)>& on_change,
           std::atomic_bool& stop,
           std::chrono::milliseconds poll_interval =
               std::chrono::milliseconds{500},
           std::chrono::milliseconds debounce =
               std::chrono::milliseconds{1000});
  [[nodiscard]] bool scan_changed();
  [[nodiscard]] std::vector<FileChange> scan_changes();

 private:
  struct FileState {
    std::uintmax_t size;
    std::filesystem::file_time_type modified;

    bool operator==(const FileState&) const = default;
  };

  [[nodiscard]] bool excluded(const std::filesystem::path& relative) const;
  [[nodiscard]] std::unordered_map<std::string, FileState> snapshot() const;

  std::filesystem::path root_;
  std::vector<std::string> excludes_;
  std::unordered_map<std::string, FileState> previous_;
};

}  // namespace across
