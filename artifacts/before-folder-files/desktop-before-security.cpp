#include <raylib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#endif

#include "across/config.hpp"
#include "across/management.hpp"
#include "across/packages.hpp"
#include "across/deployer.hpp"
#include "across/watcher.hpp"

#ifndef _WIN32
extern char** environ;
#endif

#ifdef __APPLE__
std::string choose_project_folder(const std::string& initial);
#else
std::string choose_project_folder(const std::string&) {
  const auto result = across::run_process({"zenity", "--file-selection", "--directory", "--title=Select project folder"});
  if (!result.ok()) return {};
  auto path = result.output;
  while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
  return path;
}
#endif

namespace {

constexpr Color kBackground{9, 14, 22, 255};
constexpr Color kSidebar{12, 18, 28, 255};
constexpr Color kSurface{15, 22, 33, 255};
constexpr Color kSurfaceRaised{20, 29, 42, 255};
constexpr Color kField{11, 17, 26, 255};
constexpr Color kBorder{39, 52, 70, 255};
constexpr Color kBorderStrong{57, 74, 97, 255};
constexpr Color kText{235, 241, 249, 255};
constexpr Color kMuted{139, 153, 174, 255};
constexpr Color kFaint{87, 101, 122, 255};
constexpr Color kAccent{83, 137, 255, 255};
constexpr Color kAccentHover{102, 152, 255, 255};
constexpr Color kDanger{245, 105, 116, 255};
constexpr Color kDangerSurface{48, 25, 34, 255};
constexpr Color kSuccess{55, 211, 153, 255};

enum class FontWeight { regular, semibold };

std::filesystem::path find_asset(const std::filesystem::path& relative_path) {
  const std::filesystem::path application_directory{GetApplicationDirectory()};
  const std::vector<std::filesystem::path> candidates{
      application_directory / ".." / "Resources" / "assets" / relative_path,
      application_directory / "assets" / relative_path,
      application_directory / ".." / "share" / "across" / "assets" /
          relative_path,
      std::filesystem::path{ACROSS_ASSET_SOURCE_DIR} / relative_path,
  };
  std::error_code error;
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate, error)) {
      return std::filesystem::weakly_canonical(candidate, error);
    }
    error.clear();
  }
  throw std::runtime_error("Required desktop asset is missing: " +
                           relative_path.string());
}

Font load_ui_font(const std::filesystem::path& relative_path) {
  const auto path = find_asset(relative_path);
  Font font = LoadFontEx(path.string().c_str(), 48, nullptr, 0);
  if (!IsFontValid(font) || font.texture.id == GetFontDefault().texture.id) {
    throw std::runtime_error("Could not load bundled UI font: " + path.string());
  }
  SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
  return font;
}

struct UiFonts {
  Font regular{load_ui_font("fonts/Inter-Regular.ttf")};
  Font semibold{load_ui_font("fonts/Inter-SemiBold.ttf")};

  ~UiFonts() {
    UnloadFont(semibold);
    UnloadFont(regular);
  }
};

UiFonts* active_fonts = nullptr;
Texture2D active_logo{};

struct UiLogo {
  UiLogo() {
    const auto path = find_asset("across-icon.png").string();
    Image image = LoadImage(path.c_str());
    if (!IsImageValid(image)) throw std::runtime_error("Could not load Across icon");
    SetWindowIcon(image);
    active_logo = LoadTextureFromImage(image);
    UnloadImage(image);
    SetTextureFilter(active_logo, TEXTURE_FILTER_BILINEAR);
  }
  ~UiLogo() { UnloadTexture(active_logo); active_logo = {}; }
};

const Font& ui_font(FontWeight weight) {
  if (active_fonts == nullptr) throw std::logic_error("UI fonts are not loaded");
  return weight == FontWeight::semibold ? active_fonts->semibold
                                        : active_fonts->regular;
}

Vector2 measure_text(const std::string& text, float size,
                     FontWeight weight = FontWeight::regular) {
  return MeasureTextEx(ui_font(weight), text.c_str(), size, 0.0F);
}

void draw_text(const std::string& text, float x, float y, float size, Color color,
               FontWeight weight = FontWeight::regular) {
  DrawTextEx(ui_font(weight), text.c_str(), {x, y}, size, 0.0F, color);
}



std::filesystem::path executable_path(const char* argument_zero) {
  if (argument_zero == nullptr || *argument_zero == '\0') {
    throw std::runtime_error("Could not determine the Across executable path");
  }
  std::error_code error;
  auto path = std::filesystem::path{argument_zero};
  if (path.is_relative()) path = std::filesystem::absolute(path, error);
  const auto canonical = std::filesystem::weakly_canonical(path, error);
  return error ? path : canonical;
}

#ifdef _WIN32
std::wstring quote_windows_argument(const std::wstring& argument) {
  std::wstring result{L"\""};
  unsigned int slashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++slashes;
    } else if (character == L'\"') {
      result.append(slashes * 2U + 1U, L'\\');
      result.push_back(character);
      slashes = 0;
    } else {
      result.append(slashes, L'\\');
      result.push_back(character);
      slashes = 0;
    }
  }
  result.append(slashes * 2U, L'\\');
  result.push_back(L'\"');
  return result;
}
#endif

void launch_file_browser(const std::filesystem::path& executable,
                         const std::string& server_id,
                         const std::filesystem::path& local_directory,
                         const std::string& remote_directory) {
#ifdef _WIN32
  std::wstring command = quote_windows_argument(executable.wstring()) +
                         L" --file-browser " +
                         quote_windows_argument(
                             std::filesystem::path{server_id}.wstring()) +
                         L" " + quote_windows_argument(local_directory.wstring()) +
                         L" " + quote_windows_argument(
                                      std::filesystem::path{remote_directory}.wstring());
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.wstring().c_str(), command.data(), nullptr,
                      nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
    throw std::runtime_error("Could not open the file browser window");
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
#else
  std::vector<std::string> arguments{
      executable.string(), "--file-browser", server_id, local_directory.string(),
      remote_directory};
  std::vector<char*> raw_arguments;
  raw_arguments.reserve(arguments.size() + 1);
  for (auto& argument : arguments) raw_arguments.push_back(argument.data());
  raw_arguments.push_back(nullptr);
  pid_t process_id{};
  const int status = posix_spawn(&process_id, executable.c_str(), nullptr, nullptr,
                                 raw_arguments.data(), environ);
  if (status != 0) {
    throw std::runtime_error("Could not open the file browser window: " +
                             std::string{std::strerror(status)});
  }
  std::thread([process_id] {
    int child_status{};
    static_cast<void>(waitpid(process_id, &child_status, 0));
  }).detach();
#endif
}

bool contains(Rectangle rectangle, Vector2 point) {
  return CheckCollisionPointRec(point, rectangle);
}

std::string field_help;
Rectangle field_help_bounds{};

void draw_field_help() {
  if (field_help.empty()) return;
  const float width = std::min(700.0F, static_cast<float>(GetScreenWidth()) - 32);
  const float left = std::clamp(field_help_bounds.x, 16.0F, static_cast<float>(GetScreenWidth()) - width - 16);
  const float top = std::min(field_help_bounds.y + field_help_bounds.height + 6, static_cast<float>(GetScreenHeight()) - 72);
  Rectangle box{left, top, width, 48};
  DrawRectangleRounded(box, 0.18F, 6, kSurfaceRaised);
  DrawRectangleRoundedLinesEx(box, 0.18F, 6, 1, kBorderStrong);
  std::string first, second;
  std::istringstream words(field_help);
  std::string word;
  while (words >> word) {
    if (second.empty() && measure_text(first + word + " ", 11).x < width - 24) first += word + " ";
    else second += word + " ";
  }
  while (!second.empty() && measure_text(second, 11).x > width - 24) second.pop_back();
  draw_text(first, left + 12, top + 9, 11, kText);
  draw_text(second, left + 12, top + 27, 11, kMuted);
}

struct TextField {
  std::string label;
  std::string value;
  std::string example;
  std::string guidance;
  Rectangle bounds{};
  bool active{false};
  bool secret{false};
  bool select_all{false};
  std::size_t max_length{1024};

  explicit TextField(std::string field_label, std::string initial_value = {},
                     std::string field_example = {}, std::string field_guidance = {})
      : label(std::move(field_label)), value(std::move(initial_value)),
        example(std::move(field_example)), guidance(std::move(field_guidance)) {}

  void update() {
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      active = contains(bounds, GetMousePosition());
      if (active && GetMousePosition().x > bounds.x + bounds.width - 28) {
        value.clear(); select_all = false;
      }
    }
    if (!active) return;
    const bool control = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) || IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (control && IsKeyPressed(KEY_A)) select_all = true;
    if (control && IsKeyPressed(KEY_C) && !secret) SetClipboardText(value.c_str());
    int character = GetCharPressed();
    while (character > 0) {
      if (character >= 32 && character < 127 && value.size() < max_length) {
        if (select_all) { value.clear(); select_all = false; }
        value.push_back(static_cast<char>(character));
      }
      character = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
      if (select_all) { value.clear(); select_all = false; }
      else if (!value.empty()) value.pop_back();
    }
    if ((IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) ||
         IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) &&
        IsKeyPressed(KEY_V)) {
      if (const char* clipboard = GetClipboardText(); clipboard != nullptr) {
        if (select_all) { value.clear(); select_all = false; }
        std::string paste{clipboard};
        paste.erase(std::remove(paste.begin(), paste.end(), '\n'), paste.end());
        paste.erase(std::remove(paste.begin(), paste.end(), '\r'), paste.end());
        if (value.size() + paste.size() > max_length) {
          paste.resize(max_length - value.size());
        }
        value += paste;
      }
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_TAB)) active = false;
  }

  void draw() const {
    draw_text(label, bounds.x, bounds.y - 18.0F, 12.0F, kMuted,
              FontWeight::semibold);
    DrawRectangleRounded(bounds, 0.14F, 6, kField);
    DrawRectangleRoundedLinesEx(bounds, 0.14F, 6, 1.0F,
                                active ? kAccent : kBorder);
    if (!value.empty()) draw_text("x", bounds.x + bounds.width - 23, bounds.y + 11, 14, kMuted);
    const bool placeholder = value.empty();
    std::string visible = placeholder ? example : secret ? std::string(value.size(), '*') : value;
    const float available = bounds.width - 46.0F;
    bool clipped = false;
    while (!visible.empty() && measure_text(visible, 14.5F).x > available) {
      if (placeholder) visible.pop_back();
      else visible.erase(visible.begin());
      clipped = true;
    }
    if (clipped && visible.size() > 3) visible.replace(placeholder ? visible.size() - 3 : 0, 3, "...");
    if (active && select_all) DrawRectangleRec({bounds.x + 9, bounds.y + 7, std::min(available + 4, measure_text(visible, 14.5F).x + 4), 24}, kSurfaceRaised);
    draw_text(visible, bounds.x + 11.0F, bounds.y + 10.0F, 14.5F, placeholder ? kMuted : kText);
    if (contains(bounds, GetMousePosition()) || (active && field_help.empty())) {
      field_help = guidance + (example.empty() ? "" : " Example: " + example);
      field_help_bounds = bounds;
    }
    if (active && (static_cast<int>(GetTime() * 2) % 2 == 0)) {
      const float width = placeholder ? 0.0F : measure_text(visible, 14.5F).x;
      DrawRectangleV({bounds.x + 11.0F + width, bounds.y + 9.0F}, {1.0F, 19.0F},
                     kAccent);
    }
  }
};

enum class ButtonStyle { primary, secondary, danger, ghost };

bool button(Rectangle bounds, const std::string& label, bool enabled = true,
            ButtonStyle style = ButtonStyle::secondary) {
  const bool hovered = enabled && contains(bounds, GetMousePosition());
  Color fill = kSurfaceRaised;
  Color border = kBorder;
  Color label_color = kText;
  if (style == ButtonStyle::primary) {
    fill = hovered ? kAccentHover : kAccent;
    border = fill;
    label_color = WHITE;
  } else if (style == ButtonStyle::danger) {
    fill = hovered ? Color{63, 31, 41, 255} : kDangerSurface;
    border = Color{104, 45, 57, 255};
    label_color = kDanger;
  } else if (style == ButtonStyle::ghost) {
    fill = hovered ? kSurfaceRaised : Color{0, 0, 0, 0};
    border = fill;
    label_color = kMuted;
  } else if (hovered) {
    fill = Color{26, 37, 53, 255};
    border = kBorderStrong;
  }
  if (!enabled) {
    fill = kField;
    border = kBorder;
    label_color = kFaint;
  }
  DrawRectangleRounded(bounds, 0.20F, 7, fill);
  DrawRectangleRoundedLinesEx(bounds, 0.20F, 7, 1.0F, border);
  const float size = 13.0F;
  const Vector2 text_size = measure_text(label, size, FontWeight::semibold);
  draw_text(label, bounds.x + (bounds.width - text_size.x) / 2.0F,
            bounds.y + (bounds.height - text_size.y) / 2.0F - 1.0F, size,
            label_color, FontWeight::semibold);
  return hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}



bool is_shareable_path(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) ||
         std::filesystem::is_directory(path, error);
}

std::string safe_remote_component(const std::filesystem::path& path) {
  const std::string source = path.filename().string();
  std::string result;
  for (const char character : source) {
    const auto safe_character = static_cast<unsigned char>(character);
    if (std::isalnum(safe_character) || character == '.' || character == '_' ||
        character == '-' || character == '+') {
      result.push_back(character);
    } else if (result.empty() || result.back() != '-') {
      result.push_back('-');
    }
  }
  while (!result.empty() && result.back() == '-') result.pop_back();
  if (result.empty() || result == "." || result == "..") return "shared-item";
  return result;
}

