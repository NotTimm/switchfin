#pragma once

#ifdef __PSV__

#include <string>
#include <utility>
#include <vector>

/// Queue downloads with the PS Vita system download service (the one the PS
/// Store uses), so they show in the status bar and keep running after the app
/// is closed. Experimental.
namespace VitaBgdl {

/// Queue `url` under `title`. Returns the system download id (>= 0), or -1
/// and sets `error`.
int queue(const std::string& title, const std::string& url, std::string& error);

/// Diagnostics, written to ux0:data/Switchfin/bgdl_log.txt (token masked)
void setRedact(const std::string& token);
void log(const std::string& line);
void probe(const std::string& url);
void logState(const std::string& why);

/// Try each (label, url) until the system creates a job for one; logs
/// everything. Blocks for up to a few minutes, call it off the UI thread.
std::string experiment(const std::string& title, const std::vector<std::pair<std::string, std::string>>& variants);

}  // namespace VitaBgdl

#endif  // __PSV__
