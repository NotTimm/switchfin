#pragma once

// Shared between Switchfin and its SceShell plugin (app/platform/vita/bgdl_plugin).

// Folder the system downloader uses for each task, by task id.
#define BGDL_TASK_DIR "ux0:bgdl/t/%08x"
// Marker Switchfin writes into a task folder. It holds the destination path for
// the finished file; tasks without it keep the stock behaviour.
#define BGDL_MARKER_NAME "switchfin.dst"
#define BGDL_MARKER_MAX 256
// Module name of the plugin (exports.yml).
#define BGDL_PLUGIN_MODULE "SwitchfinBgdl"
// File name of the plugin inside the app's module/ folder.
#define BGDL_PLUGIN_FILE "switchfin_bgdl.suprx"
// The plugin writes the SceShell process id here once its hooks are in place.
#define BGDL_PID_FILE "ux0:data/switchfin_bgdl.pid"
