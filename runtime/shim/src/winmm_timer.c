/* winmm: multimedia timers. timeSetEvent starts a timer that fires an event or calls a guest
 * callback. One timer thread serves them all. It holds the guest lock while it runs, so a
 * callback runs like any other guest code and never overlaps another thread.
 */
#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "vt3/vt3_log.h"
#include "win32/kernel32.h"
#include "win32/winmm.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VT3_MAX_TIMERS 64    /* int, so the loop indices stay signed */
#define TIMERR_NOERROR 0u
#define TIMERR_NOCANDIDATES 97u
#define TIME_PERIODIC 0x0001u
#define TIME_CALLBACK_EVENT_SET 0x0010u
#define TIME_CALLBACK_EVENT_PULSE 0x0020u

struct vt3_timer {
    UINT id;
    int active;
    uint32_t period;          /* ms; periodic timers reschedule by this much */
    uint32_t next;            /* monotonic ms of the next firing */
    uint32_t callback;        /* guest TimeProc, or a HANDLE for the event modes */
    uint32_t user;
    UINT flags;
};

static struct vt3_timer timers[VT3_MAX_TIMERS];
static UINT next_id = 1;
static int thread_started;

static void *timer_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(vt3s_gil());
    vt3s_thread_attach_host();
    for (;;) {
        uint32_t now = vt3s_now_ms();

        /* Fire what is due. A callback runs guest code, so it runs with the lock held. The slot
         * is settled before the callback runs: a periodic timer has its next deadline set, and a
         * one-shot timer is switched off. Then the callback may kill this timer or re-arm into the
         * same slot. The loop never reads the slot again after the callback, so it cannot switch
         * off a timer that the callback just re-armed. */
        for (int i = 0; i < VT3_MAX_TIMERS; i++) {
            struct vt3_timer *t = &timers[i];
            if (!t->active || (int32_t)(now - t->next) < 0) continue;
            struct vt3_timer f = *t;
            if (f.flags & TIME_PERIODIC) t->next += f.period ? f.period : 1u;
            else t->active = 0;
            if (f.flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE)) {
                HANDLE h = f.callback;
                if (f.flags & TIME_CALLBACK_EVENT_PULSE) PulseEvent(h);
                else SetEvent(h);
            } else if (f.callback) {
                /* TimeProc(uTimerID, uMsg, dwUser, dw1, dw2), stdcall. */
                uint32_t args[5] = {f.id, 0, f.user, 0, 0};
                vt3s_call_guest_current(f.callback, args, 5);
            }
        }

        /* Sleep until the next deadline, or until a timer is added or killed. */
        uint32_t soonest = 0;
        int any = 0;
        for (int i = 0; i < VT3_MAX_TIMERS; i++) {
            struct vt3_timer *t = &timers[i];
            if (!t->active) continue;
            if (!any || (int32_t)(t->next - soonest) < 0) soonest = t->next;
            any = 1;
        }
        if (!any) {
            pthread_cond_wait(vt3s_gcond(), vt3s_gil());
            continue;
        }
        now = vt3s_now_ms();
        if ((int32_t)(soonest - now) <= 0) continue;
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        uint32_t wait = soonest - now;
        deadline.tv_sec += wait / 1000u;
        deadline.tv_nsec += (long)(wait % 1000u) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(vt3s_gcond(), vt3s_gil(), &deadline);
    }
    return NULL;
}

MMRESULT WINAPI timeSetEvent(UINT delay, UINT resolution, LPVOID proc, DWORD user, UINT flags)
{
    (void)resolution;
    struct vt3_timer *slot = NULL;
    for (int i = 0; i < VT3_MAX_TIMERS; i++) {
        if (!timers[i].active) { slot = &timers[i]; break; }
    }
    if (!slot) return TIMERR_NOCANDIDATES;
    memset(slot, 0, sizeof *slot);
    slot->id = next_id++;
    slot->active = 1;
    slot->period = delay;
    slot->next = vt3s_now_ms() + delay;
    slot->callback = (uint32_t)proc;
    slot->user = user;
    slot->flags = flags;
    if (!thread_started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 8u << 20);
        if (pthread_create(&th, &at, timer_main, NULL) != 0) {
            slot->active = 0;
            pthread_attr_destroy(&at);
            return TIMERR_NOCANDIDATES;
        }
        pthread_attr_destroy(&at);
        pthread_detach(th);
        thread_started = 1;
    } else {
        pthread_cond_broadcast(vt3s_gcond());
    }
    return slot->id;
}

MMRESULT WINAPI timeKillEvent(UINT id)
{
    for (int i = 0; i < VT3_MAX_TIMERS; i++) {
        if (timers[i].active && timers[i].id == id) {
            timers[i].active = 0;
            pthread_cond_broadcast(vt3s_gcond());
            return TIMERR_NOERROR;
        }
    }
    return TIMERR_NOCANDIDATES;
}

MMRESULT WINAPI timeGetDevCaps(LPVOID caps, UINT size)
{
    /* TIMECAPS: wPeriodMin, wPeriodMax. */
    if (size < 8 || !vt3s_guest_range_ok(caps, 8)) return TIMERR_NOCANDIDATES;
    vt3s_store_u32(caps, 1u);
    vt3s_store_u32(caps + 4, 1000u);
    return TIMERR_NOERROR;
}
