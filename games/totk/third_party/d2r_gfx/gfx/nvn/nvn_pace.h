// 出图节拍器（纯函数，Mac / iOS 共用；单测 tests/t_pace.c）。
// 语义同垂直同步：节拍点 = 上一节拍点 + 周期。早到 → 等到节拍点；迟到不足一个周期 → 立即出图、保持原相位；
// 落后一个周期以上（卡顿）→ 从当前时刻重新起拍，不连发追帧。周期 0 = 不等（节拍点跟随当前时刻）。
// Present pacer (pure function, shared by Mac / iOS; unit test tests/t_pace.c).
// Semantics as vsync: present slot = previous present slot + period. Early → wait until the present slot; late by less than one period → present immediately and keep the original phase;
// behind by one period or more (stutter) → restart the cadence from the current time, no burst of presents to catch up. Period 0 = no wait (the present slot follows the current time).
#pragma once
#include <stdint.h>

typedef struct { uint64_t target; } nvn_pacer;

// now：单调时钟（ns）；period：本帧周期（ns，0 = 不等）。返回需要等待的 ns（0 = 立即出图），并更新内部目标时刻。
// now: monotonic clock (ns); period: this frame's period (ns, 0 = no wait). Returns the ns to wait (0 = present immediately) and updates the internal target time.
static inline uint64_t nvn_pace(nvn_pacer *p, uint64_t now, uint64_t period) {
    if (!period || !p->target) { p->target = now; return 0; }
    uint64_t next = p->target + period;
    if (now <= next) {
        // TOTK (2026-10-20): the target must not run more than one period ahead of now. The pacer advances
        // the target by one period per call whenever the caller arrives early, so a caller that runs FASTER
        // than the period (the uncapped mode, where the game presents at its own rate) lets the target run
        // away without bound: the texture then looks "available" seconds in the future. Measured: with
        // TOTK_MFX=0 the acquire's nominal wait reached 28.3 s at p50 after seven minutes, against a frame
        // that lasts 13.9 ms. The drift is exactly (period - frame interval), in four configurations.
        // With this cap the target never gets more than one period ahead, the wait is at most one period,
        // and the texture's availability stays within two periods. It is a no-op when the caller runs inside
        // the period (the target is already on the grid), and it bounds the runaway when it does not.
        // A side effect worth stating: the uncapped mode then presents at the period (the display's rate)
        // rather than faster, which is what a display can actually show.
        p->target = next;
        if (p->target > now + period) { p->target = now + period; return period; }
        return next - now;
    }
    p->target = now - next < period ? next : now;
    return 0;
}
