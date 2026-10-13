/* kernel32: threads, synchronisation objects, critical sections, TLS, and Sleep.
 *
 * Each guest thread is a pthread with its own guest stack and TEB, carved from the guest
 * arena. Guest code runs only while its thread holds the guest lock (gil). The runtime
 * under the Win32 layer (handle table, heap, D3D, DirectSound) assumes one thread at a
 * time, so the lock keeps that true. A thread gives the lock up only while it waits, and it
 * waits on one condition variable (gcond). Every state change broadcasts gcond, and waits
 * re-check their condition when they wake. Events, mutexes, critical sections, and thread
 * handles all wait this way.
 */
#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "vt3/vt3_log.h"
#include "vt3shim/shim.h"
#include "win32/kernel32.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VT3_TEB_BYTES 0x1000u
#define VT3_THREAD_STACK (1u << 20)      /* guest stack, in the arena */
#define VT3_HOST_STACK (8u << 20)        /* host stack for the pthread's C frames */
#define VT3_PEB 0x0FE01000u              /* shared PEB; the runtime sets it up */
#define VT3_MAX_TLS 64u
#define VT3_STILL_ACTIVE 259u
#define VT3_CREATE_SUSPENDED 0x4u
#define VT3_PSEUDO_CURRENT_THREAD 0xFFFFFFFEu
#define VT3_WAIT_MAX 64u
#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif

#define CS_LOCKCOUNT 4u
#define CS_RECURSION 8u
#define CS_OWNER 12u

struct vt3s_thread {
    pthread_t host;
    uint32_t tid;
    uint32_t stack_top;   /* guest address the thread's stack starts at (grows down) */
    uint32_t teb;
    uint32_t entry, param;
    uint32_t suspend_count;
    int finished;
    int is_main;
    DWORD exit_code;
    DWORD priority;
    uint32_t tls[VT3_MAX_TLS];
    struct vt3s_thread *next;     /* every thread, for zeroing a TLS slot on free */
};

struct vt3s_sync {
    int is_mutex;
    int manual;             /* manual-reset event */
    int signaled;
    uint32_t owner;         /* mutex owner tid, 0 when free */
    uint32_t count;         /* mutex recursion */
    uint32_t gen;          /* PulseEvent generation: waiters that started earlier are released */
};

static pthread_mutex_t gil = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gcond = PTHREAD_COND_INITIALIZER;
static vt3s_thread g_main = {.tid = 1, .is_main = 1};
static vt3s_thread *threads_head = &g_main;
static uint32_t next_tid = 2;
static uint8_t tls_in_use[VT3_MAX_TLS];
static vt3shim_guest_call_fn guest_call;
static __thread vt3s_thread *g_cur;

static vt3s_thread *current(void) { return g_cur ? g_cur : &g_main; }

pthread_mutex_t *vt3s_gil(void) { return &gil; }
pthread_cond_t *vt3s_gcond(void) { return &gcond; }
uint32_t vt3s_current_tid(void) { return current()->tid; }

uint32_t vt3s_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void vt3shim_set_guest_call(vt3shim_guest_call_fn fn) { guest_call = fn; }

DWORD vt3s_call_guest(uint32_t stack_top, uint32_t teb, uint32_t fn, const uint32_t *args, uint32_t nargs)
{
    return guest_call ? guest_call(stack_top, teb, fn, args, nargs) : 0;
}

/* Calls guest code on the calling thread's own stack. */
DWORD vt3s_call_guest_current(uint32_t fn, const uint32_t *args, uint32_t nargs)
{
    vt3s_thread *t = current();
    return vt3s_call_guest(t->stack_top, t->teb, fn, args, nargs);
}

/* The main thread takes the guest lock before the guest entry runs, and keeps it until it
 * waits. */
void vt3shim_thread_begin_main(void)
{
    pthread_mutex_lock(&gil);
    g_cur = &g_main;
}