std::string remote_child_path(std::string directory, const std::string& name) {
  if (directory.empty() || directory.front() != '/') {
    throw std::runtime_error("Remote queue destination must be an absolute directory");
  }
  while (directory.size() > 1 && directory.ends_with('/')) directory.pop_back();
  return directory == "/" ? "/" + name : directory + "/" + name;
}

bool is_safe_remote_name(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  return std::all_of(name.begin(), name.end(), [](const char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isalnum(value) || character == '.' || character == '_' ||
           character == '@' || character == '+' || character == ':' ||
           character == '-';
  });
}

std::string remote_existing_child(const std::string& directory,
                                  const std::string& name) {
  if (!is_safe_remote_name(name)) {
    throw std::runtime_error(
        "This remote name contains characters that Across cannot transfer safely");
  }
  return remote_child_path(directory, name);
}

struct LocalFileEntry {
  std::filesystem::path path;
  std::string name;
  bool directory{false};
};

struct RemoteFileEntry {
  std::string name;
  bool directory{false};
};

struct RemoteListing {
  std::vector<RemoteFileEntry> entries;
  std::string error;
};

struct TransferOutcome {
  bool ok{false};
  std::string message;
};

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  });
  return value;
}

template <typename Entry>
void sort_file_entries(std::vector<Entry>& entries) {
  std::sort(entries.begin(), entries.end(), [](const Entry& left,
                                                const Entry& right) {
    if (left.directory != right.directory) return left.directory;
    return lowercase(left.name) < lowercase(right.name);
  });
}

std::vector<LocalFileEntry> read_local_directory(
    const std::filesystem::path& directory) {
  std::vector<LocalFileEntry> entries;
  std::error_code error;
  std::filesystem::directory_iterator iterator{
      directory, std::filesystem::directory_options::skip_permission_denied,
      error};
  if (error) {
    throw std::runtime_error("Could not open local folder: " + error.message());
  }
  for (const auto& item : iterator) {
    std::error_code type_error;
    const bool directory_entry = item.is_directory(type_error);
    if (type_error) continue;
    entries.push_back(
        {item.path(), item.path().filename().string(), directory_entry});
  }
  sort_file_entries(entries);
  return entries;
}

RemoteListing read_remote_directory(const across::ServerProfile& server,
                                    const std::string& directory) {
  RemoteListing listing;
  const auto result = across::Deployer(server).remote_command(
      {"find", directory, "-mindepth", "1", "-maxdepth", "1", "-printf",
       "%y\\0%f\\0"});
  if (!result.ok()) {
    listing.error = result.output.empty() ? "Could not read the server folder"
                                          : result.output;
    return listing;
  }

  std::size_t position = 0;
  while (position < result.output.size()) {
    const std::size_t type_end = result.output.find('\0', position);
    if (type_end == std::string::npos || type_end == position) break;
    const std::size_t name_end = result.output.find('\0', type_end + 1);
    if (name_end == std::string::npos) break;
    const char type = result.output[position];
    std::string name = result.output.substr(type_end + 1, name_end - type_end - 1);
    if (!name.empty() && name != "." && name != "..") {
      listing.entries.push_back({std::move(name), type == 'd'});
    }
    position = name_end + 1;
  }
  sort_file_entries(listing.entries);
  return listing;
}

enum class BrowserDragSource { none, local, remote };

struct CodeEditor {
  std::string value;
  Rectangle bounds{};
  bool active{false};
  std::size_t cursor{0};
  bool select_all{false};
  int scroll{0};
  void update() {
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      active = contains(bounds, GetMousePosition()); select_all = false;
      if (active) {
        const auto mouse = GetMousePosition();
        const int line_index = scroll + std::max(0, static_cast<int>((mouse.y - bounds.y - 10) / 19));
        cursor = 0;
        for (int line = 0; line < line_index; ++line) { const auto next = value.find('\n', cursor); if (next == std::string::npos) { cursor = value.size(); break; } cursor = next + 1; }
        const auto start = cursor;
        while (cursor < value.size() && value[cursor] != '\n' && measure_text(value.substr(start, cursor - start + 1), 12).x < mouse.x - bounds.x - 47) ++cursor;
      }
    }
    cursor = std::min(cursor, value.size());
    if (contains(bounds, GetMousePosition())) scroll = std::max(0, scroll - static_cast<int>(GetMouseWheelMove() * 3));
    if (!active) return;
    auto insert = [&](const std::string& text) { if ((select_all ? 0 : value.size()) + text.size() <= 65536) { if (select_all) { value.clear(); cursor = 0; select_all = false; } value.insert(cursor, text); cursor += text.size(); } };
    const bool control = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) || IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (control && IsKeyPressed(KEY_A)) select_all = true;
    if (control && IsKeyPressed(KEY_V)) { if (const char* text = GetClipboardText()) insert(text); }
    else { int character; while ((character = GetCharPressed()) != 0) if (character >= 32 && character < 127) insert(std::string(1, static_cast<char>(character))); }
    if (control && IsKeyPressed(KEY_C)) SetClipboardText(value.c_str());
    if (IsKeyPressed(KEY_ENTER)) insert("\n");
    if (IsKeyPressed(KEY_TAB)) insert("  ");
    if (select_all && (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_DELETE))) { value.clear(); cursor = 0; select_all = false; }
    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && cursor > 0) value.erase(--cursor, 1);
    if ((IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) && cursor < value.size()) value.erase(cursor, 1);
    if ((IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) && cursor > 0) --cursor;
    if ((IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) && cursor < value.size()) ++cursor;
    const auto before = cursor == 0 ? std::string::npos : value.rfind('\n', cursor - 1);
    const auto start = before == std::string::npos ? 0 : before + 1;
    const auto after = value.find('\n', cursor);
    if (IsKeyPressed(KEY_HOME)) cursor = start;
    if (IsKeyPressed(KEY_END)) cursor = after == std::string::npos ? value.size() : after;
    if (IsKeyPressed(KEY_UP) && start > 0) {
      const auto previous = start < 2 ? std::string::npos : value.rfind('\n', start - 2);
      const auto previous_start = previous == std::string::npos ? 0 : previous + 1;
      cursor = previous_start + std::min(cursor - start, start - 1 - previous_start);
    }
    if (IsKeyPressed(KEY_DOWN) && after != std::string::npos) {
      const auto next_end = value.find('\n', after + 1);
      cursor = after + 1 + std::min(cursor - start, (next_end == std::string::npos ? value.size() : next_end) - after - 1);
    }
    const int line_count = static_cast<int>(std::count(value.begin(), value.end(), '\n')) + 1;
    const int visible = std::max(1, static_cast<int>((bounds.height - 20) / 19));
    scroll = std::clamp(scroll, 0, std::max(0, line_count - visible));
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_BACKSPACE)) {
      const int line = static_cast<int>(std::count(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(cursor), '\n'));
      if (line < scroll) scroll = line;
      if (line >= scroll + visible) scroll = line - visible + 1;
    }
  }
  void draw() const {
    DrawRectangleRec(bounds, kField); DrawRectangleLinesEx(bounds, 1, active ? kAccent : kBorder);
    BeginScissorMode(static_cast<int>(bounds.x + 8), static_cast<int>(bounds.y + 8), static_cast<int>(bounds.width - 16), static_cast<int>(bounds.height - 16));
    std::istringstream lines(value); std::string line; int index = 0; std::size_t offset = 0;
    while (std::getline(lines, line)) {
      if (index >= scroll) {
        const float y = bounds.y + 10 + static_cast<float>(index - scroll) * 19;
        if (y > bounds.y + bounds.height) break;
        draw_text(std::to_string(index + 1), bounds.x + 8, y, 12, kFaint);
        draw_text(line, bounds.x + 47, y, 12, kText);
        if (active && cursor >= offset && cursor <= offset + line.size()) DrawRectangle(static_cast<int>(bounds.x + 47 + measure_text(line.substr(0, cursor - offset), 12).x), static_cast<int>(y), 1, 16, kAccent);
      }
      offset += line.size() + 1; ++index;
    }
    EndScissorMode();
  }
};


struct FileBrowser {
  CodeEditor editor;
  bool editing{false};
  bool editing_remote{false};
  std::string editing_path;
  std::string original_text;
  std::future<std::string> editor_job;
  bool loading_editor{false};
  across::ServerProfile server;
  std::filesystem::path local_directory;
  std::string remote_directory;
  std::vector<LocalFileEntry> local_entries;
  std::vector<RemoteFileEntry> remote_entries;
  int local_selected{-1};
  int remote_selected{-1};
  int local_scroll{0};
  int remote_scroll{0};
  std::future<RemoteListing> remote_job;
  std::future<TransferOutcome> transfer_job;
  TextField new_name{"Name", "", "release-backup", "Name for a new folder or the selected item."};
  double trash_confirmation{-10};
  bool loading_remote{false};
  bool transferring{false};
  std::string status{"Ready"};
  BrowserDragSource drag_source{BrowserDragSource::none};
  int drag_index{-1};
  Vector2 drag_start{};
  bool dragging{false};
  BrowserDragSource last_click_source{BrowserDragSource::none};
  int last_click_index{-1};
  double last_click_time{-1.0};

  FileBrowser(across::ServerProfile profile,
              std::filesystem::path initial_local_directory,
              std::string initial_remote_directory, bool connect_server = true)
      : server(std::move(profile)),
        local_directory(std::move(initial_local_directory)),
        remote_directory(std::move(initial_remote_directory)) {
    std::error_code error;
    if (!std::filesystem::is_directory(local_directory, error)) {
      if (const char* home = std::getenv("HOME"); home != nullptr) {
        local_directory = home;
      } else if (const char* profile_path = std::getenv("USERPROFILE");
                 profile_path != nullptr) {
        local_directory = profile_path;
      } else {
        local_directory = std::filesystem::current_path();
      }
    }
    if (remote_directory.empty() || remote_directory.front() != '/') {
      remote_directory = "/tmp";
    }
    refresh_local();
    if (connect_server) refresh_remote();
  }

  ~FileBrowser() {
    if (editor_job.valid()) editor_job.wait();
    if (remote_job.valid()) remote_job.wait();
    if (transfer_job.valid()) transfer_job.wait();
  }

  void refresh_local() {
    try {
      local_entries = read_local_directory(local_directory);
      local_selected = -1;
      local_scroll = 0;
    } catch (const std::exception& error) {
      status = error.what();
    }
  }

  void refresh_remote() {
    if (loading_remote) return;
    loading_remote = true;
    remote_selected = -1;
    remote_scroll = 0;
    status = "Loading server folder...";
    const auto server_snapshot = server;
    const auto directory_snapshot = remote_directory;
    remote_job = std::async(std::launch::async,
                            [server_snapshot, directory_snapshot] {
      return read_remote_directory(server_snapshot, directory_snapshot);
    });
  }

  void poll_jobs() {
    if (loading_editor && editor_job.valid() && editor_job.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
      try {
        original_text = editor_job.get(); editor.value = original_text;
        editor.cursor = 0; editor.scroll = 0; editing = true;
        status = "Text file opened";
      } catch (const std::exception& error) { status = error.what(); }
      loading_editor = false;
    }
    if (loading_remote && remote_job.valid() &&
        remote_job.wait_for(std::chrono::seconds{0}) ==
            std::future_status::ready) {
      try {
        auto listing = remote_job.get();
        if (listing.error.empty()) {
          remote_entries = std::move(listing.entries);
          status = "Server folder loaded";
        } else {
          remote_entries.clear();
          status = "Server folder failed: " + listing.error;
        }
      } catch (const std::exception& error) {
        remote_entries.clear();
        status = std::string{"Server folder failed: "} + error.what();
      }
      loading_remote = false;
    }
    if (transferring && transfer_job.valid() &&
        transfer_job.wait_for(std::chrono::seconds{0}) ==
            std::future_status::ready) {
      try {
        const auto outcome = transfer_job.get();
        status = outcome.message;
        if (outcome.ok && editing) original_text = editor.value;
      } catch (const std::exception& error) {
        status = std::string{"Transfer failed: "} + error.what();
      }
      transferring = false;
      refresh_local();
      refresh_remote();
    }
  }

  static std::string read_text_file(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 65536) throw std::runtime_error("Select a text file up to 64 KiB");
    std::ifstream input(path, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (!input || text.find('\0') != std::string::npos) throw std::runtime_error("Cannot edit this file as text");
    return text;
  }

  void open_editor(bool remote) {
    if (transferring || loading_remote || loading_editor) return;
    editing_remote = remote;
    if (!remote) {
      const auto& entry = local_entries.at(static_cast<std::size_t>(local_selected));
      if (entry.directory) throw std::runtime_error("Select a text file");
      editing_path = entry.path.string();
      original_text = read_text_file(entry.path); editor.value = original_text;
      editor.cursor = 0; editor.scroll = 0; editing = true;
      return;
    }
    const auto& entry = remote_entries.at(static_cast<std::size_t>(remote_selected));
    if (entry.directory) throw std::runtime_error("Select a text file");
    editing_path = remote_existing_child(remote_directory, entry.name);
    const auto path = editing_path; const auto profile = server;
    loading_editor = true; status = "Opening server text file...";
    editor_job = std::async(std::launch::async, [profile, path] {
      across::SshTransport transport(profile);
      const auto quoted = across::shell_quote(path);
      const auto result = transport.execute_shell("set -eu; test -f " + quoted + "; test ! -L " + quoted + "; test $(stat -c %s -- " + quoted + ") -le 65536; cat -- " + quoted, std::chrono::seconds{30});
      if (!result.ok()) throw std::runtime_error("Cannot open server text file: " + result.output);
      if (result.output.size() > 65536 || result.output.find('\0') != std::string::npos) throw std::runtime_error("Select a text file up to 64 KiB");
      return result.output;
    });
  }

