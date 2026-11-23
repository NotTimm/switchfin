#pragma once

#ifdef __PSV__

#include <string>

/// Queue downloads with the PS Vita system download service (the one the PS
/// Store uses), so they show in the status bar and keep running after the app
/// is closed. Experimental.
namespace VitaBgdl {

/// Queue `url` under `title`. Returns the system download id (>= 0), or -1
/// and sets `error`.
int queue(const std::string& title, const std::string& url, std::string& error);

}  // namespace VitaBgdl

#endif  // __PSV__
