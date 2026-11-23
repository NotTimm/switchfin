// Queues downloads with the PS Vita system download service (SceDownload,
// reached through SceShellSvc's IPMI interface).
//
// Adapted from pkgj's bgdl.cpp (https://github.com/blastrock/pkgj):
//   Copyright 2018-2019 Philippe Daouadi
//   Copyright 2019-2020 Asakura Reiko
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are
//   met: 1. Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//   2. Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
//   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.
// Interface reverse engineered by @dots_tb with help from @CelesteBlue123,
// @SilicaDevs, @possvkey and the NPS Team.

#ifdef __PSV__

#include "utils/vita_bgdl.hpp"

#include <borealis/core/logger.hpp>
#include <fmt/format.h>
#include <curl/curl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <taihen.h>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <memory>
#include <mutex>
#include <vector>

namespace {

// Download type. pkgj uses 0x16 (game) etc. for PKGs; 1 is a generic
// (media) download, as used by bgvpk.
constexpr int BGDL_TYPE_GENERIC = 1;

struct ipmi_download_param {
    int type[2];
    char unk_0x08[0x68];
    char url[0x800];
    char icon_path[0x100];
    char title[0x33A];
    char license_path[0x100];
    char unk_0xDAA[0x16];
};
static_assert(sizeof(ipmi_download_param) == 0xDC0, "DC0 layout");

struct sce_ipmi_download_param {
    union {
        struct {
            uint32_t* ptr_to_dc0_ptr;
            uint32_t* ptr_to_2e0_ptr;
            uint32_t unk_1;  // 2
            uint32_t unk_2;  // -1
            uint32_t unk_3;  // 0
            ipmi_download_param* addr_DC0;
            uint32_t sizeDC0;
        } init;
        struct {
            int32_t* result;
            uint32_t unk_2;  // 0
            uint32_t unk_3;  // 0
            uint32_t unk_4;  // 1
            uint32_t unk_5;  // 0
            uint32_t unk_6;
            uint32_t unk_7;  // 0x00000A0A
        } state;
    };
    void* addr_2E0;
    uint32_t size2E0;
    uint32_t unk_4;
    uint32_t* pBgdlId;
    uint32_t unk_5;  // 4
    int32_t* result;
    uint32_t unk_4_2;
    uint32_t shell_func_8;
};

struct shellsvc_init_struct {
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

struct scedownload_class_header {
    uint32_t unk0;
    uint32_t unk1;
    uint32_t unk2;
    uint32_t** func_table;
    uint32_t unk3;
    uint32_t* bufC4;
    uint32_t* buf10000;
};

typedef int (*SceDownloadInit)(uint32_t** ipmi_sce_download_ptr, void* ipmi_sce_download_ptr_deref, int unk_1,
    shellsvc_init_struct* bufc8, int unk_2);
typedef int (*SceDownloadChangeState)(
    uint32_t** ipmi_sce_download_ptr, int cmd, void* ptr_to_dc0_ptr, int unk_1, sce_ipmi_download_param r5);

struct scedownload_class {
    shellsvc_init_struct init_header;
    scedownload_class_header* class_header;
    SceDownloadInit init;
    SceDownloadChangeState change_state;
};

int (*SceIpmi_4E255C31)(const char* name, int unk) = nullptr;
int (*SceIpmi_B282B430)(
    uint32_t*** func_table, const char* name, scedownload_class_header* class_header, uint32_t* buf10000) = nullptr;

/// Connects to SceDownload once; the connection lives for the whole process.
scedownload_class* getService(std::string& error) {
    static std::mutex mutex;
    static scedownload_class* service = nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (service) return service;

    sceKernelLoadStartModule("vs0:sys/external/libshellsvc.suprx", 0, nullptr, 0, nullptr, nullptr);
    if (taiGetModuleExportFunc("SceShellSvc", 0xF4E34EDB, 0x4E255C31, (uintptr_t*)&SceIpmi_4E255C31) < 0 ||
        taiGetModuleExportFunc("SceShellSvc", 0xF4E34EDB, 0xB282B430, (uintptr_t*)&SceIpmi_B282B430) < 0) {
        error = "SceShellSvc exports not found";
        return nullptr;
    }

    auto obj = new scedownload_class();
    std::memset(obj, 0, sizeof(*obj));
    std::strncpy(obj->init_header.name, "SceDownload", sizeof(obj->init_header.name));
    obj->init_header.unk_1 = 1;
    obj->init_header.size1 = 0x1E00;
    obj->init_header.size2 = 0x1E00;
    obj->init_header.unk_2 = 1;
    obj->init_header.unk_3 = 0x0F00;
    obj->init_header.unk_4 = 0x0F00;
    obj->init_header.unk_5 = 1;
    obj->init_header.unk_7 = 2;
    obj->init_header.unk_8 = -1;

    int res = SceIpmi_4E255C31(obj->init_header.name, 0x1E00);
    if (res != 0xc4) {
        error = fmt::format("SceIpmi create failed: {:#010x}", (uint32_t)res);
        delete obj;
        return nullptr;
    }
    obj->class_header = (scedownload_class_header*)new char[0x18]();
    obj->class_header->bufC4 = (uint32_t*)new char[res]();
    obj->class_header->buf10000 = (uint32_t*)new char[0x1000]();

    res = SceIpmi_B282B430(
        &obj->class_header->func_table, obj->init_header.name, obj->class_header, obj->class_header->buf10000);
    if (res != 0) {
        error = fmt::format("SceIpmi init failed: {:#010x}", (uint32_t)res);
        return nullptr;  // buffers are leaked on purpose: the IPMI side may still reference them
    }
    obj->init = (SceDownloadInit)(*obj->class_header->func_table)[1];
    obj->change_state = (SceDownloadChangeState)(*obj->class_header->func_table)[5];

    res = obj->init(obj->class_header->func_table, *obj->class_header->func_table, 0x14, &obj->init_header, 2);
    if (res != 0) {
        error = fmt::format("SceDownload init failed: {:#010x}", (uint32_t)res);
        return nullptr;
    }
    service = obj;
    return service;
}

}  // namespace

namespace VitaBgdl {

static std::string redactToken;
static std::mutex logMutex;

void setRedact(const std::string& token) { redactToken = token; }

void log(const std::string& line) {
    std::string out = line;
    if (!redactToken.empty())
        for (size_t p; (p = out.find(redactToken)) != std::string::npos;) out.replace(p, redactToken.size(), "***");
    std::lock_guard<std::mutex> lock(logMutex);
    std::ofstream f("ux0:data/Switchfin/bgdl_log.txt", std::ios::app);
    char ts[32];
    std::time_t now = std::time(nullptr);
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::gmtime(&now));
    f << ts << " " << out << "\n";
    brls::Logger::info("VitaBgdl: {}", out);
}

static std::string hexdump(const uint8_t* data, size_t len) {
    std::string out;
    for (size_t i = 0; i < len; i += 16) {
        std::string hex, txt;
        for (size_t j = i; j < i + 16 && j < len; j++) {
            hex += fmt::format("{:02x} ", data[j]);
            txt += (data[j] >= 0x20 && data[j] < 0x7f) ? (char)data[j] : '.';
        }
        out += fmt::format("\n    {:04x}: {:<48} {}", i, hex, txt);
    }
    return out;
}

static bool allZero(const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (d[i]) return false;
    return true;
}

static size_t headerCb(char* buf, size_t size, size_t n, void* user) {
    auto* out = static_cast<std::string*>(user);
    std::string line(buf, size * n);
    std::string lower = line;
    for (auto& c : lower) c = std::tolower((unsigned char)c);
    if (lower.rfind("http/", 0) == 0 || lower.rfind("content-", 0) == 0 || lower.rfind("accept-ranges", 0) == 0 ||
        lower.rfind("allow", 0) == 0 || lower.rfind("location", 0) == 0 || lower.rfind("transfer-encoding", 0) == 0) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        *out += "\n    " + line;
    }
    return size * n;
}