  void save_editor() {
    const auto text = editor.value;
    if (!editing_remote) {
      if (read_text_file(editing_path) != original_text) throw std::runtime_error("File changed on disk. Reopen it before saving.");
      const auto backup = editing_path + ".across-backup-" + across::make_release_id();
      std::filesystem::copy_file(editing_path, backup);
      std::ofstream output(editing_path, std::ios::binary); output << text; output.close();
      if (!output) throw std::runtime_error("Could not save file; backup retained at " + backup);
      original_text = text; status = "Local file saved; backup retained";
      return;
    }
    const auto profile = server; const auto path = editing_path; const auto original = original_text;
    transferring = true; status = "Saving server text file...";
    transfer_job = std::async(std::launch::async, [profile, path, original, text] {
      const auto token = across::make_release_id();
      const auto local = std::filesystem::temp_directory_path() / ("across-edit-" + token);
      std::filesystem::create_directory(local);
      std::filesystem::permissions(local, std::filesystem::perms::owner_all);
      const auto remote = "/tmp/across-edit-" + token;
      across::SshTransport transport(profile);
      auto result = transport.execute({"mkdir", "-m", "700", "--", remote});
      try {
        if (!result.ok()) throw std::runtime_error(result.output);
        { std::ofstream output(local / "original", std::ios::binary); output << original; }
        { std::ofstream output(local / "updated", std::ios::binary); output << text; }
        for (const auto& name : {"original", "updated"}) {
          result = across::Deployer(profile).upload(local / name, remote + "/" + name);
          if (!result.ok()) throw std::runtime_error(result.output);
        }
        const auto quoted = across::shell_quote(path);
        const auto backup = across::shell_quote(path + ".across-backup-" + token);
        result = transport.execute_shell("set -eu; test -f " + quoted + "; test ! -L " + quoted + "; cmp -s -- " + quoted + " " + across::shell_quote(remote + "/original") + "; cp -p -- " + quoted + " " + backup + "; cat -- " + across::shell_quote(remote + "/updated") + " > " + quoted, std::chrono::seconds{30});
        transport.execute({"rm", "-r", "--", remote});
        std::filesystem::remove_all(local);
        return TransferOutcome{result.ok(), result.ok() ? "Server file saved; backup retained" : "Save failed or file changed. Reopen before saving: " + result.output};
      } catch (...) {
        transport.execute({"rm", "-r", "--", remote});
        std::filesystem::remove_all(local); throw;
      }
    });
  }

  void enter_local(int index) {
    if (index < 0 || index >= static_cast<int>(local_entries.size()) ||
        !local_entries[static_cast<std::size_t>(index)].directory) {
      return;
    }
    local_directory = local_entries[static_cast<std::size_t>(index)].path;
    refresh_local();
  }

  void local_up() {
    const auto parent = local_directory.parent_path();
    if (!parent.empty() && parent != local_directory) {
      local_directory = parent;
      refresh_local();
    }
  }

  void enter_remote(int index) {
    if (index < 0 || index >= static_cast<int>(remote_entries.size()) ||
        !remote_entries[static_cast<std::size_t>(index)].directory ||
        loading_remote) {
      return;
    }
    try {
      remote_directory = remote_existing_child(
          remote_directory, remote_entries[static_cast<std::size_t>(index)].name);
      refresh_remote();
    } catch (const std::exception& error) {
      status = error.what();
    }
  }

  void remote_up() {
    if (remote_directory == "/" || loading_remote) return;
    const auto separator = remote_directory.find_last_of('/');
    remote_directory = separator == 0 ? "/" : remote_directory.substr(0, separator);
    refresh_remote();
  }

  void start_upload(std::vector<std::filesystem::path> items,
                    const std::string& destination_directory) {
    if (transferring || items.empty()) return;
    if (loading_remote) {
      status = "Wait for the server folder to finish loading";
      return;
    }
    const auto server_snapshot = server;
    transferring = true;
    status = "Uploading " + std::to_string(items.size()) + " item(s)...";
    transfer_job = std::async(
        std::launch::async,
        [server_snapshot, items = std::move(items), destination_directory] {
      std::vector<std::string> names;
      for (const auto& item : items) {
        const std::string base_name = safe_remote_component(item);
        std::string name = base_name;
        unsigned int suffix = 2;
        while (std::find(names.begin(), names.end(), name) != names.end()) {
          name = base_name + "-" + std::to_string(suffix++);
        }
        names.push_back(name);
        const auto destination = remote_child_path(destination_directory, name);
        const auto result =
            across::Deployer(server_snapshot).upload(item, destination);
        if (!result.ok()) {
          return TransferOutcome{false, "Upload failed: " + result.output};
        }
      }
      return TransferOutcome{true, "Upload completed"};
    });
  }

  void upload_selected(const std::string& destination_directory = {}) {
    if (local_selected < 0 ||
        local_selected >= static_cast<int>(local_entries.size())) {
      status = "Select a local file or folder first";
      return;
    }
    start_upload(
        {local_entries[static_cast<std::size_t>(local_selected)].path},
        destination_directory.empty() ? remote_directory : destination_directory);
  }

  void start_download(int remote_index,
                      const std::filesystem::path& destination_directory) {
    if (transferring || loading_remote || remote_index < 0 ||
        remote_index >= static_cast<int>(remote_entries.size())) {
      return;
    }
    try {
      const auto entry = remote_entries[static_cast<std::size_t>(remote_index)];
      const auto remote_source =
          remote_existing_child(remote_directory, entry.name);
      const auto local_destination = destination_directory / entry.name;
      const auto server_snapshot = server;
      transferring = true;
      status = "Downloading " + entry.name + "...";
      transfer_job = std::async(
          std::launch::async,
          [server_snapshot, remote_source, local_destination, name = entry.name] {
        const auto result = across::Deployer(server_snapshot)
                                .download(remote_source, local_destination);
        if (!result.ok()) {
          return TransferOutcome{false, "Download failed: " + result.output};
        }
        return TransferOutcome{true, "Downloaded " + name};
      });
    } catch (const std::exception& error) {
      status = error.what();
    }
  }

  void mutate_remote(const std::string& action) {
    if (transferring || loading_remote) return;
    const auto profile = server;
    std::string source;
    if (action != "mkdir") {
      if (remote_selected < 0) throw std::runtime_error("Select a server file or folder first");
      source = remote_existing_child(remote_directory, remote_entries.at(static_cast<std::size_t>(remote_selected)).name);
    }
    if (action != "trash" && !is_safe_remote_name(new_name.value)) throw std::runtime_error("Enter a name using letters, numbers, dot, dash or underscore");
    const auto target = action == "trash" ? remote_child_path(remote_directory, ".across-trash-" + across::make_release_id()) : remote_child_path(remote_directory, new_name.value);
    transferring = true;
    transfer_job = std::async(std::launch::async, [profile, action, source, target] {
      across::SshTransport transport(profile);
      const auto result = action == "mkdir" ? transport.execute({"mkdir", "--", target}) : transport.execute_shell("set -eu; test ! -e " + across::shell_quote(target) + "; test ! -L " + across::shell_quote(target) + "; mv -T -- " + across::shell_quote(source) + " " + across::shell_quote(target), std::chrono::seconds{30});
      return TransferOutcome{result.ok(), result.ok() ? "Saved server file change: " + target : result.output};
    });
  }

  void mutate_local(const std::string& action) {
    if (transferring || loading_editor) return;
    if (action != "trash" && !is_safe_remote_name(new_name.value)) throw std::runtime_error("Enter a name using letters, numbers, dot, dash or underscore");
    const auto target = local_directory / (action == "trash" ? ".across-trash-" + across::make_release_id() : new_name.value);
    if (std::filesystem::exists(target) || std::filesystem::is_symlink(target)) throw std::runtime_error("An item with that name already exists");
    if (action == "mkdir") std::filesystem::create_directory(target);
    else {
      if (local_selected < 0) throw std::runtime_error("Select a local file or folder first");
      std::filesystem::rename(local_entries.at(static_cast<std::size_t>(local_selected)).path, target);
    }
    refresh_local(); status = "Saved local file change: " + target.string();
  }

  void download_selected(const std::filesystem::path& destination = {}) {
    if (remote_selected < 0 ||
        remote_selected >= static_cast<int>(remote_entries.size())) {
      status = "Select a server file or folder first";
      return;
    }
    start_download(remote_selected,
                   destination.empty() ? local_directory : destination);
  }
};


struct Application {
  across::ConfigStore store;
  std::vector<across::ServerProfile> servers;
  int selected{-1};

  TextField server_label{"Server name", "", "DigitalOcean production", "A friendly label for this server."};
  TextField server_host{"Host", "", "178.128.144.213", "Enter the VPS IP address or SSH hostname."};
  TextField server_user{"SSH user", "ubuntu", "root", "Use the user your SSH key can log in as."};
  TextField server_port{"SSH port", "22", "22", "Usually 22; use the port configured on your VPS."};
  TextField identity{"SSH key", "", "/Users/you/.ssh/digiocean", "Full local path to your private SSH key."};
  TextField manifest{"Manifest", "", "/Users/you/Projects/my-app/across.json", "Load the Across manifest in your project folder."};
  TextField remote_command{"Remote command (arguments are not run through a shell)", "", "uname -a", "Enter a program and arguments; shell operators are not supported."};
  TextField app_id{"Application", "", "my-app", "A stable application ID using lowercase letters, numbers and hyphens."};

  int page{0};
  bool show_advanced{false};
  bool show_secrets{false};
  bool show_activity{false};
  int website_scroll{0};
  int package_scroll{0};
  TextField custom_package{"Add package", "", "git curl", "Ubuntu/Debian package names, separated by spaces."};
  nlohmann::json package_state = nlohmann::json::object();
  std::string transfer_protocol{"rsync"};
  TextField project_root{"Project folder", "", "/Users/you/Projects/my-app", "Full local path to the project to deploy."};
  TextField project_type{"Type", "static", "node", "Choose node for Next.js, static for built files, or proxy for upstreams."};
  TextField project_port{"Port", "3000", "3000", "The port the Node application listens on; Across sets PORT."};
  TextField domains{"Domains", "", "app.example.com www.example.com", "Hostnames or the VPS IP, separated by spaces. DNS must point to your VPS."};
  TextField artifact{"Output folder", "dist", "dist", "Static output folder relative to the uploaded project."};
  TextField install{"Install", "", "npm ci", "Optional dependency install command, run at the selected build location."};
  TextField build{"Build", "", "npm run build", "Optional build command, run at the selected location. Local Next.js builds need standalone output."};
  std::string build_location{"vps"};
  TextField deployment_dir{"Deployment folder", ".", ".next/standalone", "For local builds, upload this folder inside the project. Use . for the whole project."};
  TextField start{"Start", "npm start", "HOSTNAME=127.0.0.1 node server.js", "VPS start command. A Next.js standalone bundle starts with node server.js."};
  TextField upstreams{"Upstreams", "", "127.0.0.1:8000 127.0.0.1:8001", "Proxy backend host:port addresses, separated by spaces."};
  TextField body_limit{"Upload limit / MB", "20", "20", "Maximum HTTP request body size in megabytes."};
  TextField rate{"Rate limit / requests/s", "0", "10", "Requests per second per client IP. Use 0 to disable the limit."};
  TextField burst{"Burst", "20", "40", "Extra requests allowed above the rate limit before requests are rejected."};
  TextField email{"Certificate email", "", "you@example.com", "Email for Let's Encrypt certificate notices."};
  TextField dns_domain{"Hostname", "", "app.example.com", "The DNS hostname to point to the VPS."};
  TextField dns_address{"IP address", "", "178.128.144.213", "Public IPv4 address for the DNS A record."};
  TextField dns_zone{"Zone ID", "", "0123456789abcdef0123456789abcdef", "Cloudflare zone ID from the domain overview."};
  TextField dns_token_file{"API token file", "", "/Users/you/.config/across/cloudflare-token", "Local file containing a Cloudflare token with DNS edit access."};
  TextField branch{"Branch", "main", "main", "Git branch that triggers the deployment pipeline."};
  TextField pipeline_manifest{"Manifest", "across.json", "frontend/across.json", "Manifest path relative to the repository root."};
  TextField resource_service{"Service", "nginx.service", "across-pm2.service", "Systemd service whose CPU and memory limits you want to set."};
  TextField cpu_limit{"CPU / %", "100", "100", "100 percent allows one full CPU core; 50 allows half a core."};
  TextField memory_limit{"Memory / MiB", "512", "512", "Maximum service memory in mebibytes."};
  TextField firewall_port{"Port", "443", "443", "TCP or UDP port to allow or remove in the firewall."};
  TextField firewall_protocol{"Protocol", "tcp", "tcp", "Use tcp or udp to match the service protocol."};
  CodeEditor nginx_editor;
  nlohmann::json websites = nlohmann::json::array();
  nlohmann::json metrics = nlohmann::json::object();
  std::mutex data_mutex;
  nlohmann::json pending_data;
  std::string pending_kind;
  bool domain_only{false};
  nlohmann::json desktop_state = nlohmann::json::object();
  bool refresh_requested{false};
  bool show_recent_projects{false};
  std::unique_ptr<FileBrowser> file_browser;
  std::vector<TextField*> management_fields() {
    return {&server_label, &server_host, &server_user, &server_port, &identity, &manifest, &app_id, &project_root, &project_type, &project_port, &domains, &artifact, &install, &build, &deployment_dir, &start, &upstreams, &body_limit, &rate, &burst, &email, &dns_domain, &dns_address, &dns_zone, &dns_token_file, &branch, &pipeline_manifest, &resource_service, &cpu_limit, &memory_limit, &firewall_port, &firewall_protocol, &custom_package};
  }
  std::mutex log_mutex;
  std::string log;
  std::future<void> job;
  std::atomic_bool busy{false};
  std::atomic_bool stop_watcher{false};
  std::future<void> watcher_job;
  bool watcher_running{false};
  std::chrono::steady_clock::time_point remove_confirmation{};
  std::chrono::steady_clock::time_point profile_remove_confirmation{};

