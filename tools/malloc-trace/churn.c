// Build: gcc -shared -fPIC -O1 -o churn.so churn.c -ldl -lpthread   (Linux, glibc). See docs/MEMORY.md section 3.
// LD_PRELOAD allocation-churn tracer: preload.c shows what stays allocated; this shows what is
// allocated at all (and freed again) - the buffers that move pages around even when nothing grows.
// It totals every allocation of at least CHURN_MIN bytes (default 1024) per call stack between
// <CHURN_DIR>/start and <CHURN_DIR>/dump, written to <CHURN_DIR>/churn.txt; tools/malloc-trace/churn.py
// groups it by the app function that asked. Its table is an 8 MB mapping of its own (dirty once
// tracing starts): leave it out of private-memory readings.
#define _GNU_SOURCE
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
extern void* __libc_malloc(size_t);
extern void __libc_free(void*);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
#define CAP (1u << 16)
#define FR 12
typedef struct { uint64_t key; unsigned long count; unsigned long bytes; int n; void* st[FR]; } S;
static S* tab;
static volatile int on;
static size_t minSize = 1024;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int inside;
static const char* dir;
static void note(size_t size)
{
    if (!on || !tab || inside || size < minSize) return;
    inside = 1;
    void* st[FR + 2];
    int n = backtrace(st, FR + 2);
    uint64_t key = 1469598103934665603ull;
    for (int i = 2; i < n; ++i) key = (key ^ (uint64_t)(uintptr_t)st[i]) * 1099511628211ull;
    pthread_mutex_lock(&mu);
    size_t i = (size_t)(key >> 40) & (CAP - 1);
    for (size_t k = 0; k < CAP; ++k, i = (i + 1) & (CAP - 1)) {
        if (tab[i].key == key) { tab[i].count++; tab[i].bytes += size; break; }
        if (tab[i].key == 0) {
            tab[i].key = key; tab[i].count = 1; tab[i].bytes = size;
            tab[i].n = n > 2 ? n - 2 : 0; memcpy(tab[i].st, st + 2, (size_t)tab[i].n * sizeof(void*));
            break;
        }
    }
    pthread_mutex_unlock(&mu);
    inside = 0;
}
void* malloc(size_t s) { note(s); return __libc_malloc(s); }
void* calloc(size_t a, size_t b) { note(a * b); return __libc_calloc(a, b); }
void* realloc(void* q, size_t s) { note(s); return __libc_realloc(q, s); }
static void* control(void* arg)
{
    (void)arg;
    inside = 1;
    char start[512], dump[512], out[512];
    snprintf(start, sizeof start, "%s/start", dir); snprintf(dump, sizeof dump, "%s/dump", dir); snprintf(out, sizeof out, "%s/churn.txt", dir);
    for (;;) {
        usleep(100000);
        if (access(start, F_OK) == 0) { pthread_mutex_lock(&mu); memset(tab, 0, sizeof(S) * CAP); pthread_mutex_unlock(&mu); on = 1; unlink(start); }
        if (access(dump, F_OK) == 0) {
            on = 0;
            int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            for (size_t i = 0; i < CAP; ++i)
                if (tab[i].key) {
                    char line[96]; int len = snprintf(line, sizeof line, "B %lu %lu\n", tab[i].bytes, tab[i].count);
                    if (write(fd, line, (size_t)len) < 0) break;
                    backtrace_symbols_fd(tab[i].st, tab[i].n, fd);
                }
            close(fd);
            unlink(dump);
        }
    }
    return 0;
}
__attribute__((constructor)) static void init(void)
{
    dir = getenv("CHURN_DIR");
    if (!dir) return;
    char exe[512] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0 || !strstr(exe, "/EasyNotepad")) { dir = 0; return; }
    unsetenv("LD_PRELOAD");
    if (getenv("CHURN_MIN")) minSize = (size_t)atol(getenv("CHURN_MIN"));
    void* m = mmap(0, sizeof(S) * CAP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) return;
    inside = 1;
    void* warm[4]; backtrace(warm, 4);
    tab = (S*)m;
    pthread_t t; pthread_create(&t, 0, control, 0); pthread_detach(t);
    inside = 0;
}
