std::string metric_number(double value, int precision = 1) {
  std::ostringstream text; text << std::fixed << std::setprecision(precision) << value; return text.str();
}

std::string bytes_label(double bytes) {
  if (bytes >= 1024 * 1024 * 1024) return metric_number(bytes / (1024 * 1024 * 1024)) + " GiB";
  if (bytes >= 1024 * 1024) return metric_number(bytes / (1024 * 1024)) + " MiB";
  if (bytes >= 1024) return metric_number(bytes / 1024) + " KiB";
  return metric_number(bytes, 0) + " B";
}

void admin_field(TextField& field, Rectangle bounds) { field.bounds = bounds; field.update(); field.draw(); }

std::string local_time_label(double seconds, bool date = false) {
  if (seconds <= 0) return "--";
  const auto value = static_cast<std::time_t>(seconds); std::tm time{};
#ifdef _WIN32
  localtime_s(&time, &value);
#else
  localtime_r(&value, &time);
#endif
  char buffer[40]; std::strftime(buffer, sizeof(buffer), date ? "%d %b %H:%M:%S" : "%H:%M:%S", &time);
  return buffer;
}

std::string table_cell_text(std::string value, float width) {
  std::replace(value.begin(), value.end(), '\n', ' ');
  std::replace(value.begin(), value.end(), '\t', ' ');
  if (measure_text(value, 11).x <= width) return value;
  while (!value.empty() && measure_text(value + "...", 11).x > width) value.pop_back();
  return value + "...";
}

struct AdminTable {
  std::vector<std::pair<std::string, float>> columns;
  std::vector<std::vector<std::string>> rows;
};

int table_scroll_limit(std::size_t rows) { return std::max(0, static_cast<int>(rows) - 5); }

int draw_admin_table(const AdminTable& table, Rectangle area, int& first, bool& dragging, float& grab,
                      const std::string& order = "Latest first") {
  constexpr float header = 30, row_height = 28;
  const Rectangle body{area.x, area.y + header, area.width - 20, row_height * 5};
  const Rectangle track{area.x + area.width - 12, body.y, 8, body.height};
  const int maximum = table_scroll_limit(table.rows.size());
  if (contains(area, GetMousePosition())) first -= static_cast<int>(GetMouseWheelMove() * 2);
  first = std::clamp(first, 0, maximum);
  const float thumb_height = maximum ? std::max(24.0F, body.height * 5 / static_cast<float>(table.rows.size())) : body.height;
  const float travel = body.height - thumb_height;
  Rectangle thumb{track.x, track.y + (maximum ? travel * static_cast<float>(first) / static_cast<float>(maximum) : 0), track.width, thumb_height};
  const auto mouse = GetMousePosition();
  if (maximum && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && contains({track.x - 4, track.y, 16, track.height}, mouse)) {
    dragging = true; grab = contains(thumb, mouse) ? mouse.y - thumb.y : thumb_height / 2;
  }
  if (dragging && maximum && (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonReleased(MOUSE_BUTTON_LEFT))) {
    first = std::clamp(static_cast<int>(std::round((mouse.y - track.y - grab) / travel * static_cast<float>(maximum))), 0, maximum);
    thumb.y = track.y + travel * static_cast<float>(first) / static_cast<float>(maximum);
  }
  if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) dragging = false;
  DrawRectangleRec({area.x, area.y, area.width, header}, kSurface);
  float total = 0; for (const auto& column : table.columns) total += column.second;
  float left = area.x;
  for (const auto& [label, weight] : table.columns) {
    const float span = body.width * weight / std::max(1.0F, total);
    draw_text(table_cell_text(label, span - 20), left + 10, area.y + 9, 11, kFaint); left += span;
  }
  std::string tooltip; int clicked = -1;
  for (int row = 0; row < 5; ++row) {
    const float y = body.y + static_cast<float>(row) * row_height;
    const int index = first + row;
    if (index < static_cast<int>(table.rows.size())) {
      const Rectangle bounds{body.x, y, body.width, row_height};
      if (contains(bounds, mouse)) DrawRectangleRec(bounds, kSurfaceRaised);
      if (contains(bounds, mouse) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !dragging) clicked = index;
      left = area.x;
      const auto& cells = table.rows[static_cast<std::size_t>(index)];
      for (std::size_t column = 0; column < table.columns.size(); ++column) {
        const float span = body.width * table.columns[column].second / std::max(1.0F, total);
        const auto value = column < cells.size() ? cells[column] : "";
        draw_text(table_cell_text(value, span - 20), left + 10, y + 8, 11, column == 0 ? kText : kMuted);
        if (contains({left, y, span, row_height}, mouse) && measure_text(value, 11).x > span - 20) tooltip = value;
        left += span;
      }
    }
    DrawLineEx({area.x, y + row_height}, {area.x + area.width, y + row_height}, 1, kBorder);
  }
  DrawRectangleRounded(track, 0.5F, 4, kSurfaceRaised);
  DrawRectangleRounded(thumb, 0.5F, 4, maximum ? kMuted : kBorder);
  if (table.rows.empty()) draw_text("No matching data. Refresh or change the filter.", body.x + 10, body.y + 17, 12, kMuted);
  const auto count = table.rows.size();
  draw_text(count ? std::to_string(first + 1) + "-" + std::to_string(std::min(static_cast<std::size_t>(first + 5), count)) + " of " + std::to_string(count) : "0 rows", area.x, body.y + body.height + 10, 10, kFaint);
  draw_text(order + (maximum ? " | Scroll for more" : ""), area.x + area.width - 225, body.y + body.height + 10, 10, kFaint);
  if (!tooltip.empty() && !dragging) {
    const float span = std::min(area.width, measure_text(tooltip, 11).x + 24);
    DrawRectangleRec({area.x, area.y - 30, span, 26}, kSurfaceRaised);
    draw_text(table_cell_text(tooltip, span - 20), area.x + 10, area.y - 22, 11, kText);
  }
  return clicked;
}

