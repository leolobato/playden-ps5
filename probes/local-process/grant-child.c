// SPDX-License-Identifier: GPL-3.0-or-later
// Payload: stands in for the title's /data grant in the probe. Start it before the title. It removes earlier runs' records, waits for each
// process to publish its PID, checks that the PID really is that program, and gives it the real root view, where the bottle shows its
// true owner, without changing its uid. A process waits for its grant-ok file, written only after every step worked. Exits after 120 s.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define FOLDER "/data/homebrew/FAKE77099"

struct Target {
  const char* pid_file;
  const char* ok_file;
  const char* program;
};

static const struct Target kTargets[] = {
    {FOLDER "/title-pid", FOLDER "/grant-ok-title", "eboot.bin"},
    {FOLDER "/child-pid", FOLDER "/grant-ok-child", "bdj.elf"},
};
enum { kTargetCount = sizeof kTargets / sizeof kTargets[0] };

static int IsProgram(FILE* log, int pid, const char* program) {
  int query[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
  // The console's record is larger than the SDK's. The fields used here must still match, or the process is refused.
  static union {
    struct kinfo_proc info;
    char bytes[8192];
  } record;
  size_t size = sizeof record;
  memset(&record, 0, sizeof record);
  const int status = sysctl(query, 4, &record, &size, NULL, 0);
  const int query_errno = errno;
  record.info.ki_comm[COMMLEN] = 0;
  fprintf(log, "pid=%d query=%d errno=%d size=%zu header=%zu ki_pid=%d name=%s\n", pid, status, query_errno, size, sizeof record.info,
          (int)record.info.ki_pid, record.info.ki_comm);
  return status == 0 && size >= sizeof record.info && record.info.ki_pid == pid && strcmp(record.info.ki_comm, program) == 0;
}

// Each step must work before the next. Returns 0 only when the whole grant is in place.
static int Grant(FILE* log, int pid) {
  const intptr_t root = kernel_get_proc_rootdir(getpid());
  if (root == 0 || root == -1) return 1;
  fprintf(log, "pid=%d uid=%d its root=0x%lx\n", pid, (int)kernel_get_ucred_uid(pid), (long)kernel_get_proc_rootdir(pid));
  // The attributes are written back whole, so a failed read must never reach the write.
  uint8_t attrs[32];
  const int got = kernel_get_ucred_attrs(pid, attrs);
  fprintf(log, "get attrs=%d\n", got);
  if (got != 0) return 2;
  attrs[3] |= 0x80;
  const int rootdir = kernel_set_proc_rootdir(pid, root);
  const int jaildir = rootdir == 0 ? kernel_set_proc_jaildir(pid, 0) : -1;
  fprintf(log, "set rootdir=%d jaildir=%d\n", rootdir, jaildir);
  if (rootdir != 0 || jaildir != 0) return 3;
  // The rest of the grant, as the helper does it: caps, authid and the unsandboxed attribute. The uid stays the app's.
  uint8_t caps[16];
  memset(caps, 0xff, sizeof caps);
  const int set_caps = kernel_set_ucred_caps(pid, caps);
  const int set_authid = set_caps == 0 ? kernel_set_ucred_authid(pid, 0x4801000000000013ul) : -1;
  const int set_attrs = set_authid == 0 ? kernel_set_ucred_attrs(pid, attrs) : -1;
  fprintf(log, "caps=%d authid=%d attrs=%d\n", set_caps, set_authid, set_attrs);
  return set_caps != 0 || set_authid != 0 || set_attrs != 0 ? 4 : 0;
}

static int ReadPid(const char* path) {
  FILE* in = fopen(path, "r");
  int pid = 0;
  if (in != NULL) {
    if (fscanf(in, "%d", &pid) != 1) pid = 0;
    fclose(in);
  }
  return pid;
}

int main(void) {
  FILE* log = fopen("/data/local-process-probe/grant.log", "w");
  if (log == NULL) return 1;
  for (int t = 0; t < kTargetCount; t++) {
    unlink(kTargets[t].pid_file);
    unlink(kTargets[t].ok_file);
    if (access(kTargets[t].pid_file, F_OK) == 0 || access(kTargets[t].ok_file, F_OK) == 0) {
      fprintf(log, "cannot remove the old records of %s\n", kTargets[t].program);
      fclose(log);
      return 2;
    }
  }
  // 0 waiting, 1 granted, -1 refused. The title's PID may be absent in the runs that use only a child.
  int state[kTargetCount] = {0};
  for (int i = 0; i < 1200 && (state[0] == 0 || state[1] == 0); i++) {
    for (int t = 0; t < kTargetCount; t++) {
      const int pid = state[t] == 0 ? ReadPid(kTargets[t].pid_file) : 0;
      if (pid <= 0) continue;
      int failed = IsProgram(log, pid, kTargets[t].program) ? Grant(log, pid) : -1;
      fprintf(log, "%s grant result=%d\n", kTargets[t].program, failed);
      fflush(log);
      state[t] = failed == 0 ? 1 : -1;
      if (failed == 0) close(open(kTargets[t].ok_file, O_WRONLY | O_CREAT | O_TRUNC, 0666));
    }
    usleep(100000);
  }
  fprintf(log, "done title=%d child=%d\n", state[0], state[1]);
  fclose(log);
  return 0;
}
