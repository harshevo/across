#define main across_desktop_entrypoint
#include "../src/desktop/main.cpp"
#undef main
#include <iostream>

int main() {
  const auto root = std::filesystem::temp_directory_path() / ("across-desktop-test-" + across::make_release_id());
  std::filesystem::create_directories(root / "project");
  setenv("ACROSS_CONFIG_DIR", root.c_str(), 1);
  auto require = [](bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
  };
  try {
    const nlohmann::json traffic{{"pages", {{{"name", "/old"}, {"last_seen", 100.0}, {"requests", 500}}, {{"name", "/latest"}, {"last_seen", 200.0}, {"requests", 1}}}}};
    const auto table = monitoring_table(traffic, 1, "");
    require(table.rows.size() == 2 && table.rows[0][0] == "/latest", "Page table must prioritize freshness over request volume");
    require(monitoring_table(traffic, 1, "/old").rows.size() == 1, "Table filter must preserve matching rows");
    require(table_scroll_limit(5) == 0 && table_scroll_limit(12) == 7, "Tables must show five rows and scroll the remaining rows");
    const nlohmann::json ssh{{"connections", {{"output", "State Recv-Q Send-Q Local Peer Process\nESTAB 0 0 127.0.0.1:22 203.0.113.2:4321 sshd\n"}}}, {"ssh_activity", {{"output", "2026-10-11T01:00:00+00:00 host sshd[1]: Accepted publickey for root from 203.0.113.3 port 22\n2026-10-11T02:00:00+00:00 host sshd[1]: Failed password for root from 203.0.113.4 port 22\n"}}}};
    const auto connections = connection_table(ssh);
    require(connections.rows.size() == 3 && connections.rows[0][0] == "Active now" && connections.rows[1][2] == "203.0.113.4", "Active connections and newest SSH activity must come first");
    across::ServerProfile profile;
    profile.id = "test"; profile.label = "Test"; profile.host = "127.0.0.1"; profile.user = "test";
    across::ConfigStore{}.save_server(profile);
    across::ProjectManifest project;
    project.app_id = "example"; project.name = "Example"; project.project_root = root / "project";
    project.type = "static"; project.nginx.domains = {"example.com"};
    const auto manifest_path = root / "project/across.json";
    { std::ofstream output(manifest_path); output << nlohmann::json(project).dump(); }
    {
      Application app;
      app.select_project_folder(root / "project");
      require(app.app_id.value == "example", "Folder selection must load an existing manifest");
      app.page = 2; app.save_desktop_state();
    }
    {
      Application app;
      require(app.selected_server().id == "test", "Selected server was not restored");
      require(app.page == 2, "Page was not restored");
      require(app.manifest.value == std::filesystem::canonical(manifest_path).string(), "Project manifest was not restored");
      require(app.recent_projects().size() == 1, "Recent projects should not duplicate");
      require(app.refresh_requested, "Startup must request live refresh");
      app.accept_monitoring({{"host", {{"timestamp", 100.0}, {"cpu_total", 1000}, {"cpu_idle", 500}, {"rx_bytes", 1000}, {"tx_bytes", 2000}}}});
      require(app.host_cpu < 0, "CPU needs two samples");
      app.accept_monitoring({{"host", {{"timestamp", 102.0}, {"cpu_total", 1200}, {"cpu_idle", 550}, {"rx_bytes", 1400}, {"tx_bytes", 2200}}}});
      require(app.host_cpu == 75.0 && app.network_rx_rate == 200.0 && app.network_tx_rate == 100.0, "CPU or network rate used cumulative counters instead of sample deltas");
      app.accept_monitoring({{"host", {{"timestamp", 104.0}, {"cpu_total", 20}, {"cpu_idle", 10}, {"rx_bytes", 10}, {"tx_bytes", 10}}}});
      require(app.host_cpu < 0 && app.network_rx_rate == 0 && app.network_tx_rate == 0, "Server restart must reset counter-derived metrics");
      std::filesystem::create_directory(root / "new-project");
      app.select_project_folder(root / "new-project");
      require(app.manifest.value.empty() && app.app_id.value == "new-project", "New folder inherited prior project settings");
      require(!std::filesystem::exists(root / "new-project/across.json"), "Selecting a folder must not silently write a manifest");
    }
    const auto text_path = root / "project/file.txt";
    { std::ofstream output(text_path); output << "original\n"; }
    FileBrowser browser{profile, root / "project", "/tmp", false};
    for (std::size_t index = 0; index < browser.local_entries.size(); ++index) {
      if (browser.local_entries[index].name == "file.txt") browser.local_selected = static_cast<int>(index);
    }
    browser.open_editor(false); browser.editor.value = "edited\n"; browser.save_editor();
    require(FileBrowser::read_text_file(text_path) == "edited\n", "Local text edit was not saved");
    { std::ofstream output(text_path); output << "external change\n"; }
    bool conflict = false;
    try { browser.save_editor(); } catch (const std::exception&) { conflict = true; }
    require(conflict, "Concurrent local edits must not be overwritten");
    { std::ofstream output(root / "binary", std::ios::binary); output.put('\0'); }
    bool binary_rejected = false;
    try { FileBrowser::read_text_file(root / "binary"); } catch (const std::exception&) { binary_rejected = true; }
    require(binary_rejected, "Binary files must not be edited as text");
    { std::ofstream output(root / "large"); output << std::string(65537, 'x'); }
    bool large_rejected = false;
    try { FileBrowser::read_text_file(root / "large"); } catch (const std::exception&) { large_rejected = true; }
    require(large_rejected, "Oversized files must be rejected");
    browser.new_name.value = "created-folder"; browser.mutate_local("mkdir");
    require(std::filesystem::is_directory(root / "project/created-folder"), "Local folder creation failed");
    for (std::size_t index = 0; index < browser.local_entries.size(); ++index) {
      if (browser.local_entries[index].name == "created-folder") browser.local_selected = static_cast<int>(index);
    }
    browser.new_name.value = "renamed-folder"; browser.mutate_local("rename");
    require(std::filesystem::is_directory(root / "project/renamed-folder"), "Local folder rename failed");
    std::filesystem::remove_all(root);
    std::cout << "Desktop persistence and file editing tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; std::filesystem::remove_all(root); return 1;
  }
}