AdminTable monitoring_table(const nlohmann::json& data, int section, const std::string& filter) {
  AdminTable table;
  const std::vector<std::string> keys{"projects", "pages", "visitors", "devices", "recent", "statuses", "host"};
  const auto key = keys.at(static_cast<std::size_t>(section));
  if (key == "host") {
    table.columns = {{"Metric", 2}, {"Current value", 3}};
    const auto host = data.value("host", nlohmann::json::object());
    table.rows = {{"Sampled", local_time_label(host.value("timestamp", 0.0), true)},
      {"Memory used / total", bytes_label(host.value("memory_total", 0.0) - host.value("memory_available", 0.0)) + " / " + bytes_label(host.value("memory_total", 0.0))},
      {"Disk used / total", bytes_label(host.value("disk_total", 0.0) - host.value("disk_free", 0.0)) + " / " + bytes_label(host.value("disk_total", 0.0))},
      {"Network received", bytes_label(host.value("rx_bytes", 0.0))}, {"Network sent", bytes_label(host.value("tx_bytes", 0.0))}, {"Load average", host.value("loadavg", "--")}};
    for (const auto& interface : host.value("interfaces", nlohmann::json::array())) table.rows.push_back({interface.value("name", ""), "RX " + bytes_label(interface.value("rx_bytes", 0.0)) + " / TX " + bytes_label(interface.value("tx_bytes", 0.0))});
  } else {
    auto rows = data.value(key, nlohmann::json::array());
    if (key == "recent") std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return std::stod(a.value("time", "0")) > std::stod(b.value("time", "0")); });
    else if (key != "statuses") std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.value("last_seen", 0.0) > b.value("last_seen", 0.0); });
    if (key == "recent") table.columns = {{"Time (local)", 1.3F}, {"Visitor IP", 1.4F}, {"Method", .65F}, {"Project / page", 2.4F}, {"Status", .6F}, {"Device", 1.5F}, {"Latency", .85F}};
    else if (key == "statuses") table.columns = {{"HTTP status", 2}, {"Requests", 2}};
    else table.columns = {{key == "projects" ? "Project" : key == "pages" ? "Project / page" : key == "visitors" ? "Visitor IP" : "Device", 2.6F}, {"Last seen", 1.3F}, {"Requests", .8F}, {"IPs", .55F}, {"Bandwidth", 1}, {"Avg / p95", 1.2F}, {"Errors", .7F}};
    for (const auto& row : rows) {
      if (!filter.empty() && row.dump().find(filter) == std::string::npos) continue;
      if (key == "recent") table.rows.push_back({local_time_label(std::stod(row.value("time", "0")), true), row.value("ip", ""), row.value("method", ""), row.value("app_id", "") + " " + row.value("path", ""), std::to_string(row.value("status", 0)), row.value("device", ""), metric_number(std::stod(row.value("duration", "0")) * 1000) + " ms"});
      else if (key == "statuses") table.rows.push_back({std::to_string(row.value("status", 0)), std::to_string(row.value("requests", 0))});
      else table.rows.push_back({row.value("name", ""), local_time_label(row.value("last_seen", 0.0), true), std::to_string(row.value("requests", 0)), std::to_string(row.value("visitors", 0)), bytes_label(row.value("bytes", 0.0)), metric_number(row.value("avg_ms", 0.0)) + " / " + metric_number(row.value("p95_ms", 0.0)) + " ms", std::to_string(row.value("errors", 0))});
    }
  }
  if (key == "host" && !filter.empty()) std::erase_if(table.rows, [&](const auto& row) { return row[0].find(filter) == std::string::npos && row[1].find(filter) == std::string::npos; });
  return table;
}