void probe(const std::string& url) {
    for (int head = 1; head >= 0; head--) {
        CURL* c = curl_easy_init();
        if (!c) return;
        std::string headers;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, headerCb);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &headers);
        if (head) {
            curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        } else {
            // first byte only, to compare with HEAD
            curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](char*, size_t s, size_t n, void*) { return s * n; });
        }
        CURLcode rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        log(fmt::format("  probe {}: curl={} ({}) http={}{}", head ? "HEAD" : "GET range 0-0", (int)rc,
            curl_easy_strerror(rc), code, headers));
        curl_easy_cleanup(c);
    }
}

static std::set<std::string> listDir(const std::string& dir) {
    std::set<std::string> out;
    SceUID d = sceIoDopen(dir.c_str());
    if (d < 0) return out;
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0) out.insert(e.d_name);
    sceIoDclose(d);
    return out;
}

static void logJob(const std::string& name) {
    std::string dir = "ux0:bgdl/t/" + name;
    log("  job " + dir + ":");
    for (auto& f : listDir(dir)) {
        SceIoStat st;
        std::memset(&st, 0, sizeof(st));
        sceIoGetstat((dir + "/" + f).c_str(), &st);
        log(fmt::format("    {} ({} bytes)", f, (long long)st.st_size));
    }
    SceUID fd = sceIoOpen((dir + "/d0.pdb").c_str(), SCE_O_RDONLY, 0);
    if (fd >= 0) {
        std::vector<uint8_t> buf(0x400);
        int n = sceIoRead(fd, buf.data(), buf.size());
        sceIoClose(fd);
        if (n > 0) log("    d0.pdb:" + hexdump(buf.data(), n));
    }
}