/* Give a new thread its guest stack and TEB. Caller holds the lock. */
static int guest_context(vt3s_thread *t)
{
    uint32_t stack = vt3s_vm_alloc(VT3_THREAD_STACK);
    uint32_t teb = vt3s_vm_alloc(VT3_TEB_BYTES);
    if (!stack || !teb) return 0;
    t->stack_top = stack + VT3_THREAD_STACK - 16u;
    t->teb = teb;
    vt3s_store_u32(teb + 0x04, t->stack_top);        /* stack base */
    vt3s_store_u32(teb + 0x08, stack);               /* stack limit */
    vt3s_store_u32(teb + 0x18, teb);                 /* self */
    vt3s_store_u32(teb + 0x24, t->tid);              /* thread id */
    vt3s_store_u32(teb + 0x30, VT3_PEB);             /* PEB */
    return 1;
}

static void link_thread(vt3s_thread *t)
{
    t->next = threads_head;
    threads_head = t;
}

/* Attach the calling host thread as a guest thread, for the timer thread. Caller holds gil. */
vt3s_thread *vt3s_thread_attach_host(void)
{
    vt3s_thread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->tid = next_tid++;
    if (!guest_context(t)) { free(t); return NULL; }
    link_thread(t);
    g_cur = t;
    return t;
}

/* A suspended thread blocks here until ResumeThread brings its count back to zero.
 * Suspension is checked at waits and at polling points, not in the middle of guest code.
 * Caller holds gil. */
static void honor_suspend(void)
{
    vt3s_thread *t = current();
    while (t->suspend_count) pthread_cond_wait(&gcond, &gil);
}

/* Wait with the guest lock released until pred(arg) holds, or ms expires. ms == INFINITE
 * waits forever. Returns 1 if pred holds, 0 on timeout. Caller holds gil. */
typedef int (*vt3s_pred)(void *arg);

static int wait_until(vt3s_pred pred, void *arg, DWORD ms)
{
    honor_suspend();
    struct timespec deadline;
    if (ms != INFINITE) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += ms / 1000u;
        deadline.tv_nsec += (long)(ms % 1000u) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    }
    while (!pred(arg)) {
        if (ms == INFINITE) {
            pthread_cond_wait(&gcond, &gil);
        } else if (pthread_cond_timedwait(&gcond, &gil, &deadline) == ETIMEDOUT) {
            return pred(arg);
        }
    }
    return 1;
}

static int never(void *arg) { (void)arg; return 0; }

/* ---- thread lifecycle ----------------------------------------------------- */

static void *thread_main(void *arg)
{
    vt3s_thread *t = arg;
    pthread_mutex_lock(&gil);
    g_cur = t;
    while (t->suspend_count) pthread_cond_wait(&gcond, &gil);
    uint32_t args[1] = {t->param};
    DWORD rc = vt3s_call_guest(t->stack_top, t->teb, t->entry, args, 1);
    t->exit_code = rc;
    t->finished = 1;
    pthread_cond_broadcast(&gcond);
    pthread_mutex_unlock(&gil);
    return NULL;
}

