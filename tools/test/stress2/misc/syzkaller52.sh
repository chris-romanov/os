#!/bin/sh

# panic: already suspended
# cpuid = 6
# time = 1651176216
# KDB: stack backtrace:
# db_trace_self_wrapper() at db_trace_self_wrapper+0x2b/frame 0xfffffe014194ea70
# vpanic() at vpanic+0x17f/frame 0xfffffe014194eac0
# panic() at panic+0x43/frame 0xfffffe014194eb20
# thread_single() at thread_single+0x774/frame 0xfffffe014194eb90
# reap_kill_proc() at reap_kill_proc+0x296/frame 0xfffffe014194ebf0
# reap_kill() at reap_kill+0x371/frame 0xfffffe014194ed00
# kern_procctl() at kern_procctl+0x30b/frame 0xfffffe014194ed70
# sys_procctl() at sys_procctl+0x11e/frame 0xfffffe014194ee00
# amd64_syscall() at amd64_syscall+0x145/frame 0xfffffe014194ef30
# fast_syscall_common() at fast_syscall_common+0xf8/frame 0xfffffe014194ef30
# --- syscall (0, FreeBSD ELF64, nosys), rip = 0x8226f27aa, rsp = 0x82803ef48, rbp = 0x82803ef70 ---
# KDB: enter: panic
# [ thread pid 3074 tid 100404 ]
# Stopped at      kdb_enter+0x32: movq    $0,0x12790b3(%rip)
# db> x/s version
# FreeBSD 14.0-CURRENT #0 main-n255099-0923ff82fb383: Thu Apr 28 09:48:48 CEST 2022
# pho@mercat1.netperf.freebsd.org:/usr/src/sys/amd64/compile/PHO
# db>

[ `uname -p` != "amd64" ] && exit 0

. ../default.cfg
cat > /tmp/syzkaller52.c <<EOF
// https://syzkaller.appspot.com/bug?id=20185b6047d7371885412b56ff188be88f740eab
// autogenerated by syzkaller (https://github.com/google/syzkaller)
// Reported-by: syzbot+79cd12371d417441b175@syzkaller.appspotmail.com

#define _GNU_SOURCE

#include <sys/types.h>

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/endian.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned long long procid;

static void kill_and_wait(int pid, int* status)
{
  kill(pid, SIGKILL);
  while (waitpid(-1, status, 0) != pid) {
  }
}

static void sleep_ms(uint64_t ms)
{
  usleep(ms * 1000);
}

static uint64_t current_time_ms(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts))
    exit(1);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void use_temporary_dir(void)
{
  char tmpdir_template[] = "./syzkaller.XXXXXX";
  char* tmpdir = mkdtemp(tmpdir_template);
  if (!tmpdir)
    exit(1);
  if (chmod(tmpdir, 0777))
    exit(1);
  if (chdir(tmpdir))
    exit(1);
}

static void __attribute__((noinline)) remove_dir(const char* dir)
{
  DIR* dp = opendir(dir);
  if (dp == NULL) {
    if (errno == EACCES) {
      if (rmdir(dir))
        exit(1);
      return;
    }
    exit(1);
  }
  struct dirent* ep = 0;
  while ((ep = readdir(dp))) {
    if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0)
      continue;
    char filename[FILENAME_MAX];
    snprintf(filename, sizeof(filename), "%s/%s", dir, ep->d_name);
    struct stat st;
    if (lstat(filename, &st))
      exit(1);
    if (S_ISDIR(st.st_mode)) {
      remove_dir(filename);
      continue;
    }
    if (unlink(filename))
      exit(1);
  }
  closedir(dp);
  if (rmdir(dir))
    exit(1);
}

static void thread_start(void* (*fn)(void*), void* arg)
{
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 128 << 10);
  int i = 0;
  for (; i < 100; i++) {
    if (pthread_create(&th, &attr, fn, arg) == 0) {
      pthread_attr_destroy(&attr);
      return;
    }
    if (errno == EAGAIN) {
      usleep(50);
      continue;
    }
    break;
  }
  exit(1);
}

typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int state;
} event_t;

static void event_init(event_t* ev)
{
  if (pthread_mutex_init(&ev->mu, 0))
    exit(1);
  if (pthread_cond_init(&ev->cv, 0))
    exit(1);
  ev->state = 0;
}

static void event_reset(event_t* ev)
{
  ev->state = 0;
}

static void event_set(event_t* ev)
{
  pthread_mutex_lock(&ev->mu);
  if (ev->state)
    exit(1);
  ev->state = 1;
  pthread_mutex_unlock(&ev->mu);
  pthread_cond_broadcast(&ev->cv);
}

static void event_wait(event_t* ev)
{
  pthread_mutex_lock(&ev->mu);
  while (!ev->state)
    pthread_cond_wait(&ev->cv, &ev->mu);
  pthread_mutex_unlock(&ev->mu);
}