void logState(const std::string& why) {
    log("=== state (" + why + ")");
    auto jobs = listDir("ux0:bgdl/t");
    log(fmt::format("  ux0:bgdl/t: {} job(s)", jobs.size()));
    for (auto& j : jobs) logJob(j);
    std::string videos;
    for (auto& v : listDir("ux0:video")) videos += " [" + v + "]";
    log("  ux0:video:" + (videos.empty() ? std::string(" (empty)") : videos));
}

std::string experiment(const std::string& title, const std::vector<std::pair<std::string, std::string>>& variants) {
    log("##### bgdl experiment: " + title);
    logState("before");
    for (auto& [label, url] : variants) {
        log("--- variant " + label + ": " + url);
        probe(url);
        auto before = listDir("ux0:bgdl/t");
        std::string error;
        int id = queue(title, url, error);
        if (id < 0) {
            log("  queue error: " + error);
            continue;
        }
        std::string job;
        for (int i = 0; i < 20 && job.empty(); i++) {
            sceKernelDelayThread(500 * 1000);
            for (auto& d : listDir("ux0:bgdl/t"))
                if (!before.count(d)) job = d;
        }
        if (job.empty()) {
            log("  no job folder appeared within 10s -> dropped");
            continue;
        }
        log("  job folder appeared: " + job);
        for (int i = 0; i < 12; i++) {
            logJob(job);
            sceKernelDelayThread(10 * 1000 * 1000);
            if (!listDir("ux0:bgdl/t").count(job)) {
                log("  job folder gone (finished, exported or cancelled)");
                break;
            }
        }
        logState("after " + label);
        return fmt::format("bgdl test: {} accepted (job {}), see bgdl_log.txt", label, job);
    }
    logState("end, nothing accepted");
    return "bgdl test: no variant accepted, see bgdl_log.txt";
}

int queue(const std::string& title, const std::string& url, std::string& error) {
    if (url.size() >= sizeof(ipmi_download_param::url)) {
        error = "URL too long for system download";
        return -1;
    }
    sceIoMkdir("ux0:bgdl", 0777);

    scedownload_class* svc = getService(error);
    if (!svc) {
        log("  service init failed: " + error);
        return -1;
    }

    int32_t result = 0;
    int32_t bgdlid = 1;  // as in pkgj; the service overwrites it
    std::vector<uint8_t> buf_dc0(0xDC0), buf_2e0(0x2E0);

    sce_ipmi_download_param params;
    std::memset(&params, 0, sizeof(params));
    params.init.ptr_to_dc0_ptr = (uint32_t*)&params.init.addr_DC0;
    params.init.ptr_to_2e0_ptr = (uint32_t*)&params.addr_2E0;
    params.init.unk_1 = 2;
    params.init.unk_2 = -1;
    params.init.unk_3 = 0;
    params.init.sizeDC0 = buf_dc0.size();
    params.init.addr_DC0 = (ipmi_download_param*)buf_dc0.data();
    params.size2E0 = buf_2e0.size();
    params.addr_2E0 = buf_2e0.data();
    params.pBgdlId = (uint32_t*)&bgdlid;
    params.unk_5 = 4;
    params.result = &result;
    params.shell_func_8 = (*svc->class_header->func_table)[8];

    auto* dc0 = params.init.addr_DC0;
    std::strncpy(dc0->url, url.c_str(), sizeof(dc0->url) - 1);
    std::strncpy(dc0->title, title.c_str(), sizeof(dc0->title) - 1);
    dc0->type[0] = dc0->type[1] = BGDL_TYPE_GENERIC;

    int res = svc->change_state(svc->class_header->func_table, 0x12340012, params.init.ptr_to_dc0_ptr, 1, params);
    log(fmt::format("  change_state(queue): res={:#010x} result={:#010x} id={} dc0ptr={}", (uint32_t)res,
        (uint32_t)result, bgdlid, (void*)params.init.addr_DC0));
    if (!allZero(buf_2e0.data(), buf_2e0.size())) log("  2E0 reply:" + hexdump(buf_2e0.data(), buf_2e0.size()));
    if (res < 0 || result < 0 || bgdlid < 0) {
        error = fmt::format(
            "SceDownload queue failed: res={:#010x} result={:#010x} id={}", (uint32_t)res, (uint32_t)result, bgdlid);
        return -1;
    }

    // start the queued download
    result = 0;
    std::memset(&params, 0, sizeof(params));
    params.state.result = &result;
    params.state.unk_4 = 1;
    params.state.unk_7 = 0x00000A0A;
    res = svc->change_state(svc->class_header->func_table, 0x12340007, nullptr, 0, params);
    log(fmt::format("  change_state(start): res={:#010x} result={:#010x}", (uint32_t)res, (uint32_t)result));
    if (res < 0 || result < 0) {
        error = fmt::format("SceDownload start failed: res={:#010x} result={:#010x}", (uint32_t)res, (uint32_t)result);
        return -1;
    }

    brls::Logger::info("VitaBgdl: queued #{} {}", bgdlid, title);
    return bgdlid;
}

}  // namespace VitaBgdl

#endif  // __PSV__