HANDLE WINAPI CreateThread(LPVOID attrs, SIZE_T stack, LPVOID start, LPVOID param, DWORD flags, LPDWORD tid)
{
    (void)attrs; (void)stack;
    if (tid) vt3s_store_u32(tid, 0);
    if (!start || !guest_call) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    vt3s_thread *t = calloc(1, sizeof *t);
    if (!t) { vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    t->tid = next_tid++;
    t->entry = start;
    t->param = param;
    t->suspend_count = (flags & VT3_CREATE_SUSPENDED) ? 1u : 0u;
    if (!guest_context(t)) {
        free(t);
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    HANDLE h = vt3s_handle_new(VT3S_H_THREAD, t);
    if (!h) { free(t); vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, VT3_HOST_STACK);
    int rc = pthread_create(&t->host, &at, thread_main, t);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        vt3s_handle_take(h, VT3S_H_THREAD);
        free(t);
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    pthread_detach(t->host);            /* nothing joins; WaitForSingleObject watches the flag */
    link_thread(t);
    if (tid) vt3s_store_u32(tid, t->tid);
    VT3_INFO("CreateThread: tid %u, handle 0x%x, entry 0x%x", t->tid, h, start);
    vt3s_set_last_error(ERROR_SUCCESS);
    return h;
}

static vt3s_thread *thread_of(HANDLE h)
{
    if (h == VT3_PSEUDO_CURRENT_THREAD) return current();
    return vt3s_handle_get(h, VT3S_H_THREAD);
}

void WINAPI ExitThread(DWORD code)
{
    vt3s_thread *t = current();
    if (t->is_main) {
        /* The main thread leaves the process. Other threads are not waited for. */
        fflush(stdout);
        exit((int)code);
    }
    t->exit_code = code;
    t->finished = 1;
    pthread_cond_broadcast(&gcond);
    pthread_mutex_unlock(&gil);
    pthread_exit(NULL);
}

DWORD WINAPI ResumeThread(HANDLE h)
{
    vt3s_thread *t = thread_of(h);
    if (!t) { vt3s_set_last_error(ERROR_INVALID_HANDLE); return 0xFFFFFFFFu; }
    DWORD prev = t->suspend_count;
    if (t->suspend_count) t->suspend_count--;
    pthread_cond_broadcast(&gcond);
    vt3s_set_last_error(ERROR_SUCCESS);
    return prev;
}

DWORD WINAPI SuspendThread(HANDLE h)
{
    vt3s_thread *t = thread_of(h);
    if (!t) { vt3s_set_last_error(ERROR_INVALID_HANDLE); return 0xFFFFFFFFu; }
    DWORD prev = t->suspend_count;
    t->suspend_count++;     /* another thread stops at its next wait or polling point */
    if (t == current()) honor_suspend();   /* a thread that suspends itself waits for ResumeThread */
    return prev;
}

BOOL WINAPI GetExitCodeThread(HANDLE h, LPDWORD code)
{
    vt3s_thread *t = thread_of(h);
    if (!t) { vt3s_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (code) vt3s_store_u32(code, t->finished ? t->exit_code : VT3_STILL_ACTIVE);
    return TRUE;
}

BOOL WINAPI SetThreadPriority(HANDLE h, int prio)
{
    vt3s_thread *t = thread_of(h);
    if (!t) { vt3s_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    t->priority = (DWORD)prio;
    return TRUE;
}

int WINAPI GetThreadPriority(HANDLE h)
{
    vt3s_thread *t = thread_of(h);
    return t ? (int)t->priority : 0x7FFFFFFF;   /* THREAD_PRIORITY_ERROR_RETURN */
}

DWORD WINAPI SetThreadIdealProcessor(HANDLE h, DWORD cpu)
{
    (void)h; (void)cpu;
    return 0;
}

HANDLE WINAPI GetCurrentThread(void) { return VT3_PSEUDO_CURRENT_THREAD; }

DWORD WINAPI GetCurrentThreadId(void) { return current()->tid; }

BOOL WINAPI SwitchToThread(void)
{
    pthread_mutex_unlock(&gil);
    sched_yield();
    pthread_mutex_lock(&gil);
    return TRUE;
}

/* A guest thread that polls (spinning on InterlockedExchange, or entering and leaving a
 * critical section) never reaches a blocking call, so it would hold the guest lock for
 * ever. Polling APIs call this. Every 64th call releases the lock for a moment. */
void vt3s_spin_point(void)
{
    honor_suspend();
    static unsigned calls;
    if ((++calls & 63u) == 0) {
        pthread_mutex_unlock(&gil);
        sched_yield();
        pthread_mutex_lock(&gil);
    }
}

void WINAPI Sleep(DWORD ms)
{
    if (ms == 0) { SwitchToThread(); return; }
    wait_until(never, NULL, ms);
}

/* ---- thread-local storage ------------------------------------------------- */

DWORD WINAPI TlsAlloc(void)
{
    for (uint32_t i = 0; i < VT3_MAX_TLS; i++) {
        if (!tls_in_use[i]) {
            tls_in_use[i] = 1;
            for (vt3s_thread *t = threads_head; t; t = t->next) t->tls[i] = 0;
            vt3s_set_last_error(ERROR_SUCCESS);
            return i;
        }
    }
    vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0xFFFFFFFFu;
}

LPVOID WINAPI TlsGetValue(DWORD index)
{
    if (index >= VT3_MAX_TLS || !tls_in_use[index]) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    vt3s_set_last_error(ERROR_SUCCESS);
    return current()->tls[index];
}

BOOL WINAPI TlsSetValue(DWORD index, LPVOID value)
{
    if (index >= VT3_MAX_TLS || !tls_in_use[index]) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    current()->tls[index] = value;
    vt3s_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI TlsFree(DWORD index)
{
    if (index >= VT3_MAX_TLS || !tls_in_use[index]) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    tls_in_use[index] = 0;
    for (vt3s_thread *t = threads_head; t; t = t->next) t->tls[index] = 0;
    return TRUE;
}

/* ---- critical sections ---------------------------------------------------- */

struct cs_wait { LPVOID cs; uint32_t tid; };

static int cs_free(void *arg)
{
    struct cs_wait *w = arg;
    uint32_t owner = vt3s_load_u32(w->cs + CS_OWNER);
    return owner == 0 || owner == w->tid;
}

void WINAPI InitializeCriticalSection(LPVOID cs)
{
    if (!vt3s_guest_range_ok(cs, 24)) return;
    memset(vt3_g2h(cs), 0, 24);
    vt3s_store_u32(cs + CS_LOCKCOUNT, 0xFFFFFFFFu);
}

void WINAPI EnterCriticalSection(LPVOID cs)
{
    vt3s_spin_point();
    if (!vt3s_guest_range_ok(cs, 24)) return;
    uint32_t me = current()->tid;
    uint32_t owner = vt3s_load_u32(cs + CS_OWNER);
    if (owner != 0 && owner != me) {
        struct cs_wait w = {cs, me};
        wait_until(cs_free, &w, INFINITE);
    }
    vt3s_store_u32(cs + CS_OWNER, me);
    vt3s_store_u32(cs + CS_RECURSION, vt3s_load_u32(cs + CS_RECURSION) + 1u);
    vt3s_store_u32(cs + CS_LOCKCOUNT, vt3s_load_u32(cs + CS_LOCKCOUNT) + 1u);
}

void WINAPI LeaveCriticalSection(LPVOID cs)
{
    if (!vt3s_guest_range_ok(cs, 24)) return;
    uint32_t rec = vt3s_load_u32(cs + CS_RECURSION);
    if (rec) vt3s_store_u32(cs + CS_RECURSION, rec - 1u);
    vt3s_store_u32(cs + CS_LOCKCOUNT, vt3s_load_u32(cs + CS_LOCKCOUNT) - 1u);
    if (rec <= 1u) {
        vt3s_store_u32(cs + CS_OWNER, 0u);
        pthread_cond_broadcast(&gcond);
    }
}

void WINAPI DeleteCriticalSection(LPVOID cs)
{
    if (!vt3s_guest_range_ok(cs, 24)) return;
    memset(vt3_g2h(cs), 0, 24);
}

/* ---- events and mutexes --------------------------------------------------- */

static HANDLE new_sync(int is_mutex, int manual, int signaled, uint32_t owner)
{
    struct vt3s_sync *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->is_mutex = is_mutex;
    s->manual = manual;
    s->signaled = signaled;
    s->owner = owner;
    s->count = owner ? 1u : 0u;
    HANDLE h = vt3s_handle_new(VT3S_H_SYNC, s);
    if (!h) free(s);
    return h;
}

HANDLE WINAPI CreateEventA(LPVOID attrs, BOOL manual, BOOL initial, LPCSTR name)
{
    (void)attrs; (void)name;
    HANDLE h = new_sync(0, manual ? 1 : 0, initial ? 1 : 0, 0);
    vt3s_set_last_error(h ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return h;
}

HANDLE WINAPI CreateMutexA(LPVOID attrs, BOOL owner, LPCSTR name)
{
    (void)attrs; (void)name;
    HANDLE h = new_sync(1, 0, 0, owner ? current()->tid : 0);
    vt3s_set_last_error(h ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return h;
}

BOOL WINAPI SetEvent(HANDLE h)
{
    struct vt3s_sync *s = vt3s_handle_get(h, VT3S_H_SYNC);
    if (!s || s->is_mutex) { vt3s_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    s->signaled = 1;
    pthread_cond_broadcast(&gcond);
    return TRUE;
}

BOOL WINAPI ResetEvent(HANDLE h)
{
    struct vt3s_sync *s = vt3s_handle_get(h, VT3S_H_SYNC);
    if (!s || s->is_mutex) { vt3s_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    s->signaled = 0;
    return TRUE;
}

/* Wakes the waiters now, then the event is no longer signalled. */
BOOL WINAPI PulseEvent(HANDLE h)
{
    struct vt3s_sync *s = vt3s_handle_get(h, VT3S_H_SYNC);
    if (!s || s->is_mutex) { vt3s_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    s->gen++;
    s->signaled = 0;
    pthread_cond_broadcast(&gcond);
    return TRUE;
}

BOOL WINAPI ReleaseMutex(HANDLE h)
{
    struct vt3s_sync *s = vt3s_handle_get(h, VT3S_H_SYNC);
    if (!s || !s->is_mutex) { vt3s_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (s->owner != current()->tid || s->count == 0) {
        vt3s_set_last_error(288u);                 /* ERROR_NOT_OWNER */
        return FALSE;
    }
    if (--s->count == 0) s->owner = 0;
    pthread_cond_broadcast(&gcond);
    return TRUE;
}

/* ---- waiting on handles --------------------------------------------------- */

struct wobj {
    vt3s_thread *thread;     /* set for a thread handle */
    struct vt3s_sync *sync;  /* set for an event or mutex */
    uint32_t tid;
    uint32_t gen;
};

static int obj_ready(const struct wobj *o)
{
    if (o->thread) return o->thread->finished;
    if (o->sync->is_mutex) return o->sync->owner == 0 || o->sync->owner == o->tid;
    return o->sync->signaled || o->sync->gen != o->gen;
}

/* Take the object once it is ready: an auto-reset event clears, a mutex is acquired. */
static void obj_take(const struct wobj *o)
{
    if (!o->sync) return;
    if (o->sync->is_mutex) {
        o->sync->owner = o->tid;
        o->sync->count++;
    } else if (!o->sync->manual) {
        o->sync->signaled = 0;
    }
}

struct wset {
    uint32_t n;
    struct wobj o[VT3_WAIT_MAX];
    int wait_all;
    uint32_t hit;
};

static int set_ready(void *arg)
{
    struct wset *w = arg;
    int any = 0, all = 1;
    for (uint32_t i = 0; i < w->n; i++) {
        int r = obj_ready(&w->o[i]);
        if (r && !any) { any = 1; w->hit = i; }
        all = all && r;
    }
    return w->wait_all ? all : any;
}

/* Build the wait set from handles. Returns 0 if any handle is invalid. */
static int collect(struct wset *w, const uint32_t *hs, uint32_t n)
{
    w->n = n;
    for (uint32_t i = 0; i < n; i++) {
        struct wobj *o = &w->o[i];
        memset(o, 0, sizeof *o);
        o->tid = current()->tid;
        vt3s_thread *t = thread_of(hs[i]);
        struct vt3s_sync *s = t ? NULL : vt3s_handle_get(hs[i], VT3S_H_SYNC);
        if (!t && !s) return 0;
        o->thread = t;
        o->sync = s;
        o->gen = s ? s->gen : 0;
    }
    return 1;
}

/* Shared body of WaitForSingleObject and WaitForMultipleObjects. */
static DWORD wait_set(struct wset *w, DWORD ms)
{
    if (!wait_until(set_ready, w, ms)) return 0x102u;           /* WAIT_TIMEOUT */
    if (w->wait_all) {
        for (uint32_t i = 0; i < w->n; i++) obj_take(&w->o[i]);
        return 0u;
    }
    obj_take(&w->o[w->hit]);
    return w->hit;
}

DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms)
{
    struct wset w;
    if (!collect(&w, &h, 1)) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return 0xFFFFFFFFu;                                    /* WAIT_FAILED */
    }
    w.wait_all = 0;
    vt3s_set_last_error(ERROR_SUCCESS);
    return wait_set(&w, ms);
}

DWORD WINAPI WaitForMultipleObjects(DWORD count, LPVOID handles, BOOL wait_all, DWORD ms)
{
    struct wset w;
    if (count == 0 || count > VT3_WAIT_MAX || !vt3s_guest_range_ok(handles, 4 * count)) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0xFFFFFFFFu;
    }
    uint32_t hs[VT3_WAIT_MAX];
    for (uint32_t i = 0; i < count; i++) hs[i] = vt3s_load_u32(handles + 4 * i);
    if (!collect(&w, hs, count)) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return 0xFFFFFFFFu;
    }
    w.wait_all = wait_all ? 1 : 0;
    vt3s_set_last_error(ERROR_SUCCESS);
    return wait_set(&w, ms);
}
