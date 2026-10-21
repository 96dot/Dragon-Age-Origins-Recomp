// Skips the null parameters of an effect when it binds them (a crash after loading
// an area with input held, such as the Ostagar save after Awakening's).
//
// An effect (made by the effect factory, 82693CF0 in Title Update 6) keeps its
// parameters in four parallel vectors: descriptors at +28, values at +44, +60 and
// +76. Its bind (vtable slot 22: sub_826644B0, tu6_sub_82698458) passes each to the
// per-parameter bind (sub_82663090, tu6_sub_82697038) with the descriptor's field
// +48. The code that makes the effect (tu6_sub_825C0A08) takes the descriptor from
// a resource's parameter list, cached in its owner at +332; when that list is still
// empty (the resource not loaded yet: an effect made while the area streams in), it
// skips the lookup and passes the cached value, null the first time. The bind then
// reads null+48 and the render thread faults. The game's own code treats an empty
// list as no parameter, so this bind does the same: it binds the others and skips
// the null ones (the effect draws without that parameter, as it would without the
// list).
#include "stdafx.h"
#include "kernel/function.h"

namespace
{
    // The per-parameter bind: (r3 device state, r4 value, r5 the descriptor's +48,
    // r6 value or override, r7 value, r8 0).
    constexpr dao::kernel::GameAddress kBindParameter{ 0x82663090, 0x82697038 };

    // The effect's vectors (begin; end at +4).
    constexpr uint32_t kDescriptors = 28;
    constexpr uint32_t kValues = 44;
    constexpr uint32_t kDefaults = 60;
    constexpr uint32_t kExtras = 76;
    constexpr uint32_t kDescriptorField = 48;

    // r3 the effect, r4 the device state, r6 a value for all (-1: each one's own).
    void BindParametersHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
    {
        const uint32_t effect = ctx.r3.u32;
        const uint32_t begin = PPC_LOAD_U32(effect + kDescriptors);
        const uint32_t count = (PPC_LOAD_U32(effect + kDescriptors + 4) - begin) / 4;
        bool missing = false;
        for (uint32_t i = 0; i < count && !missing; i++)
            missing = PPC_LOAD_U32(begin + i * 4) == 0;
        if (!missing)
        {
            original(ctx, base);
            return;
        }

        static std::atomic<bool> logged{ false };
        if (!logged.exchange(true))
            LOGW("Effect {:08X}: a parameter that wasn't loaded when it was made is skipped", effect);
        // The game's loop, with the null descriptors skipped.
        const uint32_t state = ctx.r4.u32;
        const uint32_t overrideValue = ctx.r6.u32;
        const uint32_t bind = kBindParameter.Get();
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t offset = i * 4;
            const uint32_t descriptor = PPC_LOAD_U32(PPC_LOAD_U32(effect + kDescriptors) + offset);
            if (!descriptor)
                continue;
            ctx.r3.u64 = state;
            ctx.r4.u64 = PPC_LOAD_U32(PPC_LOAD_U32(effect + kValues) + offset);
            ctx.r5.u64 = PPC_LOAD_U32(descriptor + kDescriptorField);
            ctx.r6.u64 = overrideValue != 0xFFFFFFFF ? overrideValue : PPC_LOAD_U32(PPC_LOAD_U32(effect + kDefaults) + offset);
            ctx.r7.u64 = PPC_LOAD_U32(PPC_LOAD_U32(effect + kExtras) + offset);
            ctx.r8.u64 = 0;
            dao::kernel::g_memory.Call(bind, ctx);
        }
    }
}

GAME_HOOK(sub_826644B0, tu6_sub_82698458, BindParametersHook)
