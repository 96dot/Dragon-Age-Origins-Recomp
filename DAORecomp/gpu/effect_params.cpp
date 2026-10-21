// Binds an effect's parameters with the null ones unbound (a crash after loading an
// area with input held, such as the Ostagar save after Awakening's; seen on Title
// Update 6, the disc build has the same code).
//
// An effect (made by the effect factory, 82693CF0 in Title Update 6) keeps its
// parameters in four parallel vectors at +28, +44, +60 and +76: the descriptors and
// three values passed with each. Its bind (vtable slot 22: sub_826644B0,
// tu6_sub_82698458) passes each to the per-parameter bind (sub_82663090,
// tu6_sub_82697038) with the descriptor's field +48. The code that makes the effect
// (tu6_sub_825C0A08) takes the descriptor from a resource's parameter list, cached in
// its owner at +332; while that list is empty (the resource still streaming in) it
// skips the lookup and stores the cached value, null the first time. The bind then
// reads null+48 and the render thread faults. This hook is the game's bind loop with
// the null entries unbound, the way the effect's unbind (tu6_sub_826984E0) clears
// every slot, so the effect draws without that parameter rather than with the
// previous draw's. The effect keeps its null for its lifetime.
#include "stdafx.h"
#include "kernel/function.h"
#include "kernel/game_build.h"

namespace
{
    constexpr dao::kernel::GameAddress kBindParameter{ 0x82663090, 0x82697038 };

    // The effect's vectors (each a begin pointer, the end at +4), indexed together.
    constexpr uint32_t kDescriptors = 28;
    constexpr uint32_t kSlots = 44;
    constexpr uint32_t kValues = 60;
    constexpr uint32_t kExtras = 76;
    // The descriptor's field the per-parameter bind takes.
    constexpr uint32_t kDescriptorField = 48;
    // r6 for each parameter's own value from kValues, rather than one for all.
    constexpr uint32_t kOwnValues = 0xFFFFFFFF;
    // More parameters than any effect has: a broken vector, left to the game's code.
    constexpr uint32_t kMaxParameters = 4096;

    // r3 the effect, r4 the device state, r6 the value for all or kOwnValues.
    void BindParametersHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
    {
        const uint32_t effect = ctx.r3.u32;
        const uint32_t begin = PPC_LOAD_U32(effect + kDescriptors);
        const uint32_t end = PPC_LOAD_U32(effect + kDescriptors + 4);
        if (end < begin || (end - begin) / 4 > kMaxParameters)
        {
            original(ctx, base);
            return;
        }

        // The game's loop: the count taken once, the vectors read each time.
        const uint32_t count = (end - begin) / 4;
        const uint32_t state = ctx.r4.u32;
        const uint32_t allValue = ctx.r6.u32;
        const uint32_t bind = kBindParameter.Get();
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t offset = i * 4;
            const uint32_t descriptor = PPC_LOAD_U32(PPC_LOAD_U32(effect + kDescriptors) + offset);
            ctx.r3.u64 = state;
            ctx.r4.u64 = PPC_LOAD_U32(PPC_LOAD_U32(effect + kSlots) + offset);
            ctx.r8.u64 = 0;
            if (descriptor)
            {
                ctx.r5.u64 = PPC_LOAD_U32(descriptor + kDescriptorField);
                ctx.r6.u64 = allValue != kOwnValues ? allValue : PPC_LOAD_U32(PPC_LOAD_U32(effect + kValues) + offset);
                ctx.r7.u64 = PPC_LOAD_U32(PPC_LOAD_U32(effect + kExtras) + offset);
            }
            else
            {
                static std::atomic<bool> logged{ false };
                if (!logged.load(std::memory_order_relaxed) && !logged.exchange(true))
                    LOGW("Effect {:08X}: a parameter that wasn't loaded when it was made is unbound", effect);
                ctx.r5.u64 = 0;
                ctx.r6.u64 = 0;
                ctx.r7.u64 = 0;
            }
            dao::kernel::g_memory.Call(bind, ctx);
        }
    }
}

GAME_HOOK(sub_826644B0, tu6_sub_82698458, BindParametersHook)
