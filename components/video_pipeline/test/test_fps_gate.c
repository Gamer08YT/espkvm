/*
 * The frame rate limit on simulated frame times: 60, 59.94, 50 and 30 Hz
 * input with jitter and encoder stalls, an hour each so a slow drift shows.
 * Checks the average rate, frames lost to jitter and bursts after a stall.
 * Then the encoder's planned rate, which must not follow every wobble.
 */
#include <stdio.h>

#include "fps_gate.h"

static int fails;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            fails++;                                                   \
        }                                                              \
    } while (0)

static uint32_t rng = 1;
static int64_t jitter(int64_t max_us)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (int64_t)(rng % (uint32_t)(2 * max_us + 1)) - max_us;
}

typedef struct {
    int64_t taken;       /* frames over the whole run */
    int64_t min_minute;  /* the slowest minute */
    int64_t max_minute;  /* the fastest minute */
    int64_t worst_burst; /* most frames taken in any one second */
} run_t;

/*
 * Frames every @p period_ns, each off by up to @p jitter_us, for @p seconds.
 * From @p stall_every_s on, a stall of @p stall_us: the loop is busy and the
 * frames of that time never reach the gate.
 */
static run_t run(int64_t period_ns, int64_t jitter_us, int fps_max, int seconds,
                 int stall_every_s, int64_t stall_us)
{
    const int64_t interval_us = 1000000 / fps_max;
    const int64_t end_us = (int64_t)seconds * 1000000;
    run_t r = {0, INT64_MAX, 0, 0};
    int64_t next_us = 0, minute = 0, minute_n = 0, busy_until = 0, last_stall = 0;
    int64_t recent[128] = {0}; /* times of the last frames taken, a ring */
    int n_recent = 0;

    for (int64_t i = 1;; i++) {
        const int64_t now_us = i * period_ns / 1000 + jitter(jitter_us);
        if (now_us >= end_us) {
            break;
        }
        if (stall_every_s && now_us - last_stall >= (int64_t)stall_every_s * 1000000) {
            last_stall = now_us;
            busy_until = now_us + stall_us;
        }
        if (now_us < busy_until) {
            continue;
        }
        if (now_us / 60000000 != minute) {
            if (minute_n < r.min_minute) r.min_minute = minute_n;
            if (minute_n > r.max_minute) r.max_minute = minute_n;
            minute = now_us / 60000000;
            minute_n = 0;
        }
        if (!fps_gate_take(&next_us, now_us, interval_us)) {
            continue;
        }
        r.taken++;
        minute_n++;
        recent[n_recent++ % 128] = now_us;
        /* how many of the frames taken lately fall within the last second */
        int64_t in_window = 0;
        for (int k = 0; k < 128 && k < n_recent; k++) {
            if (now_us - recent[k] < 1000000) {
                in_window++;
            }
        }
        if (in_window > r.worst_burst) r.worst_burst = in_window;
    }
    return r;
}

int main(void)
{
    const int hour = 3600;
    run_t r;

    /* 60 Hz, limit 30, +-3 ms: the 0.69.0 bug gave 25 fps here */
    r = run(16666667, 3000, 30, hour, 0, 0);
    printf("60 Hz, limit 30, jitter 3 ms: %.3f fps, minutes %lld..%lld\n",
           (double)r.taken / hour, (long long)r.min_minute, (long long)r.max_minute);
    CHECK(r.taken >= 30 * hour - 30 && r.taken <= 30 * hour + 30);
    CHECK(r.min_minute >= 1799 && r.max_minute <= 1801);
    CHECK(r.worst_burst <= 31);

    /* 60 Hz, limit 60: every frame, jitter or not */
    r = run(16666667, 2000, 60, hour, 0, 0);
    printf("60 Hz, limit 60, jitter 2 ms: %.3f fps\n", (double)r.taken / hour);
    CHECK(r.taken >= 60 * hour - 1);

    /* 59.94 Hz, limit 60: slower than the limit, so nothing is dropped */
    r = run(16683350, 2000, 60, hour, 0, 0);
    printf("59.94 Hz, limit 60, jitter 2 ms: %.3f fps\n", (double)r.taken / hour);
    CHECK(r.taken >= (int64_t)(59.94 * hour) - 1);

    /* 30 Hz input right at a 30 limit: each frame early or late by up to 4 ms */
    r = run(33333333, 4000, 30, hour, 0, 0);
    printf("30 Hz, limit 30, jitter 4 ms: %.3f fps\n", (double)r.taken / hour);
    CHECK(r.taken >= 30 * hour - 1);

    /* 50 Hz, limit 30: 50 cannot be split evenly, the average stays under 30 */
    r = run(20000000, 2000, 30, hour, 0, 0);
    printf("50 Hz, limit 30, jitter 2 ms: %.3f fps\n", (double)r.taken / hour);
    CHECK(r.taken <= 30 * hour + 1 && r.taken >= 24 * hour);

    /* encoder stalls of 250 ms once a second: the missed slots are skipped */
    r = run(16666667, 2000, 30, hour, 1, 250000);
    printf("60 Hz, limit 30, 250 ms stall every second: %.3f fps, burst %lld\n",
           (double)r.taken / hour, (long long)r.worst_burst);
    CHECK(r.worst_burst <= 31);
    CHECK(r.taken <= 30 * hour * 3 / 4 + hour);

    /* stalls a bit under one interval: the late frame keeps its slot */
    r = run(16666667, 1000, 30, hour, 1, 30000);
    printf("60 Hz, limit 30, 30 ms stall every second: %.3f fps, burst %lld\n",
           (double)r.taken / hour, (long long)r.worst_burst);
    CHECK(r.worst_burst <= 31);
    CHECK(r.taken <= 30 * hour + 1);

    /* the plan's share of the source: steady while the rate wanders */
    {
        int32_t div = 1;
        const int32_t made_720p60[] = {53, 59, 52, 58, 57, 60, 51, 59, 53}; /* a Switch at 720p60 */
        for (unsigned i = 0; i < sizeof made_720p60 / sizeof made_720p60[0]; i++) {
            div = fps_plan_div(div, 60, made_720p60[i]);
            CHECK(div == 1);
        }
        const int32_t made_1080p50[] = {26, 25, 27, 26, 24, 26}; /* every other frame */
        for (unsigned i = 0; i < sizeof made_1080p50 / sizeof made_1080p50[0]; i++) {
            div = fps_plan_div(div, 50, made_1080p50[i]);
            CHECK(div == 2);
        }
        CHECK(fps_plan_div(2, 50, 49) == 1); /* back to every frame */
        CHECK(fps_plan_div(1, 30, 28) == 1); /* 1080p30, encoder a little short */
        CHECK(fps_plan_div(1, 60, 28) == 2); /* 1080p60 on rev 3.x */
        CHECK(fps_plan_div(3, 60, 0) == 3);  /* no measurement: keep */
        CHECK(fps_plan_div(1, 0, 30) == 1);  /* no source rate: keep */
    }

    if (fails) {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("fps gate: all passed\n");
    return 0;
}