static int event_isset(event_t* ev)
{
  pthread_mutex_lock(&ev->mu);
  int res = ev->state;
  pthread_mutex_unlock(&ev->mu);
  return res;
}

static int event_timedwait(event_t* ev, uint64_t timeout)
{
  uint64_t start = current_time_ms();
  uint64_t now = start;
  pthread_mutex_lock(&ev->mu);
  for (;;) {
    if (ev->state)
      break;
    uint64_t remain = timeout - (now - start);
    struct timespec ts;
    ts.tv_sec = remain / 1000;
    ts.tv_nsec = (remain % 1000) * 1000 * 1000;
    pthread_cond_timedwait(&ev->cv, &ev->mu, &ts);
    now = current_time_ms();
    if (now - start > timeout)
      break;
  }
  int res = ev->state;
  pthread_mutex_unlock(&ev->mu);
  return res;
}

static void sandbox_common()
{
  struct rlimit rlim;
  rlim.rlim_cur = rlim.rlim_max = 128 << 20;
  setrlimit(RLIMIT_AS, &rlim);
  rlim.rlim_cur = rlim.rlim_max = 8 << 20;
  setrlimit(RLIMIT_MEMLOCK, &rlim);
  rlim.rlim_cur = rlim.rlim_max = 1 << 20;
  setrlimit(RLIMIT_FSIZE, &rlim);
  rlim.rlim_cur = rlim.rlim_max = 1 << 20;
  setrlimit(RLIMIT_STACK, &rlim);
  rlim.rlim_cur = rlim.rlim_max = 0;
  setrlimit(RLIMIT_CORE, &rlim);
  rlim.rlim_cur = rlim.rlim_max = 256;
  setrlimit(RLIMIT_NOFILE, &rlim);
}

static void loop();

static int do_sandbox_none(void)
{
  sandbox_common();
  loop();
  return 0;
}

struct thread_t {
  int created, call;
  event_t ready, done;
};

static struct thread_t threads[16];
static void execute_call(int call);
static int running;

static void* thr(void* arg)
{
  struct thread_t* th = (struct thread_t*)arg;
  for (;;) {
    event_wait(&th->ready);
    event_reset(&th->ready);
    execute_call(th->call);
    __atomic_fetch_sub(&running, 1, __ATOMIC_RELAXED);
    event_set(&th->done);
  }
  return 0;
}

static void execute_one(void)
{
  int i, call, thread;
  for (call = 0; call < 14; call++) {
    for (thread = 0; thread < (int)(sizeof(threads) / sizeof(threads[0]));
         thread++) {
      struct thread_t* th = &threads[thread];
      if (!th->created) {
        th->created = 1;
        event_init(&th->ready);
        event_init(&th->done);
        event_set(&th->done);
        thread_start(thr, th);
      }
      if (!event_isset(&th->done))
        continue;
      event_reset(&th->done);
      th->call = call;
      __atomic_fetch_add(&running, 1, __ATOMIC_RELAXED);
      event_set(&th->ready);
      event_timedwait(&th->done, 50);
      break;
    }
  }
  for (i = 0; i < 100 && __atomic_load_n(&running, __ATOMIC_RELAXED); i++)
    sleep_ms(1);
}

static void execute_one(void);

#define WAIT_FLAGS 0

static void loop(void)
{
  int iter = 0;
  for (;; iter++) {
    char cwdbuf[32];
    sprintf(cwdbuf, "./%d", iter);
    if (mkdir(cwdbuf, 0777))
      exit(1);
    int pid = fork();
    if (pid < 0)
      exit(1);
    if (pid == 0) {
      if (chdir(cwdbuf))
        exit(1);
      execute_one();
      exit(0);
    }
    int status = 0;
    uint64_t start = current_time_ms();
    for (;;) {
      if (waitpid(-1, &status, WNOHANG | WAIT_FLAGS) == pid)
        break;
      sleep_ms(1);
      if (current_time_ms() - start < 5000)
        continue;
      kill_and_wait(pid, &status);
      break;
    }
    remove_dir(cwdbuf);
  }
}

uint64_t r[4] = {0x0, 0x0, 0x0, 0x0};