AdminTable connection_table(const nlohmann::json& data) {
  AdminTable table{{{"Time (local)", 1.3F}, {"State / event", 1.2F}, {"Peer", 1.8F}, {"Details", 4}}, {}};
  std::istringstream connections(data.value("connections", nlohmann::json::object()).value("output", ""));
  std::string line;
  while (std::getline(connections, line)) {
    std::istringstream row(line); std::string state, received, sent, local, peer, details;
    if (!(row >> state >> received >> sent >> local >> peer) || state == "State") continue;
    std::getline(row, details); table.rows.push_back({"Active now", state, peer, local + " " + details});
  }
  std::vector<std::vector<std::string>> events;
  std::istringstream activity(data.value("ssh_activity", nlohmann::json::object()).value("output", ""));
  while (std::getline(activity, line)) {
    if (line.size() < 20 || line[4] != '-') continue;
    const auto marker = line.find("]: "); const auto space = line.find(' ');
    if (marker == std::string::npos || space == std::string::npos) continue;
    const auto detail = line.substr(marker + 3); std::string peer = "--";
    const auto from = detail.find(" from ");
    if (from != std::string::npos) { const auto end = detail.find(' ', from + 6); peer = detail.substr(from + 6, end - from - 6); }
    const auto event = detail.find("Accepted") != std::string::npos ? "Accepted" : detail.find("Failed") != std::string::npos || detail.find("Invalid user") != std::string::npos ? "Rejected" : detail.find("closed") != std::string::npos ? "Closed" : "SSH event";
    events.push_back({line.substr(0, space), event, peer, detail});
  }
  std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a[0] > b[0]; });
  for (auto& row : events) {
    std::tm time{}; std::istringstream timestamp(row[0].substr(0, 19)); timestamp >> std::get_time(&time, "%Y-%m-%dT%H:%M:%S");
    if (!timestamp.fail()) {
#ifdef _WIN32
      const auto seconds = _mkgmtime(&time);
#else
      const auto seconds = timegm(&time);
#endif
      int offset = 0;
      if (row[0].size() >= 25 && (row[0][19] == '+' || row[0][19] == '-')) {
        try { offset = (std::stoi(row[0].substr(20, 2)) * 60 + std::stoi(row[0].substr(23, 2))) * 60 * (row[0][19] == '+' ? 1 : -1); } catch (...) {}
      }
      row[0] = local_time_label(static_cast<double>(seconds - offset), true);
    }
    table.rows.push_back(std::move(row));
  }
  return table;
}

void admin_report(const std::string& report, Rectangle area, int& scroll) {
  std::vector<std::string> lines; std::istringstream input(report); std::string line;
  const auto columns = static_cast<std::size_t>(std::max(20.0F, area.width / 7));
  while (std::getline(input, line)) {
    std::replace(line.begin(), line.end(), '\t', ' ');
    do { lines.push_back(line.substr(0, columns)); line = line.size() > columns ? line.substr(columns) : ""; } while (!line.empty());
  }
  const int visible = std::max(1, static_cast<int>(area.height / 21));
  if (contains(area, GetMousePosition())) scroll -= static_cast<int>(GetMouseWheelMove() * 3);
  scroll = std::clamp(scroll, 0, std::max(0, static_cast<int>(lines.size()) - visible));
  BeginScissorMode(static_cast<int>(area.x), static_cast<int>(area.y), static_cast<int>(area.width), static_cast<int>(area.height));
  for (int row = 0; row < visible && scroll + row < static_cast<int>(lines.size()); ++row) draw_text(lines[static_cast<std::size_t>(scroll + row)], area.x, area.y + static_cast<float>(row) * 21, 12, kMuted);
  EndScissorMode();
}