  Application() {
    try {
      std::ifstream input(across::config_directory() / "desktop.json");
      if (input) input >> desktop_state;
      if (!desktop_state.is_object()) desktop_state = nlohmann::json::object();
    } catch (const std::exception& error) { append_log(std::string{"Could not restore desktop state: "} + error.what()); }
    servers = store.list_servers();
    const int saved_page = std::clamp(desktop_state.value("page", 1), 0, 9);
    const auto saved_server = desktop_state.value("selected_server", "");
    const auto found = std::find_if(servers.begin(), servers.end(), [&](const auto& server) { return server.id == saved_server; });
    if (!servers.empty()) select(found == servers.end() ? 0 : static_cast<int>(found - servers.begin()));
    page = saved_page;
  }

  ~Application() {
    stop_watcher.store(true);
    if (watcher_job.valid()) watcher_job.wait();
    if (job.valid()) job.wait();
    save_desktop_state();
  }

  void save_desktop_state() {
    try {
      desktop_state["page"] = page;
      if (selected >= 0) desktop_state["selected_server"] = selected_server().id;
      const auto path = across::config_directory() / "desktop.json";
      std::filesystem::create_directories(path.parent_path());
      const auto temporary = path.string() + ".tmp";
      std::ofstream output(temporary); output << desktop_state.dump(2) << '\n'; output.close();
      if (!output) throw std::runtime_error("Could not save desktop state");
      std::filesystem::rename(temporary, path);
    } catch (const std::exception& error) { append_log(error.what()); }
  }

  void remember_project() {
    if (selected < 0 || manifest.value.empty()) return;
    const auto id = selected_server().id;
    auto projects = desktop_state.value("projects", nlohmann::json::object());
    auto paths = projects.value(id, std::vector<std::string>{});
    std::erase(paths, manifest.value); paths.insert(paths.begin(), manifest.value);
    if (paths.size() > 20) paths.resize(20);
    projects[id] = paths; desktop_state["projects"] = projects;
    save_desktop_state();
  }

  std::vector<std::string> recent_projects() const {
    if (selected < 0) return {};
    return desktop_state.value("projects", nlohmann::json::object()).value(selected_server().id, std::vector<std::string>{});
  }

  void refresh_all() {
    if (selected < 0 || busy.load()) return;
    const auto profile = selected_server(); const auto id = app_id.value;
    refresh_requested = false;
    start_job("Refresh all", [this, profile, id](const auto&) {
      nlohmann::json data = nlohmann::json::object();
      bool failed = false;
      auto fetch = [&](const std::string& name, std::vector<std::string> arguments) {
        const auto result = across::Deployer(profile).server_command(arguments);
        if (result.ok()) {
          try { data[name] = nlohmann::json::parse(result.output); }
          catch (const std::exception& error) { failed = true; append_log(name + ": " + error.what()); }
        } else { failed = true; append_log(name + " refresh failed: " + result.output); }
      };
      fetch("list", {"list", "--json"});
      auto selected_app = id;
      if (data.contains("list")) {
        const auto& apps = data["list"]["apps"];
        const auto found = std::find_if(apps.begin(), apps.end(), [&](const auto& app) { return app.value("app_id", "") == id; });
        if (found == apps.end()) selected_app = apps.empty() ? "" : apps.front().value("app_id", "");
      }
      if (!selected_app.empty()) fetch("website", {"website", "--app", selected_app, "--json"});
      fetch("metrics", {"metrics", "--json"});
      auto packages = profile.packages;
      for (const auto& name : profile.custom_packages) if (std::find(packages.begin(), packages.end(), name) == packages.end()) packages.push_back(name);
      fetch("packages", {"packages", "--packages", nlohmann::json(packages).dump(), "--json"});
      data["selected_app"] = selected_app; data["server"] = profile.id;
      { std::scoped_lock lock(data_mutex); pending_data = data; pending_kind = "all"; }
      return across::ProcessResult{failed ? 1 : 0, "", false};
    });
  }

  void append_log(const std::string& message) {
    std::scoped_lock lock(log_mutex);
    log += message;
    if (!log.ends_with('\n')) log.push_back('\n');
    constexpr std::size_t limit = 50000;
    if (log.size() > limit) log.erase(0, log.size() - limit);
  }

  std::string log_snapshot() {
    std::scoped_lock lock(log_mutex);
    return log;
  }

  void refresh_servers() {
    servers = store.list_servers();
    if (servers.empty()) {
      selected = -1;
    } else if (selected < 0 || selected >= static_cast<int>(servers.size())) {
      select(0);
    }
  }

  void select(int index) {
    if (index < 0 || index >= static_cast<int>(servers.size())) return;
    file_browser.reset();
    selected = index;
    const auto& server = servers[static_cast<std::size_t>(index)];
    server_label.value = server.label;
    server_host.value = server.host;
    server_user.value = server.user;
    server_port.value = std::to_string(server.port);
    identity.value = server.identity_file;
    transfer_protocol = server.transfer_protocol;
    website_scroll = 0;
    package_scroll = 0; package_state = nlohmann::json::object();
    websites = nlohmann::json::array(); metrics = nlohmann::json::object();
    app_id.value.clear(); nginx_editor.value.clear(); domains.value.clear(); domain_only = false;
    manifest.value.clear(); project_root.value.clear(); show_recent_projects = false;
    const auto paths = recent_projects();
    if (!paths.empty()) {
      try { load_project(paths.front()); }
      catch (const std::exception& error) { append_log(std::string{"Saved project unavailable: "} + error.what()); }
    }
    refresh_requested = true;
    save_desktop_state();
  }

  void new_server() {
    selected = -1;
    page = 6;
    server_label.value.clear();
    server_host.value.clear();
    server_user.value = "ubuntu";
    server_port.value = "22";
    identity.value.clear();
    append_log("Enter the new server connection and click Save server.");
  }

  void remove_selected_server() {
    const auto profile = selected_server();
    store.remove_server(profile.id);
    selected = -1;
    refresh_servers();
    append_log("Removed local server profile: " + profile.label);
  }

  across::ServerProfile selected_server() const {
    if (selected < 0 || selected >= static_cast<int>(servers.size())) {
      throw std::runtime_error("Select a server first");
    }
    return servers[static_cast<std::size_t>(selected)];
  }

  void save_server() {
    across::ServerProfile profile;
    profile.id = across::slugify(server_label.value);
    profile.label = server_label.value;
    profile.host = server_host.value;
    profile.user = server_user.value;
    const int port = std::stoi(server_port.value);
    if (port < 1 || port > 65535) throw std::runtime_error("Invalid SSH port");
    profile.port = static_cast<std::uint16_t>(port);
    profile.identity_file = identity.value;
    profile.transfer_protocol = transfer_protocol;
    if (selected >= 0) {
      profile.packages = selected_server().packages;
      profile.custom_packages = selected_server().custom_packages;
    }
    store.save_server(profile);
    refresh_servers();
    const auto found = std::find_if(servers.begin(), servers.end(),
                                    [&](const auto& value) {
                                      return value.id == profile.id;
                                    });
    if (found != servers.end()) select(static_cast<int>(found - servers.begin()));
    append_log("Saved server profile: " + profile.label);
  }

  void select_dropped_manifest(FilePathList dropped) {
    if (dropped.count == 0) return;
    if (dropped.count != 1) {
      append_log("Drop one across.json file or project folder onto the manifest field.");
      return;
    }
    std::filesystem::path candidate{dropped.paths[0]};
    std::error_code error;
    if (std::filesystem::is_directory(candidate, error)) candidate /= "across.json";
    if (std::filesystem::is_regular_file(candidate, error) &&
        candidate.filename() == "across.json") {
      manifest.value = candidate.string();
      manifest.active = false;
      append_log("Selected deployment manifest: " + manifest.value);
    } else {
      append_log(
          "Manifest drop ignored: choose an across.json file or a folder containing one.");
    }
  }

  void load_project(const std::filesystem::path& path) {
    const auto project = across::load_manifest(path);
    manifest.value = path.string();
    project_root.value = project.project_root.string(); app_id.value = project.app_id;
    domains.value.clear(); for (const auto& domain : project.nginx.domains) domains.value += domain + " ";
    project_type.value = project.type; project_port.value = std::to_string(project.port);
    artifact.value = project.artifact_dir; install.value = project.install_command;
    build.value = project.build_command; start.value = project.start_command;
    build_location = project.build_location; deployment_dir.value = project.deployment_dir;
    upstreams.value.clear(); for (const auto& upstream : project.nginx.upstreams) upstreams.value += upstream + " ";
    append_log("Loaded project settings.");
    remember_project();
  }

  void select_project_folder(const std::filesystem::path& folder) {
    if (!std::filesystem::is_directory(folder)) throw std::runtime_error("Select an existing project folder");
    const auto root = std::filesystem::canonical(folder);
    const auto path = root / "across.json";
    if (std::filesystem::exists(path)) {
      load_project(path);
      project_root.value = root.string();
    } else {
      project_root.value = root.string(); manifest.value.clear();
      app_id.value = across::slugify(root.filename().string());
      project_type.value = "static"; project_port.value = "3000";
      artifact.value = "dist"; install.value.clear(); build.value.clear();
      start.value = "npm start"; build_location = "vps"; deployment_dir.value = ".";
      domains.value.clear(); upstreams.value.clear();
      append_log("Folder selected. Save creates across.json in this folder.");
    }
    for (auto* value : management_fields()) value->active = false;
  }

  template <typename Function>
  void start_job(const std::string& label, Function function) {
    if (busy.exchange(true)) return;
    append_log("\n[" + label + "]");
    job = std::async(std::launch::async,
                     [this, label, function = std::move(function)]() mutable {
      try {
        const auto result = function([this](const std::string& chunk) {
          append_log(chunk);
        });
        if (result.ok()) append_log(label + " completed successfully.");
        else append_log(label + " failed with exit " + std::to_string(result.exit_code) +
                        ".");
      } catch (const std::exception& error) {
        append_log(label + " failed: " + error.what());
      }
      busy.store(false);
    });
  }

  void check_finished_jobs() {
    {
      std::scoped_lock lock(data_mutex);
      if (pending_kind == "all" && selected >= 0 && pending_data.value("server", "") == selected_server().id) {
        if (pending_data.contains("list")) websites = pending_data["list"]["apps"];
        if (pending_data.contains("metrics")) metrics = pending_data["metrics"];
        if (pending_data.contains("packages")) package_state = pending_data["packages"];
        if (pending_data.contains("website")) {
          const auto& website = pending_data["website"];
          domain_only = website.value("domain_only", false);
          app_id.value = pending_data.value("selected_app", "");
          const auto& config = website["manifest"]["nginx"];
          domains.value.clear(); for (const auto& domain : config["domains"]) domains.value += domain.get<std::string>() + " ";
          body_limit.value = std::to_string(config.value("client_max_body_mb", 20));
          rate.value = std::to_string(config.value("rate_limit_rps", 0));
          burst.value = std::to_string(config.value("rate_limit_burst", 20));
          nginx_editor.value = website["nginx"].get<std::string>(); nginx_editor.cursor = 0; nginx_editor.scroll = 0;
        }
      }
      if (pending_kind == "list") websites = pending_data.at("apps");
      if (pending_kind == "metrics") metrics = pending_data;
      if (pending_kind == "packages") package_state = pending_data;
      if (pending_kind == "nginx-access") domain_only = pending_data.value("domain_only", false);
      if (pending_kind == "website") {
        domain_only = pending_data.value("domain_only", false);
        const auto& config = pending_data.at("manifest").at("nginx");
        domains.value.clear(); for (const auto& domain : config.at("domains")) domains.value += domain.get<std::string>() + " ";
        body_limit.value = std::to_string(config.value("client_max_body_mb", 20));
        rate.value = std::to_string(config.value("rate_limit_rps", 0));
        burst.value = std::to_string(config.value("rate_limit_burst", 20));
        nginx_editor.value = pending_data.at("nginx").get<std::string>(); nginx_editor.cursor = 0; nginx_editor.scroll = 0;
      }
      pending_kind.clear();
    }
    if (job.valid() &&
        job.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
      job.get();
    }
    if (watcher_job.valid() &&
        watcher_job.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
      watcher_job.get();
      watcher_running = false;
    }
    if (refresh_requested && !busy.load()) refresh_all();
  }

  void start_watch() {
    if (watcher_running) {
      stop_watcher.store(true);
      append_log("Stopping file watcher...");
      return;
    }
    const auto server = selected_server();
    const auto project = across::load_manifest(manifest.value);
    stop_watcher.store(false);
    watcher_running = true;
    append_log("Watching " + project.project_root.string());
    watcher_job = std::async(std::launch::async, [this, server, project] {
      try {
        across::FileWatcher watcher(project.project_root, project.excludes);
        watcher.run(
            [this, server, project](const std::vector<across::FileChange>& changes) {
              append_log("Change detected. Deploying...");
              for (const auto& change : changes) {
                append_log("  " + across::change_kind_name(change.kind) + "  " +
                           change.path);
              }
              across::Deployer deployer(server);
              const auto result = deployer.deploy(project, [this](const std::string& chunk) {
                append_log(chunk);
              });
              append_log(result.ok() ? "Watched deployment completed."
                                     : "Watched deployment failed.");
            },
            stop_watcher);
      } catch (const std::exception& error) {
        append_log(std::string("Watcher failed: ") + error.what());
      }
    });
  }
};