void execute_call(int call)
{
  intptr_t res = 0;
  switch (call) {
  case 0:
    *(uint32_t*)0x20000000 = 0x3f;
    *(uint32_t*)0x20000004 = 8;
    *(uint32_t*)0x20000008 = 0x1000;
    *(uint32_t*)0x2000000c = 7;
    syscall(SYS_sigsuspend, 0x20000000ul);
    break;
  case 1:
    syscall(SYS_setgid, 0);
    break;
  case 2:
    syscall(SYS_getgroups, 0ul, 0ul);
    break;
  case 3:
    syscall(SYS_setegid, 0);
    break;
  case 4:
    res = syscall(SYS_shmget, 0ul, 0x2000ul, 0x420ul, 0x20ffd000ul);
    if (res != -1)
      r[0] = res;
    break;
  case 5:
    res = syscall(SYS_getpid);
    if (res != -1)
      r[1] = res;
    break;
  case 6:
    *(uint32_t*)0x20000200 = -1;
    *(uint32_t*)0x20000204 = 0;
    *(uint32_t*)0x20000208 = -1;
    *(uint32_t*)0x2000020c = 0;
    *(uint16_t*)0x20000210 = 0xf965;
    *(uint16_t*)0x20000212 = 0x2000;
    *(uint32_t*)0x20000214 = 0;
    *(uint64_t*)0x20000218 = 0x2d;
    *(uint32_t*)0x20000220 = 0x1f;
    *(uint64_t*)0x20000228 = 2;
    *(uint64_t*)0x20000230 = 4;
    *(uint64_t*)0x20000238 = 0;
    *(uint32_t*)0x20000240 = r[1];
    *(uint32_t*)0x20000244 = -1;
    *(uint16_t*)0x20000248 = 7;
    *(uint16_t*)0x2000024a = 0;
    *(uint64_t*)0x20000250 = 0;
    *(uint64_t*)0x20000258 = 0;
    syscall(SYS_shmctl, r[0], 1ul, 0x20000200ul);
    break;
  case 7:
    syscall(SYS_getgid);
    break;
  case 8:
    syscall(SYS___semctl, 0, 0ul, 1ul, 0ul);
    break;
  case 9:
    *(uint32_t*)0x20000300 = 4;
    *(uint32_t*)0x20000304 = 0;
    *(uint16_t*)0x20000308 = 7;
    *(uint16_t*)0x2000030a = 6;
    memcpy((void*)0x2000030c,
           "\x26\xb9\x52\x60\x70\xe1\xb8\x97\x99\x4b\x39\xd3\xea\x42\xe7\xed",
           16);
    syscall(SYS_fhstat, 0x20000300ul, 0ul);
    break;
  case 10:
    res = syscall(SYS_getgid);
    if (res != -1)
      r[2] = res;
    break;
  case 11:
    *(uint32_t*)0x20000440 = 3;
    *(uint32_t*)0x20000444 = 0;
    *(uint32_t*)0x20000448 = r[1];
    *(uint32_t*)0x2000044c = 0x81;
    *(uint32_t*)0x20000450 = r[1];
    memset((void*)0x20000454, 0, 60);
    res = syscall(SYS_procctl, 0ul, r[1], 6ul, 0x20000440ul);
    if (res != -1)
      r[3] = *(uint32_t*)0x20000450;
    break;
  case 12:
    *(uint32_t*)0x200004c0 = 0;
    *(uint32_t*)0x200004c4 = 0;
    *(uint32_t*)0x200004c8 = 0;
    *(uint32_t*)0x200004cc = r[2];
    *(uint16_t*)0x200004d0 = 0x100;
    *(uint16_t*)0x200004d2 = 8;
    *(uint32_t*)0x200004d4 = 0;
    *(uint64_t*)0x200004d8 = 0x7ff;
    *(uint64_t*)0x200004e0 = 0x7f;
    *(uint64_t*)0x200004e8 = 0x81;
    *(uint64_t*)0x200004f0 = 0xfff;
    *(uint64_t*)0x200004f8 = 0x3a;
    *(uint64_t*)0x20000500 = 0x100000000;
    *(uint64_t*)0x20000508 = 9;
    *(uint32_t*)0x20000510 = r[1];
    *(uint32_t*)0x20000514 = r[3];
    *(uint64_t*)0x20000518 = 0;
    *(uint64_t*)0x20000520 = 0;
    syscall(SYS_msgctl, -1, 1ul, 0x200004c0ul);
    break;
  case 13:
    syscall(SYS_ioctl, -1, 0xc0f24425ul, 0ul);
    break;
  }
}
int main(void)
{
  syscall(SYS_mmap, 0x20000000ul, 0x1000000ul, 7ul, 0x1012ul, -1, 0ul);
  for (procid = 0; procid < 4; procid++) {
    if (fork() == 0) {
      use_temporary_dir();
      do_sandbox_none();
    }
  }
  sleep(1000000);
  return 0;
}
EOF
mycc -o /tmp/syzkaller52 -Wall -Wextra -O0 /tmp/syzkaller52.c -l pthread ||
    exit 1

start=`date +%s`
while [ $((`date +%s` - start)) -lt 120 ]; do
	(cd /tmp; timeout 3m ./syzkaller52)
done

rm -rf /tmp/syzkaller52 /tmp/syzkaller52.c /tmp/syzkaller52.core \
    /tmp/syzkaller.??????
exit 0