std::string security_reading(const nlohmann::json& data, const std::string& key) {
  const auto reading = data.value(key, nlohmann::json::object());
  return key + ": " + (reading.value("available", false) ? "" : "Unavailable or not installed\n") + reading.value("output", "Refresh to load") + "\n";
}

void draw_security_page(Application& app, float x, float width) {
  const bool enabled = app.selected >= 0 && !app.busy.load();
  if (!app.security_attempted && enabled) app.refresh_admin("security");
  if (button({x + width - 150, 147, 150, 38}, "Refresh", enabled)) app.refresh_admin("security");
  draw_text("Server access and protection", x, 158, 14, kMuted);
  draw_text(app.busy.load() ? "Refreshing..." : "Updated " + local_time_label(app.security_received) + " local", x + width - 370, 158, 11, kFaint);
  const std::vector<std::string> sections{"Overview", "SSH", "Users", "Processes", "Firewall", "Fail2ban", "Websites"};
  float left = x;
  for (int index = 0; index < static_cast<int>(sections.size()); ++index) {
    const float span = measure_text(sections[static_cast<std::size_t>(index)], 12).x + 28;
    if (button({left, 202, span, 34}, sections[static_cast<std::size_t>(index)], true, app.security_section == index ? ButtonStyle::primary : ButtonStyle::ghost)) { app.security_section = index; app.security_scroll = 0; app.process_confirmation = {}; }
    left += span + 8;
  }
  const auto& data = app.security_data;
  const float bottom = static_cast<float>(GetScreenHeight()) - 100;
  if (app.security_section == 0) {
    const auto users = data.value("users", nlohmann::json::array());
    const auto processes = data.value("processes", nlohmann::json::array());
    const bool ping = data.value("ping_v4", true);
    const bool jail = data.value("sshd_jail", nlohmann::json::object()).value("available", false);
    const std::vector<std::pair<std::string, std::string>> rows{
      {"Administrator IP", data.value("administrator_ip", "Loading")}, {"Domain-only access", data.value("domain_only", false) ? "Enabled" : "Disabled"},
      {"SSH brute-force protection", jail ? "Fail2ban sshd jail active" : "Not active"}, {"IPv4 ping", ping ? "Allowed" : "Blocked"},
      {"IPv6 ping", !data.value("ping_v6_available", false) ? "Not supported by this kernel" : data.value("ping_v6", true) ? "Allowed" : "Blocked"},
      {"Server accounts", std::to_string(users.size())}, {"Running processes", std::to_string(processes.size())}};
    float y = 259;
    for (const auto& [label, value] : rows) { draw_text(label, x, y, 13, kMuted); draw_text(value, x + 290, y, 13, kText); y += 31; }
    if (button({x, y + 15, 175, 38}, ping ? "Disable ping" : "Enable ping", enabled)) app.admin_operation("Ping policy", {"security-action", "--action", "ping", "--mode", ping ? "block" : "allow"});
    if (button({x + 187, y + 15, 218, 38}, "Enforce SSH keys only", enabled)) app.admin_operation("SSH key authentication", {"security-action", "--action", "ssh-keys-only"});
    if (button({x + 417, y + 15, 170, 38}, "Service limits", true)) app.page = 4;
    draw_text("Ping is controlled per server IP. Device details are reported by clients.", x, y + 67, 12, kFaint);
    admin_report(security_reading(data, "updates"), {x, y + 105, width, std::max(50.0F, bottom - y - 105)}, app.security_scroll);
  } else if (app.security_section == 1) {
    draw_text(app.security_ssh_details ? "SSH settings and login history" : "Active connections and latest SSH activity", x, 261, 13, kText);
    if (button({x + width - 160, 253, 160, 30}, app.security_ssh_details ? "Show connections" : "Settings / history", true)) { app.security_ssh_details = !app.security_ssh_details; app.security_scroll = 0; }
    if (app.security_ssh_details) admin_report(security_reading(data, "sessions") + security_reading(data, "login_history") + security_reading(data, "ssh_config"), {x, 307, width, bottom - 307}, app.security_scroll);
    else draw_admin_table(connection_table(data), {x, 305, width, 170}, app.security_scroll, app.security_scroll_drag, app.security_scroll_grab, "Active now / latest first");
  } else if (app.security_section == 2) {
    AdminTable table{{{"User", 1.5F}, {"UID / GID", 1}, {"Home", 2}, {"Shell", 2}}, {}};
    for (const auto& user : data.value("users", nlohmann::json::array())) table.rows.push_back({user.value("name", ""), user.value("uid", "") + " / " + user.value("gid", ""), user.value("home", ""), user.value("shell", "")});
    draw_admin_table(table, {x, 270, width, 170}, app.security_scroll, app.security_scroll_drag, app.security_scroll_grab, "Server accounts");
  } else if (app.security_section == 3) {
    admin_field(app.process_filter, {x, 270, width, 38});
    const auto processes = data.value("processes", nlohmann::json::array());
    std::vector<int> filtered;
    for (int index = 0; index < static_cast<int>(processes.size()); ++index) {
      const auto& process = processes[static_cast<std::size_t>(index)];
      const auto text = std::to_string(process.value("pid", 0)) + " " + process.value("user", "") + " " + process.value("name", "");
      if (app.process_filter.value.empty() || text.find(app.process_filter.value) != std::string::npos) filtered.push_back(index);
    }
    draw_text("PID / USER", x, 325, 11, kFaint); draw_text("CPU %", x + 170, 325, 11, kFaint); draw_text("RAM %", x + 250, 325, 11, kFaint); draw_text("ELAPSED / PROCESS", x + 330, 325, 11, kFaint);
    const Rectangle area{x, 345, width, std::max(60.0F, bottom - 410)};
    const int visible = std::max(1, static_cast<int>(area.height / 29));
    if (contains(area, GetMousePosition())) app.security_scroll -= static_cast<int>(GetMouseWheelMove() * 3);
    app.security_scroll = std::clamp(app.security_scroll, 0, std::max(0, static_cast<int>(filtered.size()) - visible));
    for (int row = 0; row < visible && row + app.security_scroll < static_cast<int>(filtered.size()); ++row) {
      const int index = filtered[static_cast<std::size_t>(row + app.security_scroll)]; const auto& process = processes[static_cast<std::size_t>(index)]; const float y = area.y + static_cast<float>(row) * 29;
      if (button({x, y, width, 27}, "", enabled, index == app.process_selected ? ButtonStyle::primary : ButtonStyle::ghost)) { app.process_selected = index; app.process_confirmation = {}; }
      draw_text(std::to_string(process.value("pid", 0)) + " / " + process.value("user", ""), x + 8, y + 5, 12, kText);
      draw_text(process.value("cpu", ""), x + 170, y + 5, 12, kMuted); draw_text(process.value("memory", ""), x + 250, y + 5, 12, kMuted);
      draw_text(clipped_tail(process.value("elapsed", "") + " / " + process.value("name", ""), width - 335, 12), x + 330, y + 5, 12, kMuted);
    }
    const bool selected = app.process_selected >= 0 && app.process_selected < static_cast<int>(processes.size());
    const bool confirm = std::chrono::steady_clock::now() - app.process_confirmation < std::chrono::seconds{5};
    for (int index = 0; index < 2; ++index) if (button({x + static_cast<float>(index) * 200, bottom - 40, 188, 38}, confirm ? (index ? "Confirm force kill" : "Confirm terminate") : (index ? "Force kill selected" : "Terminate selected"), enabled && selected, ButtonStyle::danger)) {
      if (!confirm) { app.process_confirmation = std::chrono::steady_clock::now(); app.append_log("Click again within 5 seconds to signal the selected process."); }
      else { const auto& process = processes[static_cast<std::size_t>(app.process_selected)]; app.admin_operation("Signal process", {"security-action", "--action", "kill-process", "--pid", std::to_string(process.value("pid", 0)), "--start", process.value("start", ""), "--signal", index ? "KILL" : "TERM"}); app.process_confirmation = {}; }
    }
    draw_text(std::to_string(filtered.size()) + " processes. SSH and system managers are protected.", x + 410, bottom - 27, 11, kFaint);
  } else if (app.security_section == 4) {
    admin_field(app.firewall_port, {x, 271, 150, 38}); admin_field(app.firewall_protocol, {x + 170, 271, 150, 38});
    for (int index = 0; index < 4; ++index) {
      const std::vector<std::string> actions{"allow", "deny", "delete", "unblock"}, labels{"Open port", "Block port", "Remove allow", "Remove block"};
      if (button({x + 340 + static_cast<float>(index) * 125, 271, 115, 38}, labels[static_cast<std::size_t>(index)], enabled)) app.admin_operation(labels[static_cast<std::size_t>(index)], {"firewall", "--action", actions[static_cast<std::size_t>(index)], "--port", app.firewall_port.value, "--protocol", app.firewall_protocol.value, "--ssh-port", std::to_string(app.selected_server().port)});
    }
    admin_field(app.security_ip, {x, 351, width - 360, 38});
    if (button({x + width - 345, 351, 160, 38}, "Block server IP", enabled)) app.admin_operation("Block server IP", {"security-action", "--action", "block-ip", "--ip", app.security_ip.value});
    if (button({x + width - 172, 351, 172, 38}, "Unblock server IP", enabled)) app.admin_operation("Unblock server IP", {"security-action", "--action", "unblock-ip", "--ip", app.security_ip.value});
    draw_text("Server IP rules affect direct connections. Use Websites to block visitors behind Cloudflare.", x, 405, 12, kFaint);
    if (button({x, 431, 150, 34}, "Install UFW", enabled)) app.admin_operation("Install firewall", {"firewall", "--action", "install"});
    if (button({x + 162, 431, 170, 34}, "Enable firewall", enabled)) app.admin_operation("Enable firewall", {"firewall", "--action", "enable", "--ssh-port", std::to_string(app.selected_server().port)});
    admin_report(security_reading(data, "firewall") + security_reading(data, "ports"), {x, 484, width, std::max(50.0F, bottom - 484)}, app.security_scroll);
  } else if (app.security_section == 5) {
    const float third = (width - 40) / 3;
    admin_field(app.fail_retry, {x, 271, third, 38}); admin_field(app.fail_window, {x + third + 20, 271, third, 38}); admin_field(app.fail_ban, {x + 2 * (third + 20), 271, third, 38});
    for (int index = 0; index < 2; ++index) if (button({x + static_cast<float>(index) * 230, 329, 218, 38}, index ? "Apply limits" : "Install & protect SSH", enabled)) app.admin_operation("Fail2ban settings", {"security-action", "--action", index ? "fail2ban-limits" : "install-fail2ban", "--maxretry", app.fail_retry.value, "--findtime", app.fail_window.value, "--bantime", app.fail_ban.value, "--ssh-port", std::to_string(app.selected_server().port)});
    admin_field(app.security_ip, {x, 414, width - 310, 38});
    if (button({x + width - 295, 414, 135, 38}, "Ban SSH IP", enabled)) app.admin_operation("Ban SSH IP", {"security-action", "--action", "ban-ip", "--ip", app.security_ip.value});
    if (button({x + width - 147, 414, 147, 38}, "Unban SSH IP", enabled)) app.admin_operation("Unban SSH IP", {"security-action", "--action", "unban-ip", "--ip", app.security_ip.value});
    admin_report(security_reading(data, "fail2ban") + security_reading(data, "sshd_jail"), {x, 480, width, std::max(50.0F, bottom - 480)}, app.security_scroll);
  } else {
    admin_field(app.app_id, {x, 271, width - 470, 38});
    if (button({x + width - 455, 271, 218, 38}, "Restart website", enabled, ButtonStyle::primary)) app.admin_operation("Restart website", {"security-action", "--action", "restart-website", "--app", app.app_id.value});
    if (button({x + width - 225, 271, 225, 38}, "Website limits / Nginx", true)) app.page = 1;
    admin_field(app.security_ip, {x, 351, width - 340, 38});
    if (button({x + width - 325, 351, 150, 38}, "Block visitor IP", enabled)) app.admin_operation("Block website visitor", {"security-action", "--action", "website-block-ip", "--ip", app.security_ip.value, "--app", app.app_id.value});
    if (button({x + width - 163, 351, 163, 38}, "Unblock visitor", enabled)) app.admin_operation("Unblock website visitor", {"security-action", "--action", "website-unblock-ip", "--ip", app.security_ip.value, "--app", app.app_id.value});
    std::string report;
    for (const auto& site : data.value("websites", nlohmann::json::array())) {
      report += site.value("app_id", "") + " | " + (site.value("https", false) ? "HTTPS enabled" : "HTTP only") + "\nDomains: ";
      for (const auto& domain : site.value("domains", std::vector<std::string>{})) report += domain + " ";
      report += "\nRate: " + std::to_string(site.value("rate", 0)) + " requests/s | Burst: " + std::to_string(site.value("burst", 0)) + " | Body: " + std::to_string(site.value("body_mb", 20)) + " MiB\nBlocked visitors: ";
      const auto ips = site.value("blocked_ips", std::vector<std::string>{});
      if (ips.empty()) report += "None"; else for (const auto& ip : ips) report += ip + " ";
      report += "\n";
      if (site.contains("certificate")) report += "Certificate:\n" + site["certificate"].value("output", "Unavailable");
      report += "\n";
    }
    admin_report(report.empty() ? "No deployed websites found." : report, {x, 416, width, std::max(50.0F, bottom - 416)}, app.security_scroll);
  }
}

