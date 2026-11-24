/*
 * switchfin_bgdl: SceShell plugin for Switchfin background downloads.
 *
 * Switchfin queues videos with the system downloader (the same one the
 * LiveArea notification tray shows). Stock firmware refuses file types it
 * does not know and has nowhere to put a finished video, so this plugin, which
 * Switchfin loads into SceShell, does three things:
 *
 *  1. Patches the downloader's "HEAD" request into "GET". Jellyfin's
 *     /Items/{id}/Download route only answers GET, so the HEAD probe fails.
 *  2. Lets the downloader accept MIME types it does not recognise.
 *  3. When a download with a Switchfin marker file finishes, moves the file to
 *     the path in the marker instead of running the stock export.
 *
 * Downloads without a marker keep the stock behaviour.
 *
 * The SceShell offsets and the HEAD -> GET patch location come from bgvpk by
 * SKGleba (https://github.com/SKGleba/VitaTools, MIT License,
 * Copyright (c) 2020 skgleba).
 */

#include <stdarg.h>

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <taihen.h>

#include "utils/psv_bgdl_common.h"

// Returned by the stock MIME lookup for an unknown type.
#define SCE_SHELL_ERROR_UNKNOWN_MIME ((int)0x80103A21)

typedef struct ShellOffsets {
    uint32_t head;    // "HEAD" string used by the downloader's probe request
    uint32_t exp;     // export of a finished download
    uint32_t mime;    // MIME type -> file type lookup
} ShellOffsets;

static SceUID hook_ids[3] = {-1, -1, -1};
static tai_hook_ref_t export_ref;
static tai_hook_ref_t mime_ref;

static int get_shell_offsets(uint32_t nid, ShellOffsets *o) {
    switch (nid) {
    case 0x0552F692:  // 3.60 retail
        *o = (ShellOffsets){0x50A4A8, 0x1163F6, 0x11B5E4};
        return 0;
    case 0x6CB01295:  // 3.60 PDEL
        *o = (ShellOffsets){0x4F9A18, 0x111D5A, 0x116F48};
        return 0;
    case 0xEAB89D5C:  // 3.60 PTEL
        *o = (ShellOffsets){0x4FEBF8, 0x112756, 0x117944};
        return 0;
    case 0x5549BF1F:  // 3.65 retail
    case 0x34B4D82E:  // 3.67 retail
    case 0x12DAC0F3:  // 3.68 retail
        *o = (ShellOffsets){0x50A9E8, 0x11644E, 0x11B63C};
        return 0;
    case 0x0703C828:  // 3.69 retail
    case 0x2053B5A5:  // 3.70 retail
    case 0xF476E785:  // 3.71 retail
    case 0x939FFBE9:  // 3.72 retail
    case 0x734D476A:  // 3.73 retail
    case 0x51CB6207:  // 3.74 retail (same SceShell layout as 3.73; HEAD is verified below)
        *o = (ShellOffsets){0x50AA28, 0x11644E, 0x11B63C};
        return 0;
    case 0xE6A02F2B:  // 3.65 PDEL
        *o = (ShellOffsets){0x4F9F58, 0x111DB2, 0x116FA0};
        return 0;
    case 0x587F9CED:  // 3.65 PTEL
        *o = (ShellOffsets){0x4FF0F8, 0x1127AE, 0x11799C};
        return 0;
    default:
        return -1;
    }
}

