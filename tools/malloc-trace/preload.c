// Build: gcc -shared -fPIC -O1 -o preload.so preload.c -ldl -lpthread   (Linux, glibc). See docs/MEMORY.md section 3.
// LD_PRELOAD malloc tracer: records every live malloc/calloc/realloc/memalign block with its stack.
// Control by files: <dir>/mark (forget current blocks), <dir>/dump (write live blocks since mark to <dir>/live.txt).
#define _GNU_SOURCE
#include <dlfcn.h>
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
extern void* __libc_memalign(size_t, size_t);
#define CAP (1u << 21)
#define FR 14
typedef struct { void* p; size_t size; unsigned long serial; int n; void* st[FR]; } E;
static E* tab;
static unsigned long serial, markSerial;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int inside;
static __thread int holding; // this thread holds mu
static const char* dir;
static size_t slot(void* p) { return (size_t)(((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull >> 43) & (CAP - 1); }
static void add(void* p, size_t size)
{
    if (!p || !tab || inside) return;
    inside = 1;
    void* st[FR + 2];
    int n = backtrace(st, FR + 2);
    pthread_mutex_lock(&mu);
    size_t i = slot(p);
    for (size_t k = 0; k < CAP; ++k, i = (i + 1) & (CAP - 1))
        if (tab[i].p == 0 || tab[i].p == (void*)1) {
            tab[i].p = p; tab[i].size = size; tab[i].serial = ++serial;
            tab[i].n = n > 2 ? n - 2 : 0; memcpy(tab[i].st, st + 2, (size_t)tab[i].n * sizeof(void*));
            break;
        }
    pthread_mutex_unlock(&mu);
    inside = 0;
}
static void del(void* p)
{
    if (!p || !tab || holding) return;
    pthread_mutex_lock(&mu);
    size_t i = slot(p);
    for (size_t k = 0; k < CAP; ++k, i = (i + 1) & (CAP - 1)) {
        if (tab[i].p == p) { tab[i].p = (void*)1; break; }
        if (tab[i].p == 0) break;
    }
    pthread_mutex_unlock(&mu);
}
void* malloc(size_t s) { void* p = __libc_malloc(s); add(p, s); return p; }
void* calloc(size_t a, size_t b) { void* p = __libc_calloc(a, b); add(p, a * b); return p; }
void* realloc(void* q, size_t s) { del(q); void* p = __libc_realloc(q, s); add(p, s); return p; }
void free(void* p) { del(p); __libc_free(p); }
void* memalign(size_t a, size_t s) { void* p = __libc_memalign(a, s); add(p, s); return p; }
void* aligned_alloc(size_t a, size_t s) { void* p = __libc_memalign(a, s); add(p, s); return p; }
int posix_memalign(void** out, size_t a, size_t s) { void* p = __libc_memalign(a, s); if (!p) return 12; add(p, s); *out = p; return 0; }
static void* control(void* arg)
{
    (void)arg;
    inside = 1;
    char mark[512], dump[512], live[512];
    snprintf(mark, sizeof mark, "%s/mark", dir); snprintf(dump, sizeof dump, "%s/dump", dir); snprintf(live, sizeof live, "%s/live.txt", dir);
    for (;;) {
        usleep(200000);
        if (access(mark, F_OK) == 0) { pthread_mutex_lock(&mu); markSerial = serial; pthread_mutex_unlock(&mu); unlink(mark); }
        if (access(dump, F_OK) == 0) {
            E* copy = (E*)mmap(0, sizeof(E) * CAP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            size_t count = 0;
            pthread_mutex_lock(&mu);
            holding = 1;
            for (size_t i = 0; i < CAP && copy != MAP_FAILED; ++i)
                if ((uintptr_t)tab[i].p > 1 && tab[i].serial > markSerial)
                    copy[count++] = tab[i];
            holding = 0;
            pthread_mutex_unlock(&mu);
            int fd = open(live, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            unsigned long bytes = 0;
            for (size_t i = 0; i < count; ++i) {
                char line[96]; int len = snprintf(line, sizeof line, "B %zu\n", copy[i].size);
                if (write(fd, line, (size_t)len) < 0) break;
                backtrace_symbols_fd(copy[i].st, copy[i].n, fd);
                bytes += copy[i].size;
            }
            char line[96]; int len = snprintf(line, sizeof line, "TOTAL %zu blocks %lu bytes\n", count, bytes);
            if (write(fd, line, (size_t)len) < 0) {}
            close(fd);
            if (copy != MAP_FAILED) munmap(copy, sizeof(E) * CAP);
            unlink(dump);
        }
    }
    return 0;
}
__attribute__((constructor)) static void init(void)
{
    dir = getenv("MTRACE_DIR");
    if (!dir) return;
    char exe[512] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0 || !strstr(exe, getenv("MTRACE_PROGRAM") ? getenv("MTRACE_PROGRAM") : "/EasyNotepad")) { dir = 0; return; }
    unsetenv("LD_PRELOAD"); // the app's child processes (git, shells) are not traced
    void* m = mmap(0, sizeof(E) * CAP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) return;
    inside = 1;
    void* warm[4]; backtrace(warm, 4); // loads libgcc before tracing starts
    tab = (E*)m;
    pthread_t t; pthread_create(&t, 0, control, 0); pthread_detach(t);
    inside = 0;
}