void draw_wrapped_log(const std::string& value, Rectangle bounds) {
  DrawRectangleRounded(bounds, 0.035F, 8, kSurface);
  DrawRectangleRoundedLinesEx(bounds, 0.035F, 8, 1.0F, kBorder);
  draw_text("Activity", bounds.x + 16.0F, bounds.y + 12.0F, 13.0F, kText,
            FontWeight::semibold);
  const float font_size = 12.5F;
  const float line_height = 17.0F;
  const float available_width = bounds.width - 32.0F;
  std::vector<std::string> lines;
  std::istringstream stream(value);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty()) {
      lines.emplace_back();
      continue;
    }
    while (measure_text(line, font_size).x > available_width && line.size() > 1) {
      std::size_t cut = line.size() - 1;
      while (cut > 1 &&
             measure_text(line.substr(0, cut), font_size).x > available_width) {
        --cut;
      }
      lines.push_back(line.substr(0, cut));
      line.erase(0, cut);
    }
    lines.push_back(line);
  }
  const int visible =
      std::max(1, static_cast<int>((bounds.height - 46.0F) / line_height));
  const int start = std::max(0, static_cast<int>(lines.size()) - visible);
  float y = bounds.y + 38.0F;
  for (int index = start; index < static_cast<int>(lines.size()); ++index) {
    draw_text(lines[static_cast<std::size_t>(index)], bounds.x + 16.0F, y,
              font_size, kMuted);
    y += line_height;
  }
}

void draw_server_sidebar(Application& app, float height) {
  constexpr float width = 232.0F;
  DrawRectangleRec({0.0F, 0.0F, width, height}, kSidebar);
  DrawTexturePro(active_logo, {0, 0, static_cast<float>(active_logo.width), static_cast<float>(active_logo.height)},
                 {18, 17, 38, 38}, {0, 0}, 0, WHITE);
  draw_text("ACROSS", 66.0F, 24.0F, 20.0F, kText, FontWeight::semibold);

  DrawLineEx({16.0F, 72.0F}, {width - 16.0F, 72.0F}, 1.0F, kBorder);
  draw_text("SERVERS", 20.0F, 88.0F, 11.0F, kMuted, FontWeight::semibold);
  float y = 112.0F;
  for (int index = 0; index < static_cast<int>(app.servers.size()); ++index) {
    Rectangle row{12.0F, y, 208.0F, 56.0F};
    const bool selected = index == app.selected;
    const bool hovered = contains(row, GetMousePosition());
    if (selected || hovered) {
      DrawRectangleRounded(row, 0.12F, 6,
                           selected ? kSurfaceRaised : kSurface);
    }
    if (selected) {
      DrawRectangleRounded({row.x, row.y + 10.0F, 3.0F, row.height - 20.0F},
                           1.0F, 4, kAccent);
    }
    draw_text(app.servers[static_cast<std::size_t>(index)].label, 24.0F,
              y + 9.0F, 13.5F, selected ? kText : kMuted,
              selected ? FontWeight::semibold : FontWeight::regular);
    const auto endpoint = app.servers[static_cast<std::size_t>(index)].host;
    draw_text(endpoint, 24.0F, y + 32.0F, 11.5F, kFaint);
    if (hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      if (!app.busy.load() && !app.watcher_running) app.select(index);
    }
    y += 64.0F;
  }
  if (button({14.0F, height - 66.0F, 98.0F, 34.0F}, "New server",
             !app.busy.load())) {
    app.new_server();
  }
  const bool confirm =
      std::chrono::steady_clock::now() - app.profile_remove_confirmation <
      std::chrono::seconds{5};
  if (button({120.0F, height - 66.0F, 98.0F, 34.0F},
             confirm ? "Confirm" : "Remove",
             app.selected >= 0 && !app.busy.load(), ButtonStyle::danger)) {
    if (!confirm) {
      app.profile_remove_confirmation = std::chrono::steady_clock::now();
      app.append_log("Click Confirm within five seconds to remove this local profile.");
    } else {
      app.profile_remove_confirmation = {};
      try {
        app.remove_selected_server();
      } catch (const std::exception& error) {
        app.append_log(std::string("Profile removal failed: ") + error.what());
      }
    }
  }

}

std::string clipped_tail(std::string value, float width, float font_size,
                         FontWeight weight = FontWeight::regular) {
  bool clipped = false;
  while (value.size() > 3 && measure_text(value, font_size, weight).x > width) {
    value.erase(value.begin());
    clipped = true;
  }
  if (clipped && value.size() > 3) value.replace(0, 3, "...");
  return value;
}

constexpr float kBrowserRowHeight = 38.0F;

Rectangle browser_row_area(Rectangle pane) {
  return {pane.x + 10.0F, pane.y + 70.0F, pane.width - 20.0F,
          pane.height - 82.0F};
}

int browser_visible_rows(Rectangle pane) {
  return std::max(
      1, static_cast<int>(browser_row_area(pane).height / kBrowserRowHeight));
}

int browser_index_at(Rectangle pane, int scroll, int entry_count,
                     Vector2 position) {
  const Rectangle area = browser_row_area(pane);
  if (!contains(area, position)) return -1;
  const int row = static_cast<int>((position.y - area.y) / kBrowserRowHeight);
  const int index = scroll + row;
  return index >= 0 && index < entry_count ? index : -1;
}

void draw_entry_kind(float x, float y, bool directory) {
  const Color color = directory ? kAccent : kFaint;
  if (directory) {
    DrawRectangleRounded({x, y + 4.0F, 22.0F, 15.0F}, 0.18F, 4, color);
    DrawRectangleRounded({x + 2.0F, y + 1.0F, 9.0F, 6.0F}, 0.20F, 4, color);
  } else {
    DrawRectangleRoundedLinesEx({x + 3.0F, y + 1.0F, 16.0F, 20.0F}, 0.12F,
                                4, 1.0F, color);
    DrawLineEx({x + 7.0F, y + 8.0F}, {x + 15.0F, y + 8.0F}, 1.0F, color);
    DrawLineEx({x + 7.0F, y + 12.0F}, {x + 15.0F, y + 12.0F}, 1.0F, color);
  }
}

void begin_browser_drag(FileBrowser& browser, BrowserDragSource source, int index) {
  if (browser.transferring || browser.loading_remote) return;
  browser.drag_source = source;
  browser.drag_index = index;
  browser.drag_start = GetMousePosition();
  browser.dragging = false;
}

void handle_browser_click(FileBrowser& browser, BrowserDragSource source,
                          int index, bool directory) {
  if (source == BrowserDragSource::local) browser.local_selected = index;
  else browser.remote_selected = index;

  const double now = GetTime();
  const bool double_click = browser.last_click_source == source &&
                            browser.last_click_index == index &&
                            now - browser.last_click_time < 0.36;
  browser.last_click_source = source;
  browser.last_click_index = index;
  browser.last_click_time = now;
  if (double_click && directory) {
    browser.drag_source = BrowserDragSource::none;
    if (source == BrowserDragSource::local) browser.enter_local(index);
    else browser.enter_remote(index);
  } else {
    begin_browser_drag(browser, source, index);
  }
}

void draw_local_pane(FileBrowser& browser, Rectangle pane) {
  DrawRectangleRounded(pane, 0.035F, 8, kSurface);
  DrawRectangleRoundedLinesEx(pane, 0.035F, 8, 1.0F, kBorder);
  draw_text("LOCAL", pane.x + 16.0F, pane.y + 13.0F, 11.0F, kMuted,
            FontWeight::semibold);
  if (button({pane.x + pane.width - 66.0F, pane.y + 9.0F, 52.0F, 29.0F},
             "Up", browser.local_directory != browser.local_directory.root_path() &&
                       !browser.transferring,
             ButtonStyle::ghost)) {
    browser.local_up();
  }
  draw_text(clipped_tail(browser.local_directory.string(), pane.width - 108.0F,
                         12.0F),
            pane.x + 16.0F, pane.y + 41.0F, 12.0F, kFaint);
  DrawLineEx({pane.x + 12.0F, pane.y + 62.0F},
             {pane.x + pane.width - 12.0F, pane.y + 62.0F}, 1.0F, kBorder);

  const Rectangle area = browser_row_area(pane);
  const int visible = browser_visible_rows(pane);
  const int maximum_scroll =
      std::max(0, static_cast<int>(browser.local_entries.size()) - visible);
  if (contains(area, GetMousePosition())) {
    const float wheel = GetMouseWheelMove();
    if (wheel > 0.0F) --browser.local_scroll;
    else if (wheel < 0.0F) ++browser.local_scroll;
  }
  browser.local_scroll = std::clamp(browser.local_scroll, 0, maximum_scroll);

  BeginScissorMode(static_cast<int>(area.x), static_cast<int>(area.y),
                   static_cast<int>(area.width), static_cast<int>(area.height));
  for (int row = 0; row < visible; ++row) {
    const int index = browser.local_scroll + row;
    if (index >= static_cast<int>(browser.local_entries.size())) break;
    const auto& entry = browser.local_entries[static_cast<std::size_t>(index)];
    Rectangle row_bounds{area.x, area.y + static_cast<float>(row) * kBrowserRowHeight,
                         area.width, kBrowserRowHeight - 2.0F};
    const bool selected = index == browser.local_selected;
    const bool hovered = contains(row_bounds, GetMousePosition());
    if (selected || hovered) {
      DrawRectangleRounded(row_bounds, 0.10F, 5,
                           selected ? kSurfaceRaised : Color{18, 27, 39, 255});
    }
    draw_entry_kind(row_bounds.x + 10.0F, row_bounds.y + 7.0F,
                    entry.directory);
    draw_text(clipped_tail(entry.name, row_bounds.width - 58.0F, 13.0F),
              row_bounds.x + 44.0F, row_bounds.y + 9.0F, 13.0F,
              selected ? kText : kMuted,
              selected ? FontWeight::semibold : FontWeight::regular);
    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      handle_browser_click(browser, BrowserDragSource::local, index,
                           entry.directory);
      if (browser.local_selected == -1) break;
    }
  }
  EndScissorMode();
}

void draw_remote_pane(FileBrowser& browser, Rectangle pane) {
  DrawRectangleRounded(pane, 0.035F, 8, kSurface);
  DrawRectangleRoundedLinesEx(pane, 0.035F, 8, 1.0F, kBorder);
  draw_text("SERVER", pane.x + 16.0F, pane.y + 13.0F, 11.0F, kMuted,
            FontWeight::semibold);
  if (button({pane.x + pane.width - 66.0F, pane.y + 9.0F, 52.0F, 29.0F},
             "Up", browser.remote_directory != "/" &&
                       !browser.transferring && !browser.loading_remote,
             ButtonStyle::ghost)) {
    browser.remote_up();
  }
  draw_text(clipped_tail(browser.remote_directory, pane.width - 108.0F, 12.0F),
            pane.x + 16.0F, pane.y + 41.0F, 12.0F, kFaint);
  DrawLineEx({pane.x + 12.0F, pane.y + 62.0F},
             {pane.x + pane.width - 12.0F, pane.y + 62.0F}, 1.0F, kBorder);

  const Rectangle area = browser_row_area(pane);
  const int visible = browser_visible_rows(pane);
  const int maximum_scroll =
      std::max(0, static_cast<int>(browser.remote_entries.size()) - visible);
  if (contains(area, GetMousePosition())) {
    const float wheel = GetMouseWheelMove();
    if (wheel > 0.0F) --browser.remote_scroll;
    else if (wheel < 0.0F) ++browser.remote_scroll;
  }
  browser.remote_scroll = std::clamp(browser.remote_scroll, 0, maximum_scroll);

  BeginScissorMode(static_cast<int>(area.x), static_cast<int>(area.y),
                   static_cast<int>(area.width), static_cast<int>(area.height));
  for (int row = 0; row < visible; ++row) {
    const int index = browser.remote_scroll + row;
    if (index >= static_cast<int>(browser.remote_entries.size())) break;
    const auto& entry = browser.remote_entries[static_cast<std::size_t>(index)];
    Rectangle row_bounds{area.x, area.y + static_cast<float>(row) * kBrowserRowHeight,
                         area.width, kBrowserRowHeight - 2.0F};
    const bool selected = index == browser.remote_selected;
    const bool hovered = contains(row_bounds, GetMousePosition());
    if (selected || hovered) {
      DrawRectangleRounded(row_bounds, 0.10F, 5,
                           selected ? kSurfaceRaised : Color{18, 27, 39, 255});
    }
    draw_entry_kind(row_bounds.x + 10.0F, row_bounds.y + 7.0F,
                    entry.directory);
    draw_text(clipped_tail(entry.name, row_bounds.width - 58.0F, 13.0F),
              row_bounds.x + 44.0F, row_bounds.y + 9.0F, 13.0F,
              selected ? kText : kMuted,
              selected ? FontWeight::semibold : FontWeight::regular);
    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      handle_browser_click(browser, BrowserDragSource::remote, index,
                           entry.directory);
      if (browser.remote_selected == -1) break;
    }
  }
  EndScissorMode();

  if (browser.loading_remote) {
    const std::string loading = "Loading server folder...";
    const float width = measure_text(loading, 13.0F).x;
    draw_text(loading, pane.x + (pane.width - width) / 2.0F,
              pane.y + pane.height / 2.0F, 13.0F, kMuted);
  }
}

std::string remote_drop_directory(FileBrowser& browser, Rectangle remote_pane,
                                  Vector2 position) {
  const int index = browser_index_at(
      remote_pane, browser.remote_scroll,
      static_cast<int>(browser.remote_entries.size()), position);
  if (index >= 0 &&
      browser.remote_entries[static_cast<std::size_t>(index)].directory) {
    return remote_existing_child(
        browser.remote_directory,
        browser.remote_entries[static_cast<std::size_t>(index)].name);
  }
  return browser.remote_directory;
}

std::filesystem::path local_drop_directory(FileBrowser& browser,
                                           Rectangle local_pane,
                                           Vector2 position) {
  const int index = browser_index_at(
      local_pane, browser.local_scroll,
      static_cast<int>(browser.local_entries.size()), position);
  if (index >= 0 &&
      browser.local_entries[static_cast<std::size_t>(index)].directory) {
    return browser.local_entries[static_cast<std::size_t>(index)].path;
  }
  return browser.local_directory;
}