// Diagnostics: one line per event in BGDL_PLUGIN_LOG.
static void plog(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = sceClibVsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (len < 0) return;
    if (len > (int)sizeof(buf) - 2) len = sizeof(buf) - 2;
    buf[len++] = '\n';
    SceUID fd = sceIoOpen(BGDL_PLUGIN_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return;
    sceIoWrite(fd, buf, len);
    sceIoClose(fd);
}

static int has_suffix(const char *name, const char *suffix) {
    SceSize n = sceClibStrnlen(name, 256);
    SceSize s = sceClibStrnlen(suffix, 16);
    return n >= s && sceClibStrncmp(name + n - s, suffix, s) == 0;
}

// Reads the destination path Switchfin left for this download. Returns 0 when
// the download is a Switchfin one.
static int read_marker(uint32_t id, char *dst, SceSize size) {
    char path[128];
    sceClibSnprintf(path, sizeof(path), BGDL_TASK_DIR "/" BGDL_MARKER_NAME, (unsigned int)id);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return fd;
    int len = sceIoRead(fd, dst, size - 1);
    sceIoClose(fd);
    if (len <= 0) return -1;
    dst[len] = '\0';
    while (len > 0 && (dst[len - 1] == '\n' || dst[len - 1] == '\r' || dst[len - 1] == ' ')) dst[--len] = '\0';
    return len > 0 ? 0 : -1;
}

// The downloaded file is the largest file in the task folder that is not one of
// the downloader's own .pdb files or our marker.
static int find_payload(uint32_t id, char *out, SceSize size) {
    char dir[64];
    sceClibSnprintf(dir, sizeof(dir), BGDL_TASK_DIR, (unsigned int)id);
    SceUID dfd = sceIoDopen(dir);
    if (dfd < 0) return dfd;

    SceIoDirent ent;
    SceOff best = -1;
    sceClibMemset(&ent, 0, sizeof(ent));
    while (sceIoDread(dfd, &ent) > 0) {
        if (SCE_S_ISREG(ent.d_stat.st_mode) && !has_suffix(ent.d_name, ".pdb") &&
            sceClibStrncmp(ent.d_name, BGDL_MARKER_NAME, sizeof(BGDL_MARKER_NAME)) != 0 &&
            ent.d_stat.st_size > best) {
            best = ent.d_stat.st_size;
            sceClibSnprintf(out, size, "%s/%s", dir, ent.d_name);
        }
        sceClibMemset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(dfd);
    return best >= 0 ? 0 : -1;
}

static int export_patched(uint32_t *data) {
    uint32_t id = *(uint32_t *)data[0];
    char dst[BGDL_MARKER_MAX];
    char src[320];

    if (read_marker(id, dst, sizeof(dst)) < 0) {
        plog("export %08x: no marker, stock export", (unsigned int)id);
        return TAI_CONTINUE(int, export_ref, data);
    }

    int res = find_payload(id, src, sizeof(src));
    if (res < 0) {
        plog("export %08x: no payload %08x", (unsigned int)id, (unsigned int)res);
        return res;
    }

    sceIoRemove(dst);
    res = sceIoRename(src, dst);
    plog("export %08x: rename %s -> %s = %08x", (unsigned int)id, src, dst, (unsigned int)res);
    return res;
}

static int mime_patched(int unk, int *type, char **filename, char **mime_type) {
    int res = TAI_CONTINUE(int, mime_ref, unk, type, filename, mime_type);
    plog("mime: res=%08x type=%d", (unsigned int)res, type ? *type : -1);
    if (res == SCE_SHELL_ERROR_UNKNOWN_MIME && type) {
        // Pretend it is a photo so the download is accepted. The stock export
        // then fails, which export_patched handles for Switchfin downloads.
        *type = 1;
        return 0;
    }
    return res;
}

// Switchfin loads this plugin every time it starts, so a copy may already be
// running in SceShell. Only the first copy installs hooks.
static int already_loaded(void) {
    SceUID ids[128];
    SceSize num = sizeof(ids) / sizeof(ids[0]);
    int found = 0;
    if (sceKernelGetModuleList(0xFF, ids, &num) < 0) return 0;
    for (SceSize i = 0; i < num; i++) {
        SceKernelModuleInfo info;
        sceClibMemset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(ids[i], &info) >= 0 &&
            sceClibStrncmp(info.module_name, BGDL_PLUGIN_MODULE, sizeof(info.module_name)) == 0)
            found++;
    }
    return found > 1;
}

// Tells Switchfin which SceShell process has the hooks, so it can tell whether
// the plugin is active in the current boot.
static void write_pid_file(void) {
    char buf[16];
    SceUID fd = sceIoOpen(BGDL_PID_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    int len = sceClibSnprintf(buf, sizeof(buf), "%08x", (unsigned int)sceKernelGetProcessId());
    sceIoWrite(fd, buf, len);
    sceIoClose(fd);
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    if (already_loaded()) {
        write_pid_file();
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    tai_module_info_t info;
    sceClibMemset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    ShellOffsets off;
    if (taiGetModuleInfo("SceShell", &info) < 0 || get_shell_offsets(info.module_nid, &off) < 0)
        return SCE_KERNEL_START_NO_RESIDENT;

    // Only patch HEAD -> GET if the bytes really are "HEAD", so an unknown
    // SceShell build is never corrupted.
    SceKernelModuleInfo minfo;
    sceClibMemset(&minfo, 0, sizeof(minfo));
    minfo.size = sizeof(minfo);
    if (sceKernelGetModuleInfo(info.modid, &minfo) >= 0 && minfo.segments[0].vaddr &&
        off.head + 4 <= minfo.segments[0].memsz &&
        sceClibMemcmp((const char *)minfo.segments[0].vaddr + off.head, "HEAD", 4) == 0) {
        hook_ids[0] = taiInjectData(info.modid, 0, off.head, "GET", 4);
    }

    hook_ids[1] = taiHookFunctionOffset(&export_ref, info.modid, 0, off.exp, 1, export_patched);
    hook_ids[2] = taiHookFunctionOffset(&mime_ref, info.modid, 0, off.mime, 1, mime_patched);

    plog("start: pid=%08x nid=%08x head=%08x export=%08x mime=%08x", (unsigned int)sceKernelGetProcessId(),
        (unsigned int)info.module_nid, (unsigned int)hook_ids[0], (unsigned int)hook_ids[1],
        (unsigned int)hook_ids[2]);
    if (hook_ids[1] < 0 || hook_ids[2] < 0) {
        if (hook_ids[2] >= 0) taiHookRelease(hook_ids[2], mime_ref);
        if (hook_ids[1] >= 0) taiHookRelease(hook_ids[1], export_ref);
        if (hook_ids[0] >= 0) taiInjectRelease(hook_ids[0]);
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    write_pid_file();
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    if (hook_ids[2] >= 0) taiHookRelease(hook_ids[2], mime_ref);
    if (hook_ids[1] >= 0) taiHookRelease(hook_ids[1], export_ref);
    if (hook_ids[0] >= 0) taiInjectRelease(hook_ids[0]);
    return SCE_KERNEL_STOP_SUCCESS;
}
