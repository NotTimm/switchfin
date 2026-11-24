#pragma once

#ifdef __PSV__

#include <cstdint>
#include <string>

/// Background downloads through the PS Vita system downloader.
///
/// A queued task shows up in the LiveArea notification tray and keeps running
/// while Switchfin is suspended or closed. The SceShell plugin
/// (app/platform/vita/bgdl_plugin) moves the finished file to the path given
/// when it was queued. Needs taiHEN; without it, or on an unsupported
/// firmware, available() is false and callers should download in-app.
namespace psv_bgdl {

/// Loads the SceShell plugin and connects to the system downloader.
/// Call once at startup. Returns available().
bool init();

/// True when the plugin is active in SceShell and the downloader is connected.
bool available();

/// Default for the setting: on when the system downloader can reach `server`,
/// that is plain http, or https with modern TLS from the vitals plugin.
bool defaultEnabled(const std::string& server);

/// Queues `url` with the system downloader. When it finishes, the file is
/// moved to `dest`. Returns the task id (>= 0) or a negative error code.
int queue(const std::string& title, const std::string& url, const std::string& dest);

/// True while the task still exists (downloading, waiting, or failed and
/// still listed in the notification tray).
bool exists(int id);

/// Bytes downloaded so far for a task, or -1 if unknown.
int64_t downloadedBytes(int id);

/// Stops the plugin from moving this task's file into Switchfin's folder.
/// The task itself can only be cancelled from the notification tray.
void forget(int id);

}  // namespace psv_bgdl

#endif