void update_browser_drag(FileBrowser& browser, Rectangle local_pane,
                         Rectangle remote_pane) {
  if (browser.drag_source == BrowserDragSource::none) return;
  const Vector2 position = GetMousePosition();
  if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
    const float dx = position.x - browser.drag_start.x;
    const float dy = position.y - browser.drag_start.y;
    if (dx * dx + dy * dy > 36.0F) browser.dragging = true;
  }
  if (!IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return;

  try {
    if (browser.dragging && browser.drag_source == BrowserDragSource::local &&
        contains(remote_pane, position) && browser.drag_index >= 0 &&
        browser.drag_index < static_cast<int>(browser.local_entries.size())) {
      const auto target = remote_drop_directory(browser, remote_pane, position);
      browser.start_upload(
          {browser.local_entries[static_cast<std::size_t>(browser.drag_index)].path},
          target);
    } else if (browser.dragging &&
               browser.drag_source == BrowserDragSource::remote &&
               contains(local_pane, position)) {
      browser.start_download(browser.drag_index,
                             local_drop_directory(browser, local_pane, position));
    }
  } catch (const std::exception& error) {
    browser.status = error.what();
  }
  browser.drag_source = BrowserDragSource::none;
  browser.drag_index = -1;
  browser.dragging = false;
}

void draw_browser_drag(const FileBrowser& browser) {
  if (!browser.dragging || browser.drag_source == BrowserDragSource::none) return;
  std::string name;
  if (browser.drag_source == BrowserDragSource::local && browser.drag_index >= 0 &&
      browser.drag_index < static_cast<int>(browser.local_entries.size())) {
    name = browser.local_entries[static_cast<std::size_t>(browser.drag_index)].name;
  } else if (browser.drag_source == BrowserDragSource::remote &&
             browser.drag_index >= 0 &&
             browser.drag_index < static_cast<int>(browser.remote_entries.size())) {
    name = browser.remote_entries[static_cast<std::size_t>(browser.drag_index)].name;
  }
  const Vector2 position = GetMousePosition();
  const std::string label = clipped_tail(name, 190.0F, 12.5F);
  const float width = std::min(220.0F, measure_text(label, 12.5F).x + 32.0F);
  DrawRectangleRounded({position.x + 14.0F, position.y + 12.0F, width, 34.0F},
                       0.18F, 6, kSurfaceRaised);
  DrawRectangleRoundedLinesEx(
      {position.x + 14.0F, position.y + 12.0F, width, 34.0F}, 0.18F, 6, 1.0F,
      kAccent);
  draw_text(label, position.x + 28.0F, position.y + 21.0F, 12.5F, kText,
            FontWeight::semibold);
}

void handle_external_file_drop(FileBrowser& browser, Rectangle remote_pane) {
  if (!IsFileDropped()) return;
  const FilePathList dropped = LoadDroppedFiles();
  try {
    if (!contains(remote_pane, GetMousePosition())) {
      browser.status = "Drop local files or folders onto the server pane";
    } else {
      std::vector<std::filesystem::path> items;
      for (unsigned int index = 0; index < dropped.count; ++index) {
        const std::filesystem::path path{dropped.paths[index]};
        if (is_shareable_path(path)) items.push_back(path);
      }
      browser.start_upload(
          std::move(items),
          remote_drop_directory(browser, remote_pane, GetMousePosition()));
    }
  } catch (const std::exception& error) {
    browser.status = error.what();
  }
  UnloadDroppedFiles(dropped);
}

void draw_file_browser(FileBrowser& browser, Rectangle bounds) {
    browser.poll_jobs();
    const float width = bounds.width;
    const float height = bounds.height;
    const float right = bounds.x + width;
    const float bottom = bounds.y + height;
    const float margin = bounds.x + 22.0F;
    constexpr float gap = 16.0F;
    const float pane_y = bounds.y + 88.0F;
    const float pane_height = std::max(350.0F, height - 234.0F);
    const float pane_width = (width - 44.0F - gap) / 2.0F;
    const Rectangle local_pane{margin, pane_y, pane_width, pane_height};
    const Rectangle remote_pane{margin + pane_width + gap, pane_y, pane_width,
                                pane_height};

    if (!browser.editing && !browser.loading_editor) handle_external_file_drop(browser, remote_pane);

    draw_text("File browser", margin, bounds.y + 17.0F, 25.0F, kText,
              FontWeight::semibold);

    const std::string server_label = browser.server.label + "  -  " + browser.server.host;
    const float server_label_width = measure_text(server_label, 12.5F).x;
    draw_text(server_label, right - 22 - server_label_width, bounds.y + 24.0F, 12.5F,
              kMuted, FontWeight::semibold);

    if (browser.editing) {
      draw_text(clipped_tail(browser.editing_path, width - 44, 13), margin, bounds.y + 67, 13, kMuted);
      browser.editor.bounds = {margin, bounds.y + 100, width - 44, height - 210};
      if (!browser.transferring) browser.editor.update();
      browser.editor.draw();
      try {
        if (button({margin, bottom - 86, 140, 38}, "Save file", !browser.transferring, ButtonStyle::primary)) browser.save_editor();
        if (button({margin + 152, bottom - 86, 160, 38}, browser.editor.value == browser.original_text ? "Back to files" : "Discard edits", !browser.transferring)) {
          browser.editing = false; browser.editor.active = false;
        }
      } catch (const std::exception& error) { browser.status = error.what(); }
      draw_text(clipped_tail(browser.status, width - 380, 12), margin + 340, bottom - 74, 12, kMuted);
      return;
    }

    draw_local_pane(browser, local_pane);
    draw_remote_pane(browser, remote_pane);
    update_browser_drag(browser, local_pane, remote_pane);

    browser.new_name.bounds = {margin, bottom - 125, width - 594, 36}; browser.new_name.update(); browser.new_name.draw();
    try {
      const bool can_edit = !browser.transferring && !browser.loading_remote && !browser.loading_editor;
      if (button({right - 556, bottom - 125, 120, 36}, "Local folder", can_edit)) browser.mutate_local("mkdir");
      if (button({right - 428, bottom - 125, 120, 36}, "VPS folder", can_edit)) browser.mutate_remote("mkdir");
      const bool local_action = browser.last_click_source == BrowserDragSource::local;
      const bool has_selection = local_action ? browser.local_selected >= 0 : browser.remote_selected >= 0;
      if (button({right - 298, bottom - 125, 120, 36}, local_action ? "Rename local" : "Rename VPS", can_edit && has_selection)) {
        if (local_action) browser.mutate_local("rename"); else browser.mutate_remote("rename");
      }
      const bool confirm = GetTime() - browser.trash_confirmation < 5;
      if (button({right - 168, bottom - 125, 146, 36}, confirm ? "Confirm archive" : local_action ? "Archive local" : "Archive VPS", can_edit && has_selection, ButtonStyle::danger)) {
        if (confirm) { if (local_action) browser.mutate_local("trash"); else browser.mutate_remote("trash"); browser.trash_confirmation = -10; }
        else { browser.trash_confirmation = GetTime(); browser.status = "Click Confirm archive to keep this item as a recoverable .across-trash entry."; }
      }
    } catch (const std::exception& error) { browser.status = error.what(); }
    const float actions_y = bottom - 58.0F;
    const bool can_transfer = !browser.transferring && !browser.loading_remote && !browser.loading_editor;
    try {
      if (button({margin, bounds.y + 48, 136, 30}, "Edit local file", can_transfer && browser.local_selected >= 0, ButtonStyle::ghost)) browser.open_editor(false);
      if (button({margin + 148, bounds.y + 48, 142, 30}, "Edit VPS file", can_transfer && browser.remote_selected >= 0, ButtonStyle::ghost)) browser.open_editor(true);
    } catch (const std::exception& error) { browser.status = error.what(); }
    if (button({margin, actions_y, 142.0F, 36.0F}, "Upload selected",
               can_transfer && browser.local_selected >= 0,
               ButtonStyle::primary)) {
      browser.upload_selected();
    }
    if (button({margin + 152.0F, actions_y, 152.0F, 36.0F},
               "Download selected",
               can_transfer && browser.remote_selected >= 0)) {
      browser.download_selected();
    }
    if (button({margin + 314.0F, actions_y, 86.0F, 36.0F}, "Refresh",
               !browser.transferring && !browser.loading_remote,
               ButtonStyle::ghost)) {
      browser.refresh_local();
      browser.refresh_remote();
    }
    const bool noteworthy_status =
        browser.transferring || browser.loading_remote ||
        (browser.status != "Ready" && browser.status != "Server folder loaded");
    const std::string footer_text = noteworthy_status
        ? browser.status
        : "";
    const Color footer_color = browser.status.find("failed") != std::string::npos
        ? kDanger
        : (browser.transferring || browser.loading_remote ? kAccent : kFaint);
    draw_text(clipped_tail(footer_text, width - 450.0F, 12.0F),
              margin + 424.0F, actions_y + 10.0F, 12.0F,
              footer_color);
    draw_browser_drag(browser);
}

void run_file_browser(const across::ServerProfile& server,
                      const std::filesystem::path& initial_local_directory,
                      const std::string& initial_remote_directory) {
  FileBrowser browser{server, initial_local_directory, initial_remote_directory};
  while (!WindowShouldClose()) {
    field_help.clear(); BeginDrawing(); ClearBackground(kBackground);
    draw_file_browser(browser, {0, 0, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())});
    draw_field_help(); EndDrawing();
  }
}

