#ifdef __PSV__

/*
 * The SceDownload IPMI calls below drive the system downloader through
 * SceShellSvc. The data layouts and call sequence come from bgvpk by SKGleba
 * (https://github.com/SKGleba/VitaTools, MIT License, Copyright (c) 2020
 * skgleba), based on RE by @dots_tb, @CelesteBlue123, @SilicaDevs and
 * @possvkey. Type 1 is the generic "multimedia" download (any file), as
 * opposed to the PKG types pkgj uses.
 */

#include "utils/psv_bgdl.hpp"
#include "utils/psv_bgdl_common.h"

#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <taihen.h>

#include <borealis/core/logger.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace psv_bgdl {

namespace {

constexpr int TYPE_MULTIMEDIA = 1;
// The system downloader keeps at most this many tasks.
constexpr size_t MAX_TASKS = 32;

constexpr uint32_t CMD_REGISTER_TASK = 0x12340012;
constexpr uint32_t CMD_START = 0x12340007;

struct DownloadParam {  // 0xDC0 bytes
    int32_t type[2];
    char unk_0x08[0x68];
    char url[0x800];
    char icon_path[0x100];
    char title[0x33A];
    char license_path[0x100];
    char unk_0xDAA[0x16];
};
static_assert(sizeof(DownloadParam) == 0xDC0, "DownloadParam layout");

struct IpmiParam {  // passed by value
    union {
        struct {
            void* ptr_to_dc0_ptr;
            void* ptr_to_2e0_ptr;
            uint32_t unk_1;  // 2
            uint32_t unk_2;  // -1
            uint32_t unk_3;  // 0
            DownloadParam* addr_dc0;
            uint32_t size_dc0;
        } init;
        struct {
            int32_t* result;
            uint32_t unk_2;  // 0
            uint32_t unk_3;  // 0
            uint32_t unk_4;  // 1
            uint32_t unk_5;  // 0
            uint32_t unk_6;
            uint32_t unk_7;  // 0x0A0A
        } state;
    };
    void* addr_2e0;
    uint32_t size_2e0;
    uint32_t unk_4;
    int32_t* bgdl_id;
    uint32_t unk_5;  // 4
    int32_t* result;
    uint32_t unk_4_2;
    uint32_t shell_func_8;
};
static_assert(sizeof(IpmiParam) == 0x3C, "IpmiParam layout");

struct ShellSvcInit {
    uint32_t unk_0;
    char name[0x10];
    void* unk_ptr;
    uint32_t unk_1;  // 1
    uint32_t size1;  // 0x1E00
    uint32_t size2;  // 0x1E00
    uint32_t unk_2;  // 1
    uint32_t unk_3;  // 0x0F00
    uint32_t unk_4;  // 0x0F00
    uint32_t unk_5;  // 1
    char padding[0x84];
    uint32_t unk_7;  // 2
    uint32_t unk_8;  // -1
    void* unk_ptr_2;
    char padding2[0x88];
};

struct ClassHeader {
    uint32_t unk0;
    uint32_t unk1;
    uint32_t unk2;
    uint32_t** func_table;
    uint32_t unk3;
    uint32_t* buf_c4;
    uint32_t* buf_1000;
};

using DownloadInitFn = int (*)(uint32_t** table, void* table_deref, int unk_1, ShellSvcInit* init, int unk_2);
using ChangeStateFn = int (*)(uint32_t** table, int cmd, void* ptr_to_dc0_ptr, int unk_1, IpmiParam param);
using IpmiGetSizeFn = int (*)(const char* name, int unk);
using IpmiInitFn = int (*)(uint32_t*** func_table, const char* name, ClassHeader* header, uint32_t* buf_1000);

std::mutex g_mutex;

// Diagnostics: mirrors bgdl log lines to BGDL_APP_LOG, since the app's own log
// only goes to stdout. API keys are masked.
std::string redact(std::string s) {
    for (const char* key : {"api_key=", "ApiKey="}) {
        size_t klen = strlen(key);
        for (size_t pos = 0; (pos = s.find(key, pos)) != std::string::npos;) {
            pos += klen;
            size_t end = s.find('&', pos);
            s.replace(pos, end == std::string::npos ? std::string::npos : end - pos, "***");
        }
    }
    return s;
}

template <typename... Args>
void blog(fmt::format_string<Args...> f, Args&&... args) {
    std::string line = redact(fmt::format(f, std::forward<Args>(args)...));
    brls::Logger::info("bgdl: {}", line);
    FILE* fp = fopen(BGDL_APP_LOG, "a");
    if (!fp) return;
    char ts[32];
    time_t now = time(nullptr);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(fp, "%s %s\n", ts, line.c_str());
    fclose(fp);
}
bool g_connected = false;
bool g_plugin = false;
std::string g_icon;
ShellSvcInit g_init;
ClassHeader* g_header = nullptr;
ChangeStateFn g_change_state = nullptr;

std::string taskDir(int id) {
    char buf[64];
    snprintf(buf, sizeof(buf), BGDL_TASK_DIR, static_cast<unsigned int>(id));
    return buf;
}

std::string titleId() {
    char buf[16] = {};
    if (sceAppMgrAppParamGetString(0, 12, buf, sizeof(buf)) < 0) return "SWITCHFIN";
    return buf;
}

SceUID shellPid() {
    SceUID pid = -1;
    if (sceAppMgrGetIdByName(&pid, "NPXS19999") < 0) return -1;
    return pid;
}

bool pluginActive(SceUID shell) {
    FILE* f = fopen(BGDL_PID_FILE, "r");
    if (!f) return false;
    unsigned int pid = 0;
    bool ok = fscanf(f, "%x", &pid) == 1;
    fclose(f);
    return ok && static_cast<SceUID>(pid) == shell;
}

bool loadPlugin() {
    SceUID shell = shellPid();
    if (shell < 0) {
        blog("SceShell not found");
        return false;
    }
    std::string path = "ux0:app/" + titleId() + "/module/" BGDL_PLUGIN_FILE;
    SceUID res = taiLoadStartModuleForPid(shell, path.c_str(), 0, nullptr, 0);
    // A copy that is already running answers with NO_RESIDENT after refreshing
    // the pid file, so check the file rather than the return value.
    bool active = pluginActive(shell);
    blog("load {} -> {:#x}, active={}", path, static_cast<uint32_t>(res), active);
    return active;
}

bool connect() {
    sceKernelLoadStartModule("vs0:sys/external/libshellsvc.suprx", 0, nullptr, 0, nullptr, nullptr);

    IpmiGetSizeFn get_size = nullptr;
    IpmiInitFn ipmi_init = nullptr;
    if (taiGetModuleExportFunc("SceShellSvc", 0xF4E34EDB, 0x4E255C31, reinterpret_cast<uintptr_t*>(&get_size)) < 0 ||
        taiGetModuleExportFunc("SceShellSvc", 0xF4E34EDB, 0xB282B430, reinterpret_cast<uintptr_t*>(&ipmi_init)) < 0) {
        blog("SceShellSvc exports not found");
        return false;
    }

    memset(&g_init, 0, sizeof(g_init));
    strncpy(g_init.name, "SceDownload", sizeof(g_init.name));
    g_init.unk_1 = 1;
    g_init.size1 = 0x1E00;
    g_init.size2 = 0x1E00;
    g_init.unk_2 = 1;
    g_init.unk_3 = 0x0F00;
    g_init.unk_4 = 0x0F00;
    g_init.unk_5 = 1;
    g_init.unk_7 = 2;
    g_init.unk_8 = static_cast<uint32_t>(-1);

    int size = get_size(g_init.name, 0x1E00);
    if (size != 0xC4) {
        blog("IPMI size {:#x}", static_cast<uint32_t>(size));
        return false;
    }

    g_header = static_cast<ClassHeader*>(calloc(1, sizeof(ClassHeader)));
    g_header->buf_c4 = static_cast<uint32_t*>(calloc(1, size));
    g_header->buf_1000 = static_cast<uint32_t*>(calloc(1, 0x1000));

    int res = ipmi_init(&g_header->func_table, g_init.name, g_header, g_header->buf_1000);
    if (res != 0 || !g_header->func_table) {
        blog("IPMI init {:#x}", static_cast<uint32_t>(res));
        return false;
    }

    auto download_init = reinterpret_cast<DownloadInitFn>((*g_header->func_table)[1]);
    g_change_state = reinterpret_cast<ChangeStateFn>((*g_header->func_table)[5]);

    res = download_init(g_header->func_table, *g_header->func_table, 0x14, &g_init, 2);
    if (res != 0) {
        blog("SceDownload init {:#x}", static_cast<uint32_t>(res));
        return false;
    }
    return true;
}

size_t taskCount() {
    SceUID dfd = sceIoDopen("ux0:bgdl/t");
    if (dfd < 0) return 0;
    size_t count = 0;
    SceIoDirent ent;
    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(dfd, &ent) > 0) {
        if (SCE_S_ISDIR(ent.d_stat.st_mode)) count++;
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(dfd);
    return count;
}

int registerTask(const std::string& title, const std::string& url) {
    std::vector<uint8_t> dc0(sizeof(DownloadParam));
    std::vector<uint8_t> buf_2e0(0x2E0);
    int32_t result = 0;
    int32_t id = -1;

    auto* param = reinterpret_cast<DownloadParam*>(dc0.data());
    param->type[0] = param->type[1] = TYPE_MULTIMEDIA;
    strncpy(param->url, url.c_str(), sizeof(param->url) - 1);
    strncpy(param->title, title.c_str(), sizeof(param->title) - 1);
    strncpy(param->icon_path, g_icon.c_str(), sizeof(param->icon_path) - 1);

    IpmiParam p;
    memset(&p, 0, sizeof(p));
    p.init.ptr_to_dc0_ptr = &p.init.addr_dc0;
    p.init.ptr_to_2e0_ptr = &p.addr_2e0;
    p.init.unk_1 = 2;
    p.init.unk_2 = static_cast<uint32_t>(-1);
    p.init.addr_dc0 = param;
    p.init.size_dc0 = sizeof(DownloadParam);
    p.addr_2e0 = buf_2e0.data();
    p.size_2e0 = static_cast<uint32_t>(buf_2e0.size());
    p.bgdl_id = &id;
    p.unk_5 = 4;
    p.result = &result;
    p.shell_func_8 = (*g_header->func_table)[8];

    int res = g_change_state(g_header->func_table, CMD_REGISTER_TASK, p.init.ptr_to_dc0_ptr, 1, p);
    // On a rejection the service writes its error code over the DownloadParam
    // pointer (and into the reply at +4) and sets the id to 0.
    uint32_t dc0ret = reinterpret_cast<uintptr_t>(p.init.addr_dc0);
    uint32_t reply[4];
    memcpy(reply, buf_2e0.data(), sizeof(reply));
    blog("register: res={:#010x} result={:#010x} id={} dc0ret={:#010x} reply={:08x} {:08x} {:08x} {:08x} url={}",
        static_cast<uint32_t>(res), static_cast<uint32_t>(result), id, dc0ret, reply[0], reply[1], reply[2], reply[3],
        url);
    if (res < 0) return res;
    if (result < 0) return result;
    if (p.init.addr_dc0 != param && (dc0ret & 0x80000000u)) return static_cast<int>(dc0ret);
    if (static_cast<int32_t>(reply[1]) < 0) return static_cast<int32_t>(reply[1]);
    if (id < 0) return -1;

    // Start the downloader's queue.
    result = 0;
    memset(&p, 0, sizeof(p));
    p.state.result = &result;
    p.state.unk_4 = 1;
    p.state.unk_7 = 0x0A0A;
    res = g_change_state(g_header->func_table, CMD_START, nullptr, 0, p);
    if (res < 0) blog("start {:#x}", static_cast<uint32_t>(res));
    else if (result < 0) blog("start result {:#x}", static_cast<uint32_t>(result));

    return id;
}

bool writeMarker(int id, const std::string& dest) {
    std::string path = taskDir(id) + "/" BGDL_MARKER_NAME;
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    bool ok = fwrite(dest.data(), 1, dest.size(), f) == dest.size();
    return fclose(f) == 0 && ok;
}

}  // namespace

