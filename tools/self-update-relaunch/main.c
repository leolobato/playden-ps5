// main.c - Playden's one-shot relaunch payload for its self-update.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A title cannot start itself after it exits. Just before Playden quits to finish an update it
// sends this payload to the payload loader on 127.0.0.1:9021. The payload reads the title id
// from REQUEST ("FAKE77031\n"), waits until that title's sandbox (/mnt/sandbox/<id>_*) is gone,
// and starts it again with sceLncUtilLaunchApp, falling back to sceSystemServiceLaunchApp. It
// gives up after 120 s. If the start fails, a notification asks the player to open Playden.
// Measured in results/20261015-self-update-probes: the sandbox goes about 5 s after the quit.
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REQUEST "/data/playden/self-update/relaunch-title"
#define LOG "/data/playden/self-update/relaunch.log"

typedef struct {
    uint32_t size;
    uint32_t user_id;
    uint32_t app_opt;
    uint32_t pad;
    uint64_t crash_report;
    uint64_t check_flag;
} launch_params;

typedef struct {
    int type, req_id, priority, msg_id, target_id, user_id, unk1, unk2, app_id, error_num, unk3;
    char use_icon_image_uri;
    char message[1024];
    char icon_uri[1024];
    char unk[1024];
} notify_request;

int sceKernelSendNotificationRequest(int, notify_request*, size_t, int);
int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(uint32_t*);
int sceLncUtilInitialize(void);
uint32_t sceLncUtilLaunchApp(const char*, const char**, launch_params*);
uint32_t sceSystemServiceLaunchApp(const char*, const char**, launch_params*);

static FILE* out;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int valid_title(const char* title) {
    if (strlen(title) != 9) return 0;
    for (int i = 0; i < 9; ++i)
        if (i < 4 ? title[i] < 'A' || title[i] > 'Z' : title[i] < '0' || title[i] > '9') return 0;
    return 1;
}

// 1 while the title's sandbox exists, 0 when it is gone, -1 when /mnt/sandbox can't be read.
static int running(const char* title) {
    DIR* folder = opendir("/mnt/sandbox");
    if (!folder) return -1;
    char prefix[16];
    snprintf(prefix, sizeof prefix, "%s_", title);
    int found = 0;
    struct dirent* entry;
    while ((entry = readdir(folder)))
        if (!strncmp(entry->d_name, prefix, strlen(prefix))) found = 1;
    closedir(folder);
    return found;
}

static void notify(const char* text) {
    notify_request request;
    memset(&request, 0, sizeof request);
    strncpy(request.message, text, sizeof request.message - 1);
    fprintf(out, "notify rc=0x%08x\n", sceKernelSendNotificationRequest(0, &request, sizeof request, 0));
}

int main(void) {
    out = fopen(LOG, "w");
    if (!out) return 1;
    setvbuf(out, NULL, _IONBF, 0);
    char title[32] = {0};
    FILE* request = fopen(REQUEST, "r");
    if (!request || !fgets(title, sizeof title, request)) {
        fprintf(out, "no request\n");
        if (request) fclose(request);
        return 1;
    }
    fclose(request);
    title[strcspn(title, "\r\n")] = 0;
    if (!valid_title(title)) {
        fprintf(out, "bad title id\n");
        return 1;
    }
    const double start = now();
    int state;
    while ((state = running(title)) == 1 && now() - start < 120) usleep(250000);
    fprintf(out, "title=%s wait state=%d s=%.2f\n", title, state, now() - start);
    if (state != 0) {
        notify("Playden was updated. Open it again from the Home screen.");
        return 1;
    }
    sceUserServiceInitialize(NULL);
    uint32_t user = 0;
    sceUserServiceGetForegroundUser(&user);
    sceLncUtilInitialize();
    launch_params params = {sizeof params, user, 0, 0, 0, 0};
    uint32_t rc = sceLncUtilLaunchApp(title, NULL, &params);
    fprintf(out, "sceLncUtilLaunchApp rc=0x%08x\n", rc);
    if (rc & 0x80000000u) {
        launch_params fallback = {sizeof fallback, user, 0, 0, 0, 1};
        rc = sceSystemServiceLaunchApp(title, NULL, &fallback);
        fprintf(out, "sceSystemServiceLaunchApp rc=0x%08x\n", rc);
    }
    if (rc & 0x80000000u) notify("Playden was updated. Open it again from the Home screen.");
    fclose(out);
    return 0;
}