void draw_management(Application& app, float x, float width) {
  const bool enabled = app.selected >= 0 && !app.busy.load();
  const float half = (width - 32.0F) / 2;
  auto field = [](TextField& value, float left, float top, float span) {
    value.bounds = {left, top, span, 38}; value.update(); value.draw();
  };
  auto operation = [&](const std::string& label, std::vector<std::string> arguments) {
    arguments.push_back("--json");
    const auto server = app.selected_server();
    app.start_job(label, [server, arguments](const auto& callback) { return across::Deployer(server).server_command(arguments, callback); });
  };
  auto refresh = [&](const std::string& command) {
    const auto server = app.selected_server();
    const auto id = app.app_id.value;
    app.start_job("Refresh " + command, [&app, server, command, id](const auto&) {
      std::vector<std::string> args{command, "--json"};
      if (command == "website") args.insert(args.end(), {"--app", id});
      if (command == "packages") {
        auto names = server.packages;
        for (const auto& name : server.custom_packages) if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        args.insert(args.end(), {"--packages", nlohmann::json(names).dump()});
      }
      auto result = across::Deployer(server).server_command(args);
      if (result.ok()) {
        const auto data = nlohmann::json::parse(result.output);
        std::scoped_lock lock(app.data_mutex);
        app.pending_data = data;
        app.pending_kind = command;
      } else app.append_log(result.output);
      return result;
    });
  };
  const std::vector<std::string> tabs{"Projects", "Websites", "Domains", "Pipelines", "Resources", "Firewall", "Connection", "Packages", "Files"};
  float tab_x = x;
  for (int index = 0; index < static_cast<int>(tabs.size()); ++index) {
    const float span = measure_text(tabs[static_cast<std::size_t>(index)], 12).x + 22;
    const Rectangle tab{tab_x, 84, span, 38};
    const int target_page = index >= 7 ? index + 1 : index;
    const bool active = app.page == target_page || (app.page == 7 && index == 1);
    const bool hovered = contains(tab, GetMousePosition());
    draw_text(tabs[static_cast<std::size_t>(index)], tab_x + 11, 94, 13,
              active ? kText : hovered ? kText : kMuted,
              active ? FontWeight::semibold : FontWeight::regular);
    if (active) DrawLineEx({tab_x + 11, 129}, {tab_x + span - 11, 129}, 2, kAccent);
    if (hovered && !app.busy.load() && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      app.page = target_page;
      app.save_desktop_state();
      app.nginx_editor.active = false;
      for (auto* value : app.management_fields()) value->active = false;
    }
    tab_x += span + 16;
  }
  DrawLineEx({x, 130}, {x + width, 130}, 1, kBorder);
  try {
  if (app.page == 0) {
    field(app.manifest, x, 185, width - 280);
    if (button({x + width - 268, 185, 128, 38}, "Recent projects", true)) app.show_recent_projects = !app.show_recent_projects;
    if (button({x + width - 128, 185, 128, 38}, "Load", true)) {
      app.load_project(app.manifest.value);
    }
    if (app.show_recent_projects) {
      const auto paths = app.recent_projects();
      draw_text("Saved projects for this server", x, 266, 15, kText);
      if (paths.empty()) draw_text("Select a folder and save your first project.", x, 305, 13, kMuted);
      for (std::size_t index = 0; index < paths.size() && index < 7; ++index) {
        if (button({x, 307 + static_cast<float>(index) * 44, width, 36}, paths[index], true, ButtonStyle::ghost)) {
          app.load_project(paths[index]); app.show_recent_projects = false;
        }
      }
      return;
    }
    field(app.project_root, x, 265, half - 142);
    if (button({x + half - 130, 265, 130, 38}, "Select folder", true)) {
      const auto folder = choose_project_folder(app.project_root.value);
      if (!folder.empty()) app.select_project_folder(folder);
    }
    field(app.app_id, x + half + 32, 265, half);
    draw_text("Save creates across.json in the selected folder.", x, 312, 11, kFaint);
    field(app.domains, x, 345, width);
    const float type_width = half;
    draw_text("Type", x, 407, 12, kMuted, FontWeight::semibold);
    int type_index = 0;
    for (const auto& type : {"static", "node", "proxy"}) {
      if (button({x + static_cast<float>(type_index++) * (type_width / 3), 425, type_width / 3 - 4, 38}, type, true,
                 app.project_type.value == type ? ButtonStyle::secondary : ButtonStyle::ghost)) {
        app.project_type.value = type;
        app.upstreams.active = false;
        if (app.project_type.value != "proxy") app.upstreams.value.clear();
      }
    }
    if (app.project_type.value == "proxy") field(app.upstreams, x + half + 32, 425, half);
    else if (app.project_type.value == "node") field(app.project_port, x + half + 32, 425, half);
    else field(app.artifact, x + half + 32, 425, half);
    if (button({x, 488, 140, 32}, app.show_advanced ? "Hide commands" : "Build & start", true, ButtonStyle::ghost)) {
      app.show_advanced = !app.show_advanced;
      for (auto* value : {&app.install, &app.build, &app.start, &app.upstreams}) value->active = false;
    }
    if (app.project_type.value != "proxy") {
      draw_text("Build on", x + half + 32, 478, 12, kMuted, FontWeight::semibold);
      if (button({x + half + 32, 496, half / 2 - 4, 32}, "Local machine", enabled,
                 app.build_location == "local" ? ButtonStyle::secondary : ButtonStyle::ghost)) app.build_location = "local";
      if (button({x + half + 32 + half / 2, 496, half / 2 - 4, 32}, "VPS", enabled,
                 app.build_location == "vps" ? ButtonStyle::secondary : ButtonStyle::ghost)) app.build_location = "vps";
    }
    if (app.show_advanced) {
      field(app.install, x, 552, half); field(app.build, x + half + 32, 552, half);
      field(app.start, x, 626, half);
      field(app.deployment_dir, x + half + 32, 626, half);
    }
    const float actions_y = app.show_advanced ? 690.0F : 556.0F;
    if (button({x, actions_y, 136, 38}, "Save", true)) {
      if (app.project_root.value.empty() || !std::filesystem::is_directory(app.project_root.value)) throw std::runtime_error("Select a project folder first");
      across::ProjectManifest project;
      project.app_id = app.app_id.value; project.name = project.app_id; project.project_root = std::filesystem::absolute(app.project_root.value);
      project.type = app.project_type.value; project.port = std::stoi(app.project_port.value);
      project.artifact_dir = app.artifact.value; project.install_command = app.install.value;
      project.build_command = app.build.value; project.start_command = app.start.value;
      project.build_location = app.build_location; project.deployment_dir = app.deployment_dir.value;
      project.nginx.domains = across::parse_domains(app.domains.value);
      std::istringstream stream(app.upstreams.value); std::string upstream; while (stream >> upstream) project.nginx.upstreams.push_back(upstream);
      across::validate_manifest(project);
      const auto path = project.project_root / "across.json";
      if (std::filesystem::exists(path) && app.manifest.value != path.string()) throw std::runtime_error("Load the existing manifest before saving changes");
      // Keep deployment policies and certificate settings when editing a project.
      if (std::filesystem::exists(path)) {
        const auto previous = across::load_manifest(path);
        if (previous.app_id != project.app_id) throw std::runtime_error("Use a separate project folder for a different app id");
        project.excludes = previous.excludes; project.keep_releases = previous.keep_releases;
        project.health_path = previous.health_path; project.health_timeout_seconds = previous.health_timeout_seconds;
        const auto domains = project.nginx.domains; const auto upstreams = project.nginx.upstreams;
        project.nginx = previous.nginx; project.nginx.domains = domains; project.nginx.upstreams = upstreams;
      }
      across::validate_manifest(project);
      std::ofstream output(path); output << nlohmann::json(project).dump(2) << '\n'; output.close();
      if (!output) throw std::runtime_error("Cannot save project manifest");
      app.manifest.value = path.string(); app.append_log("Saved " + path.string());
      app.remember_project();
    }
    if (button({x + 148, actions_y, 136, 38}, "Deploy", enabled, ButtonStyle::primary)) {
      const auto server = app.selected_server(); const auto project = across::load_manifest(app.manifest.value);
      app.start_job("Deployment", [server, project](const auto& callback) { return across::Deployer(server).deploy(project, callback); });
    }
    if (button({x + 296, actions_y, 128, 38}, app.watcher_running ? "Stop watching" : "Watch", enabled || app.watcher_running)) app.start_watch();
    if (button({x + 436, actions_y, 130, 38}, "Files", enabled)) { app.page = 9; app.save_desktop_state(); }
  } else if (app.page == 9) {
    if (app.selected < 0) { draw_text("Select a server to browse files.", x, 185, 15, kMuted); return; }
    if (!app.file_browser) app.file_browser = std::make_unique<FileBrowser>(app.selected_server(),
        app.project_root.value.empty() ? std::filesystem::current_path() : std::filesystem::path{app.project_root.value}, "/tmp");
    draw_file_browser(*app.file_browser, {x, 145, width, static_cast<float>(GetScreenHeight()) - 215});
  } else if (app.page == 1) {
    if (button({x + width - 150, 147, 150, 38}, "Refresh", enabled)) refresh("list");
    draw_text("APPLICATION", x, 207, 11, kFaint); draw_text("TYPE", x + 210, 207, 11, kFaint); draw_text("DOMAINS", x + 305, 207, 11, kFaint);
    if (contains({x, 230, width, 130}, GetMousePosition())) app.website_scroll = std::clamp(app.website_scroll - static_cast<int>(GetMouseWheelMove()), 0, std::max(0, static_cast<int>(app.websites.size()) - 3));
    int row = 0;
    int site_index = 0;
    for (const auto& site : app.websites) {
      if (site_index++ < app.website_scroll) continue;
      if (row >= 3) break;
      const float y = 235 + static_cast<float>(row) * 42;
      const auto id = site.value("app_id", "");
      if (button({x, y, 195, 32}, id, enabled, app.app_id.value == id ? ButtonStyle::primary : ButtonStyle::ghost)) { app.app_id.value = id; refresh("website"); }
      draw_text(site.value("type", "unknown"), x + 210, y + 8, 12, kMuted);
      std::string domains; for (const auto& domain : site.value("domains", std::vector<std::string>{})) domains += domain + " ";
      draw_text(clipped_tail(domains, width - 310, 12), x + 305, y + 8, 12, kMuted); ++row;
    }
    if (app.websites.size() > 3) draw_text(std::to_string(app.websites.size()) + " websites", x, 364, 11, kFaint);
    if (app.websites.empty()) draw_text("No websites loaded", x, 243, 13, kMuted);
    field(app.app_id, x, 390, 180);
    if (button({x + 192, 390, 128, 38}, "Load", enabled)) refresh("website");
    if (button({x + 332, 390, 112, 38}, "Rollback", enabled)) operation("Rollback", {"rollback", "--app", app.app_id.value});
    const bool confirm = std::chrono::steady_clock::now() - app.remove_confirmation < std::chrono::seconds{5};
    if (button({x + 456, 390, 128, 38}, confirm ? "Confirm delete" : "Delete", enabled, ButtonStyle::danger)) {
      if (confirm) { operation("Remove website", {"remove", "--app", app.app_id.value}); app.remove_confirmation = {}; }
      else { app.remove_confirmation = std::chrono::steady_clock::now(); app.append_log("Click Confirm delete to remove this website and its releases."); }
    }
    field(app.domains, x, 456, width);
    field(app.body_limit, x, 522, (width - 40) / 3); field(app.rate, x + (width - 40) / 3 + 20, 522, (width - 40) / 3); field(app.burst, x + 2 * ((width - 40) / 3 + 20), 522, (width - 40) / 3);
    if (button({x, 581, 166, 38}, "Save settings", enabled, ButtonStyle::primary)) operation("Website settings", {"website-settings", "--app", app.app_id.value, "--domains", app.domains.value, "--body-mb", app.body_limit.value, "--rate", app.rate.value, "--burst", app.burst.value});
    if (button({x + 178, 581, 166, 38}, "Edit Nginx", enabled)) { app.page = 7; }
  } else if (app.page == 7) {
    field(app.app_id, x, 224, 180);
    if (button({x + 192, 224, 140, 38}, "Load", enabled)) refresh("website");
    draw_text("Load an existing website, then edit its Nginx server block.", x, 274, 12, kMuted);
    app.nginx_editor.bounds = {x, 303, width, 280}; app.nginx_editor.update(); app.nginx_editor.draw();
    if (button({x, 607, 172, 38}, "Save & reload", enabled, ButtonStyle::primary)) operation("Save Nginx config", {"nginx-save", "--app", app.app_id.value, "--content", app.nginx_editor.value});
  } else if (app.page == 2) {
    if (button({x, 147, 204, 38}, app.domain_only ? "Restore IP access" : "Block IP access", enabled)) {
      const auto server = app.selected_server(); const auto mode = app.domain_only ? "restore" : "domain-only";
      app.start_job("Server access", [&app, server, mode](const auto& callback) {
        auto result = across::Deployer(server).server_command({"nginx-access", "--mode", mode, "--json"}, callback);
        if (result.ok()) { std::scoped_lock lock(app.data_mutex); const auto json_start = result.output.find('{');
          if (json_start == std::string::npos) throw std::runtime_error("Server returned no access status");
          app.pending_data = nlohmann::json::parse(result.output.substr(json_start)); app.pending_kind = "nginx-access"; }
        return result;
      });
    }
    draw_text(app.domain_only ? "Domain-only access enabled for this server" : "Block IP and unknown hostnames for this server", x + 218, 159, 12, kMuted);
    field(app.app_id, x, 225, 180); field(app.domains, x + 200, 225, width - 200);
    if (button({x, 280, 152, 38}, "Save domains", enabled, ButtonStyle::primary)) operation("Update domains", {"website-settings", "--app", app.app_id.value, "--domains", app.domains.value});
    field(app.email, x, 354, width - 310);
    if (button({x + width - 296, 354, 140, 38}, "Add HTTPS", enabled, ButtonStyle::primary)) operation("Issue HTTPS", {"tls-issue", "--app", app.app_id.value, "--email", app.email.value});
    if (button({x + width - 144, 354, 144, 38}, "Renew", enabled)) operation("Renew", {"tls-renew", "--app", app.app_id.value});
    draw_text("Uses Let's Encrypt. Port 80 must be reachable.", x, 404, 12, kMuted);
    field(app.dns_domain, x, 462, half); field(app.dns_address, x + half + 32, 462, half);
    field(app.dns_zone, x, 528, half); field(app.dns_token_file, x + half + 32, 528, half);
    if (button({x, 588, 188, 38}, "Point DNS", !app.busy.load(), ButtonStyle::primary)) {
      const auto zone = app.dns_zone.value, domain = app.dns_domain.value, address = app.dns_address.value, token = app.dns_token_file.value;
      app.start_job("Point DNS", [zone, domain, address, token](const auto& callback) { const auto result = across::point_cloudflare_domain(zone, domain, address, token); callback(result.output); return result; });
    }
  } else if (app.page == 3) {
    field(app.project_root, x, 235, width); field(app.branch, x, 310, half); field(app.pipeline_manifest, x + half + 32, 310, half);
    if (button({x, 373, 174, 38}, "Create pipeline", !app.busy.load(), ButtonStyle::primary)) {
      const auto path = across::create_pipeline(app.project_root.value, app.branch.value, app.pipeline_manifest.value);
      app.append_log("Created " + path.string() + ". Commit .github/workflows and .across/client, then set repository secrets and enable the production environment.");
    }
    if (button({x, 451, 176, 34}, app.show_secrets ? "Hide required secrets" : "Required secrets", true, ButtonStyle::ghost)) app.show_secrets = !app.show_secrets;
    if (app.show_secrets) {
      const std::vector<std::pair<std::string, std::string>> secrets{{"ACROSS_HOST", "Host"}, {"ACROSS_USER / ACROSS_PORT", "SSH user / port"}, {"ACROSS_SSH_KEY", "Deployment key"}, {"ACROSS_KNOWN_HOSTS", "Verified host key"}};
      float y = 510;
      for (const auto& [name, description] : secrets) { draw_text(name, x, y, 12, kText); draw_text(description, x + 340, y, 12, kMuted); y += 34; }
    }
  } else if (app.page == 4) {
    if (button({x + width - 150, 147, 150, 38}, "Refresh", enabled)) refresh("metrics");
    auto gib = [](std::uint64_t bytes) { std::ostringstream text; text.precision(2); text << std::fixed << static_cast<double>(bytes) / (1024 * 1024 * 1024) << " GiB"; return text.str(); };
    auto memory = app.metrics.value("memory_total_bytes", std::uint64_t{0});
    auto available = app.metrics.value("memory_available_bytes", std::uint64_t{0});
    auto disk = app.metrics.value("disk_total_bytes", std::uint64_t{0});
    auto free = app.metrics.value("disk_free_bytes", std::uint64_t{0});
    const auto states = app.metrics.value("service_states", nlohmann::json::object());
    std::string load = app.metrics.value("loadavg", std::string{});
    std::istringstream averages(load); std::string one, five, fifteen; averages >> one >> five >> fifteen;
    const std::vector<std::pair<std::string, std::string>> readings{
      {"CPU cores", app.metrics.empty() ? "Refresh to load" : std::to_string(app.metrics.value("cpu_cores", 0))},
      {"Load average (1 / 5 / 15 min)", load.empty() ? "Unavailable" : one + " / " + five + " / " + fifteen},
      {"Uptime", app.metrics.contains("uptime_seconds") ? std::to_string(static_cast<int>(app.metrics.at("uptime_seconds").get<double>() / 3600)) + " hours" : "Unavailable"},
      {"Memory used / total", memory > 0 ? gib(memory - std::min(memory, available)) + " / " + gib(memory) : "Unavailable"},
      {"Disk used / total", disk > 0 ? gib(disk - std::min(disk, free)) + " / " + gib(disk) : "Unavailable"},
      {"Nginx", states.value("nginx.service", "Unchecked")},
      {"Managed application service", states.value("across-pm2.service", "Unchecked")}};
    float y = 215;
    for (const auto& [label, value] : readings) {
      draw_text(label, x, y, 13, kMuted); draw_text(value, x + 340, y, 13, kText, FontWeight::semibold);
      DrawLineEx({x, y + 27}, {x + width, y + 27}, 1, kBorder); y += 39;
    }
    field(app.resource_service, x, 530, half); field(app.cpu_limit, x + half + 32, 530, (half - 20) / 2); field(app.memory_limit, x + half + 40 + (half - 20) / 2, 530, (half - 20) / 2);
    if (button({x, 594, 188, 38}, "Apply limits", enabled, ButtonStyle::primary)) operation("Service limits", {"resource-limits", "--service", app.resource_service.value, "--cpu", app.cpu_limit.value, "--memory-mb", app.memory_limit.value});
  } else if (app.page == 5) {
    if (button({x, 230, 166, 38}, "Refresh", enabled)) operation("Firewall status", {"firewall", "--action", "status"});
    if (button({x + 178, 230, 150, 38}, "Install UFW", enabled)) operation("Install firewall", {"firewall", "--action", "install"});
    if (button({x + 340, 230, 184, 38}, "Enable firewall", enabled)) operation("Enable firewall", {"firewall", "--action", "enable", "--ssh-port", std::to_string(app.selected_server().port)});
    field(app.firewall_port, x, 340, half); field(app.firewall_protocol, x + half + 32, 340, half);
    for (int index = 0; index < 3; ++index) {
      const std::string action = index == 0 ? "allow" : index == 1 ? "deny" : "delete";
      if (button({x + static_cast<float>(index) * 170, 408, 158, 38}, index == 0 ? "Allow" : index == 1 ? "Deny" : "Remove allow rule", enabled, index == 0 ? ButtonStyle::primary : ButtonStyle::danger)) operation("Firewall " + action, {"firewall", "--action", action, "--port", app.firewall_port.value, "--protocol", app.firewall_protocol.value, "--ssh-port", std::to_string(app.selected_server().port)});
    }
  } else if (app.page == 8) {
    const auto os = app.package_state.value("os", nlohmann::json::object());
    draw_text(os.value("name", "Ubuntu / Debian"), x, 167, 13, kMuted);
    if (button({x + width - 116, 154, 116, 38}, "Refresh", enabled)) refresh("packages");
    auto profile = app.selected >= 0 ? app.selected_server() : across::ServerProfile{};
    std::vector<std::string> catalog{"nginx", "pm2", "node", "npm", "docker"};
    for (const auto& list : {profile.custom_packages, profile.packages}) for (const auto& name : list) if (std::find(catalog.begin(), catalog.end(), name) == catalog.end()) catalog.push_back(name);
    const auto inventory = app.package_state.value("packages", nlohmann::json::object());
    if (contains({x, 215, width, 288}, GetMousePosition())) app.package_scroll = std::clamp(app.package_scroll - static_cast<int>(GetMouseWheelMove()), 0, std::max(0, static_cast<int>(catalog.size()) - 6));
    int row_index = 0;
    for (int index = app.package_scroll; index < static_cast<int>(catalog.size()) && row_index < 6; ++index, ++row_index) {
      const auto& name = catalog[static_cast<std::size_t>(index)];
      const float top = 215 + static_cast<float>(row_index) * 48;
      const Rectangle row{x, top, width, 44};
      const Rectangle box{x + 2, top + 11, 20, 20};
      const bool selected = std::find(profile.packages.begin(), profile.packages.end(), name) != profile.packages.end();
      DrawRectangleRounded(box, 0.15F, 4, selected ? kAccent : kField);
      DrawRectangleRoundedLinesEx(box, 0.15F, 4, 1, selected ? kAccent : kBorderStrong);
      if (selected) {
        DrawLineEx({box.x + 4, box.y + 10}, {box.x + 8, box.y + 14}, 2, WHITE);
        DrawLineEx({box.x + 8, box.y + 14}, {box.x + 16, box.y + 6}, 2, WHITE);
      }
      const std::string label = name == "nginx" ? "Nginx" : name == "pm2" ? "PM2" : name == "node" ? "Node.js" : name == "docker" ? "Docker" : name;
      draw_text(label, x + 42, top + 13, 14, kText);
      const auto details = inventory.value(name, nlohmann::json::object());
      const auto version = details.value("installed", false) ? details.value("version", std::string{"Installed"}) : inventory.contains(name) ? "Not installed" : "Unchecked";
      draw_text(clipped_tail(version, width / 2, 12), x + width - measure_text(clipped_tail(version, width / 2, 12), 12).x, top + 15, 12, details.value("installed", false) ? kSuccess : kFaint);
      DrawLineEx({x, top + 43}, {x + width, top + 43}, 1, kBorder);
      if (enabled && contains(row, GetMousePosition()) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (selected) {
          std::erase(profile.packages, name);
          if (name == "node") { std::erase(profile.packages, "npm"); std::erase(profile.packages, "pm2"); }
          if (name == "npm") std::erase(profile.packages, "pm2");
        } else {
          profile.packages.push_back(name);
          if (name == "pm2" || name == "npm") {
            if (std::find(profile.packages.begin(), profile.packages.end(), "node") == profile.packages.end()) profile.packages.push_back("node");
            if (name == "pm2" && std::find(profile.packages.begin(), profile.packages.end(), "npm") == profile.packages.end()) profile.packages.push_back("npm");
          }
        }
        app.store.save_server(profile); app.servers[static_cast<std::size_t>(app.selected)] = profile;
      }
    }
    field(app.custom_package, x, 548, width - 132);
    if (button({x + width - 116, 548, 116, 38}, "Add", enabled)) {
      const auto names = across::parse_package_names(app.custom_package.value);
      if (names.empty()) throw std::runtime_error("Enter a package name");
      for (const auto& name : names) {
        if (std::find(catalog.begin(), catalog.end(), name) == catalog.end()) profile.custom_packages.push_back(name);
        if (std::find(profile.packages.begin(), profile.packages.end(), name) == profile.packages.end()) profile.packages.push_back(name);
      }
      if (std::find(profile.packages.begin(), profile.packages.end(), "pm2") != profile.packages.end()) {
        for (const auto& dependency : {"node", "npm"}) if (std::find(profile.packages.begin(), profile.packages.end(), dependency) == profile.packages.end()) profile.packages.push_back(dependency);
      }
      app.store.save_server(profile); app.servers[static_cast<std::size_t>(app.selected)] = profile;
      app.package_scroll = std::max(0, static_cast<int>(5 + profile.custom_packages.size()) - 6); app.custom_package.value.clear();
    }
    if (button({x, 620, 168, 38}, "Install selected", enabled && !profile.packages.empty(), ButtonStyle::primary)) {
      const auto server = app.selected_server();
      app.start_job("Install packages", [&app, server](const auto& callback) {
        auto result = across::Deployer(server).server_command({"packages-install", "--packages", nlohmann::json(server.packages).dump(), "--json"}, callback);
        if (result.ok()) {
          auto names = server.packages;
          for (const auto& name : server.custom_packages) if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
          const auto inventory_result = across::Deployer(server).server_command({"packages", "--packages", nlohmann::json(names).dump(), "--json"});
          if (inventory_result.ok()) {
            const auto data = nlohmann::json::parse(inventory_result.output);
            std::scoped_lock lock(app.data_mutex); app.pending_data = data; app.pending_kind = "packages";
          } else app.append_log("Package installation succeeded; version check failed: " + inventory_result.output);
        }
        return result;
      });
    }
    draw_text(std::to_string(profile.packages.size()) + " selected", x + 192, 633, 12, kMuted);
  } else {
    field(app.server_label, x, 235, half); field(app.server_host, x + half + 32, 235, half);
    field(app.server_user, x, 310, half); field(app.server_port, x + half + 32, 310, half);
    field(app.identity, x, 385, width);
    if (button({x, 440, 132, 38}, "Save", !app.busy.load(), ButtonStyle::primary)) app.save_server();
    if (button({x + 144, 440, 132, 38}, "Check", enabled)) { const auto server = app.selected_server(); app.start_job("Check", [server](const auto& callback) { return across::Deployer(server).doctor(callback); }); }
    if (button({x + 288, 440, 176, 38}, "Install backend", enabled)) { const auto server = app.selected_server(); app.start_job("Bootstrap", [server](const auto& callback) { return across::Deployer(server).bootstrap(callback); }); }
    draw_text("Transfer protocol", x, 510, 13, kText, FontWeight::semibold);
    int index = 0;
    for (const auto& protocol : {"rsync", "scp", "sftp"}) {
      if (button({x + static_cast<float>(index++) * 130, 540, 118, 38}, protocol, !app.busy.load(), app.transfer_protocol == protocol ? ButtonStyle::primary : ButtonStyle::secondary)) { app.transfer_protocol = protocol; app.append_log("Save server to use " + app.transfer_protocol + " for file transfers."); }
    }
  }
  } catch (const std::exception& error) { app.append_log(error.what()); }
}

}  // namespace

