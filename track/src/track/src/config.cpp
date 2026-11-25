#include "config.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>

// 去除字符串首尾空白
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool parse_config(const std::string& path, TrackConfig& cfg) {
    // 默认值
    cfg.log_dir = "/var/log/track";
    cfg.log_size_kb = 1024;
    cfg.log_count = 5;
    cfg.check_interval = 2;
    cfg.heartbeat_interval = 60;
    cfg.startup_grace = 15;
    cfg.rapid_death_window = 30;
    cfg.circuit_break_threshold = 3;
    // CPU 监控默认值
    cfg.cpu_monitor_enable = 1;
    cfg.cpu_log_interval = 300;
    cfg.cpu_total_threshold = 90;
    cfg.cpu_alert_duration = 3;
    cfg.cpu_snapshot_on_alert = 1;
    cfg.cpu_snapshot_cooldown = 60;
    cfg.cpu_top_n = 5;
    // 内存监控默认值
    cfg.mem_monitor_enable = 1;
    cfg.mem_log_interval = 300;
    cfg.mem_available_threshold_mb = 40;
    cfg.mem_alert_duration = 5;
    cfg.mem_snapshot_on_alert = 1;
    cfg.mem_snapshot_cooldown = 60;
    cfg.mem_top_n = 5;
    // Flash 监控默认值
    cfg.flash_monitor_enable = 1;
    cfg.flash_log_interval = 300;
    cfg.flash_paths = "/userdata /oem";
    cfg.flash_used_pct_threshold = 90;
    cfg.flash_alert_duration = 2;
    cfg.flash_snapshot_on_alert = 1;
    cfg.flash_snapshot_cooldown = 60;
    cfg.flash_top_n = 5;
    cfg.processes.clear();

    std::ifstream f(path);
    if (!f.is_open()) {
        return false;
    }

    std::string line;
    std::string current_section;
    ProcessConfig* cur_proc = nullptr;

    while (std::getline(f, line)) {
        // 去掉注释
        size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) continue;

        // section: [process:xxx]
        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.size() - 2);
            if (current_section.substr(0, 8) == "process:") {
                ProcessConfig pc;
                pc.name = current_section.substr(8);
                pc.last_pid = 0;
                pc.crash_count = 0;
                pc.last_restart_ms = 0;
                pc.last_death_ms = 0;
                pc.restart_delay = 1;
                cfg.processes.push_back(pc);
                cur_proc = &cfg.processes.back();
            } else {
                cur_proc = nullptr;
            }
            continue;
        }

        // key=value
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (cur_proc) {
            if (key == "pidfile")        cur_proc->pidfile = val;
            else if (key == "comm")      cur_proc->comm = val;
            else if (key == "restart_cmd") cur_proc->restart_cmd = val;
            else if (key == "restart_delay") cur_proc->restart_delay = atoi(val.c_str());
        } else {
            if (key == "log_dir")              cfg.log_dir = val;
            else if (key == "log_size")        cfg.log_size_kb = atoi(val.c_str());
            else if (key == "log_count")       cfg.log_count = atoi(val.c_str());
            else if (key == "check_interval")  cfg.check_interval = atoi(val.c_str());
            else if (key == "heartbeat_interval") cfg.heartbeat_interval = atoi(val.c_str());
            else if (key == "startup_grace")      cfg.startup_grace = atoi(val.c_str());
            else if (key == "rapid_death_window") cfg.rapid_death_window = atoi(val.c_str());
            else if (key == "circuit_break_threshold") cfg.circuit_break_threshold = atoi(val.c_str());
            else if (key == "cpu_monitor_enable")     cfg.cpu_monitor_enable = atoi(val.c_str());
            else if (key == "cpu_log_interval")       cfg.cpu_log_interval = atoi(val.c_str());
            else if (key == "cpu_total_threshold")    cfg.cpu_total_threshold = atoi(val.c_str());
            else if (key == "cpu_alert_duration")     cfg.cpu_alert_duration = atoi(val.c_str());
            else if (key == "cpu_snapshot_on_alert")  cfg.cpu_snapshot_on_alert = atoi(val.c_str());
            else if (key == "cpu_snapshot_cooldown")  cfg.cpu_snapshot_cooldown = atoi(val.c_str());
            else if (key == "cpu_top_n")              cfg.cpu_top_n = atoi(val.c_str());
            else if (key == "mem_monitor_enable")         cfg.mem_monitor_enable = atoi(val.c_str());
            else if (key == "mem_log_interval")           cfg.mem_log_interval = atoi(val.c_str());
            else if (key == "mem_available_threshold_mb") cfg.mem_available_threshold_mb = atoi(val.c_str());
            else if (key == "mem_alert_duration")         cfg.mem_alert_duration = atoi(val.c_str());
            else if (key == "mem_snapshot_on_alert")      cfg.mem_snapshot_on_alert = atoi(val.c_str());
            else if (key == "mem_snapshot_cooldown")      cfg.mem_snapshot_cooldown = atoi(val.c_str());
            else if (key == "mem_top_n")                  cfg.mem_top_n = atoi(val.c_str());
            else if (key == "flash_monitor_enable")        cfg.flash_monitor_enable = atoi(val.c_str());
            else if (key == "flash_log_interval")          cfg.flash_log_interval = atoi(val.c_str());
            else if (key == "flash_paths")                 cfg.flash_paths = val;
            else if (key == "flash_used_pct_threshold")    cfg.flash_used_pct_threshold = atoi(val.c_str());
            else if (key == "flash_alert_duration")        cfg.flash_alert_duration = atoi(val.c_str());
            else if (key == "flash_snapshot_on_alert")     cfg.flash_snapshot_on_alert = atoi(val.c_str());
            else if (key == "flash_snapshot_cooldown")     cfg.flash_snapshot_cooldown = atoi(val.c_str());
            else if (key == "flash_top_n")                 cfg.flash_top_n = atoi(val.c_str());
        }
    }

    f.close();
    return true;
}