void draw_monitoring_page(Application& app, float x, float width) {
  const bool enabled = app.selected >= 0 && !app.busy.load();
  if (!app.monitoring_attempted && enabled) app.refresh_admin("monitoring");
  const auto& data = app.monitoring_data;
  const auto summary = data.value("summary", nlohmann::json::object());
  const auto host = data.value("host", nlohmann::json::object());
  const float bottom = static_cast<float>(GetScreenHeight()) - 106;
  draw_text("Traffic overview", x, 154, 14, kText, FontWeight::semibold);
  draw_text(app.busy.load() ? "Refreshing..." : "Updated " + local_time_label(app.monitoring_received) + " local", x + width - 205, 154, 11, kMuted);
  const std::vector<float> weights{1.25F, .65F, 1.2F, 1.4F, 1.2F, 1.4F};
  std::vector<Rectangle> controls; float control_x = x;
  for (const float weight : weights) { const float span = (width - 60) * weight / 7.1F; controls.push_back({control_x, 196, span, 34}); control_x += span + 12; }
  admin_field(app.traffic_app, controls[0]);
  admin_field(app.traffic_hours, controls[1]);
  if (button(controls[2], "Refresh traffic", enabled)) { app.monitoring_scroll = 0; app.refresh_admin("monitoring"); }
  if (button(controls[3], app.monitoring_auto ? "Auto refresh: on" : "Auto refresh: off", true)) app.monitoring_auto = !app.monitoring_auto;
  if (button(controls[4], "Export report", !data.empty())) {
    const auto root = app.project_root.value.empty() ? across::config_directory() : std::filesystem::path(app.project_root.value) / ".across/reports";
    std::filesystem::create_directories(root); const auto path = root / ("monitoring-" + across::make_release_id() + ".json");
    std::ofstream output(path); output << data.dump(2); output.close(); if (!output) throw std::runtime_error("Cannot export monitoring report"); app.append_log("Report saved: " + path.string());
  }
  const bool logging = data.value("logging", nlohmann::json::object()).value(app.app_id.value, false);
  if (button(controls[5], logging ? "Pause logging" : "Enable logging", enabled && !app.app_id.value.empty())) app.admin_operation(logging ? "Pause traffic logging" : "Enable traffic logging", {"traffic-logging", "--action", logging ? "disable" : "enable", "--app", app.app_id.value}, "monitoring");
  draw_text("Logging: " + app.app_id.value + (logging ? " | Active" : " | Paused") + (app.monitoring_auto ? " | Refreshing every 30s" : ""), x, 239, 10, kFaint);
  const std::vector<std::pair<std::string, std::string>> metrics{
    {"Requests", std::to_string(summary.value("requests", 0))}, {"Visitor IPs", std::to_string(summary.value("visitors", 0))}, {"Errors (4xx / 5xx)", std::to_string(summary.value("errors", 0))},
    {"Website bandwidth", bytes_label(summary.value("bytes", 0.0))}, {"Latency avg / p95", metric_number(summary.value("avg_ms", 0.0)) + " / " + metric_number(summary.value("p95_ms", 0.0)) + " ms"},
    {"CPU", app.host_cpu < 0 ? "Sampling..." : metric_number(app.host_cpu) + "%"}, {"Network RX / TX", bytes_label(app.network_rx_rate) + " / " + bytes_label(app.network_tx_rate) + "/s"},
    {"RAM used / total", bytes_label(host.value("memory_total", 0.0) - host.value("memory_available", 0.0)) + " / " + bytes_label(host.value("memory_total", 0.0))}};
  for (std::size_t index = 0; index < metrics.size(); ++index) {
    const float left = x + static_cast<float>(index % 4) * width / 4, y = 260 + static_cast<float>(index / 4) * 50;
    draw_text(metrics[index].first, left, y, 11, kFaint); draw_text(metrics[index].second, left, y + 18, 16, kText);
  }
  const float table_y = bottom - 199;
  const float section_y = table_y - 88;
  const float chart_height = std::max(20.0F, section_y - 395);
  const Rectangle graph{x, 375, width, chart_height};
  draw_text("Requests / minute", x, 355, 11, kMuted);
  DrawLineEx({graph.x, graph.y + graph.height}, {graph.x + graph.width, graph.y + graph.height}, 1, kBorder);
  const auto series = data.value("series", nlohmann::json::array());
  if (!series.empty()) {
    double maximum = 1; for (const auto& point : series) maximum = std::max(maximum, point.value("requests", 0.0));
    const double start = series.front().value("time", 0.0), end = std::max(start + 60, series.back().value("time", 0.0));
    Vector2 previous{}; bool has_previous = false;
    for (const auto& point : series) {
      const Vector2 current{graph.x + static_cast<float>((point.value("time", 0.0) - start) / (end - start)) * graph.width, graph.y + graph.height * (1 - static_cast<float>(point.value("requests", 0.0) / maximum))};
      if (has_previous) DrawLineEx(previous, current, 2, kAccent);
      DrawCircleV(current, 2, kAccent); previous = current; has_previous = true;
    }
    draw_text(metric_number(maximum, 0) + " peak / minute", x + width - 150, 355, 11, kFaint);
    draw_text(local_time_label(start), x, graph.y + graph.height + 5, 9, kFaint);
    draw_text(local_time_label(series.back().value("time", 0.0)), x + width - 60, graph.y + graph.height + 5, 9, kFaint);
  } else draw_text("No traffic recorded in this period.", x, graph.y + 8, 11, kMuted);
  const std::vector<std::string> sections{"Projects", "Pages", "Visitor IPs", "Devices", "Requests", "Status codes", "Host"};
  float left = x;
  for (int index = 0; index < static_cast<int>(sections.size()); ++index) {
    const float span = measure_text(sections[static_cast<std::size_t>(index)], 12).x + 26;
    if (button({left, section_y, span, 30}, sections[static_cast<std::size_t>(index)], true, app.monitoring_section == index ? ButtonStyle::primary : ButtonStyle::ghost)) { app.monitoring_section = index; app.monitoring_scroll = 0; app.monitoring_scroll_drag = false; } left += span + 7;
  }
  const auto before = app.traffic_filter.value;
  admin_field(app.traffic_filter, {x, section_y + 52, width, 30});
  if (before != app.traffic_filter.value) app.monitoring_scroll = 0;
  const auto table = monitoring_table(data, app.monitoring_section, app.traffic_filter.value);
  draw_admin_table(table, {x, table_y, width, 170}, app.monitoring_scroll, app.monitoring_scroll_drag, app.monitoring_scroll_grab, app.monitoring_section == 5 ? "Status codes" : app.monitoring_section == 6 ? "Current server readings" : "Latest first");
  draw_text(data.value("partial", false) ? "Partial report: scan limit reached. Export includes captured data." : "Origin traffic | Cloudflare cache hits excluded | Current and previous daily logs", x, bottom, 10, kFaint);
}