int main(int argument_count, char** arguments) {
  const bool file_browser_mode =
      argument_count > 1 && std::string{arguments[1]} == "--file-browser";
  if (file_browser_mode && argument_count != 5) {
    return 2;
  }
  const auto across_executable = executable_path(arguments[0]);
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
  InitWindow(file_browser_mode ? 1180 : 1280, file_browser_mode ? 760 : 820,
             file_browser_mode ? "Across File Browser"
                               : "Across Deployment Manager");
  SetWindowMinSize(file_browser_mode ? 980 : 1140,
                   file_browser_mode ? 640 : 800);
  SetTargetFPS(60);

  int exit_code = 0;
  try {
    UiLogo logo;
    UiFonts fonts;
    active_fonts = &fonts;
    if (file_browser_mode) {
      across::ConfigStore store;
      const auto profiles = store.list_servers();
      const auto found = std::find_if(
          profiles.begin(), profiles.end(), [&](const auto& profile) {
            return profile.id == arguments[2];
          });
      if (found == profiles.end()) {
        throw std::runtime_error("The selected server profile no longer exists");
      }
      run_file_browser(*found, arguments[3], arguments[4]);
    } else {
      Application app;
      while (!WindowShouldClose()) {
      app.check_finished_jobs();
      const float width = static_cast<float>(GetScreenWidth());
      const float height = static_cast<float>(GetScreenHeight());
      constexpr float content_x = 280.0F;
      const float content_width = std::clamp(width - content_x - 40.0F, 800.0F, 1040.0F);

      if (IsFileDropped() && app.page != 9) {
        const FilePathList dropped = LoadDroppedFiles();
        if (app.page == 0 && contains(app.project_root.bounds, GetMousePosition()) && dropped.count == 1) {
          try { app.select_project_folder(dropped.paths[0]); }
          catch (const std::exception& error) { app.append_log(error.what()); }
        } else if (contains(app.manifest.bounds, GetMousePosition())) {
          app.select_dropped_manifest(dropped);
        } else if (app.selected >= 0 && dropped.count > 0) {
          std::filesystem::path local_start{dropped.paths[0]};
          std::error_code error;
          if (!std::filesystem::is_directory(local_start, error)) {
            local_start = local_start.parent_path();
          }
          try {
            launch_file_browser(
                across_executable,
                app.servers[static_cast<std::size_t>(app.selected)].id,
                local_start, "/tmp");
            app.append_log("Opened the file browser for dropped items.");
          } catch (const std::exception& launch_error) {
            app.append_log(std::string{"File browser failed: "} +
                           launch_error.what());
          }
        } else {
          app.append_log("Select a server before opening dropped files.");
        }
        UnloadDroppedFiles(dropped);
      }

      field_help.clear();
      BeginDrawing();
      ClearBackground(kBackground);
      draw_server_sidebar(app, height);

      const std::vector<std::string> page_titles{"Projects", "Websites", "Domains", "Pipelines", "Resources", "Firewall", "Connection", "Nginx", "Packages", "Files"};
      draw_text(page_titles.at(static_cast<std::size_t>(app.page)), content_x, 27.0F, 25.0F, kText,
                FontWeight::semibold);
      const std::string status = app.busy.load()
          ? "Working"
          : (app.watcher_running ? "Watching" : "Ready");
      const float status_width = measure_text(status, 12.5F).x;
      DrawCircleV({width - status_width - 38.0F, 31.0F}, 4.0F,
                  app.busy.load() ? kAccent : kSuccess);
      draw_text(status, width - status_width - 26.0F, 23.5F, 12.5F, kMuted,
                FontWeight::semibold);
      if (button({width - 266, 14, 134, 34}, "Refresh all", app.selected >= 0 && !app.busy.load())) app.refresh_all();

      if (!app.show_activity) draw_management(app, content_x, content_width);
      else draw_wrapped_log(app.log_snapshot(), {content_x, 154, content_width, height - 232});

      const float activity_y = height - 48;
      DrawLineEx({content_x, activity_y - 12}, {content_x + content_width, activity_y - 12}, 1, kBorder);
      if (button({content_x, activity_y, 112, 30}, app.show_activity ? "Back" : "Activity", true, ButtonStyle::ghost)) {
        app.show_activity = !app.show_activity;
        app.nginx_editor.active = false;
        for (auto* value : app.management_fields()) value->active = false;
      }
      if (!app.show_activity) {
        std::istringstream messages(app.log_snapshot()); std::string line, latest;
        while (std::getline(messages, line)) if (!line.empty()) latest = line;
        const float available = content_width - 150;
        bool truncated = false;
        while (!latest.empty() && measure_text(latest, 12).x > available) { latest.pop_back(); truncated = true; }
        if (truncated && latest.size() > 3) latest.replace(latest.size() - 3, 3, "...");
        draw_text(latest, content_x + 138, activity_y + 8, 12, kMuted);
      }
      draw_field_help();
      EndDrawing();
      }
    }
    active_fonts = nullptr;
  } catch (const std::exception& error) {
    active_fonts = nullptr;
    TraceLog(LOG_ERROR, "Across desktop failed: %s", error.what());
    exit_code = 1;
  }

  CloseWindow();
  return exit_code;
}