bool init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_connected) return g_plugin;

    g_plugin = loadPlugin();
    if (g_plugin) g_connected = connect();
    if (!g_connected) g_plugin = false;
    g_icon = "ux0:app/" + titleId() + "/sce_sys/icon0.png";
    blog("init: available={} connected={}", g_plugin, g_connected);
    return g_plugin;
}

bool available() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_connected && g_plugin;
}

int queue(const std::string& title, const std::string& url, const std::string& dest) {
    if (url.size() >= sizeof(DownloadParam::url) || dest.size() >= BGDL_MARKER_MAX) return -1;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_connected || !g_plugin) return -1;
    if (taskCount() >= MAX_TASKS) {
        blog("the system download queue is full");
        return -1;
    }

    sceIoRemove(dest.c_str());
    int id = registerTask(title, url);
    if (id < 0) {
        blog("queue failed {:#x}", static_cast<uint32_t>(id));
        return id;
    }
    if (!writeMarker(id, dest)) {
        // Without the marker the plugin leaves the file in the task folder.
        blog("failed to write marker for task {:08x}", static_cast<uint32_t>(id));
        return -1;
    }
    blog("queued task {:08x} -> {}", static_cast<uint32_t>(id), dest);
    return id;
}

bool exists(int id) {
    SceIoStat st;
    return id >= 0 && sceIoGetstat(taskDir(id).c_str(), &st) >= 0;
}

int64_t downloadedBytes(int id) {
    std::string dir = taskDir(id);
    SceUID dfd = sceIoDopen(dir.c_str());
    if (dfd < 0) return -1;
    int64_t best = -1;
    SceIoDirent ent;
    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(dfd, &ent) > 0) {
        size_t n = strlen(ent.d_name);
        bool pdb = n >= 4 && strcmp(ent.d_name + n - 4, ".pdb") == 0;
        if (SCE_S_ISREG(ent.d_stat.st_mode) && !pdb && strcmp(ent.d_name, BGDL_MARKER_NAME) != 0)
            best = std::max<int64_t>(best, ent.d_stat.st_size);
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(dfd);
    return best;
}

void forget(int id) {
    if (id < 0) return;
    sceIoRemove((taskDir(id) + "/" BGDL_MARKER_NAME).c_str());
}

}  // namespace psv_bgdl

#endif  // __PSV__
