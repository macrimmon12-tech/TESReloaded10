#pragma once

// Minimal mirror of xNVSE's DIHookControl for use by plugins.
//
// DIHookControl inherits from ISingleton<DIHookControl>, which has a virtual
// destructor.  That adds a vtable pointer at offset 0 in every instance, so
// m_keys begins at offset 4.  We replicate that layout here by declaring our
// own virtual destructor so the compiler inserts the same vtable slot.
//
// Obtain the singleton pointer via:
//   auto* data = (NVSEDataInterface*)nvse->QueryInterface(kInterface_Data);
//   g_DIHookCtrl = (DIHookControl*)data->GetSingleton(NVSEDataInterface::kNVSEData_DIHookControl);

enum
{
    kMacro_MouseButtonOffset = 256,         // LMB=256, RMB=257, ...
    kMacro_MouseWheelOffset  = 256 + 8,     // wheel-up=264, wheel-down=265
    kMaxMacros               = 256 + 8 + 2, // 266 total
};

class DIHookControl
{
public:
    virtual ~DIHookControl() = default; // mirrors ISingleton virtual dtor; keeps vtable ptr in layout

    enum
    {
        kDisable_User   = 1 << 0,
        kDisable_Script = 1 << 1,
        kDisable_All    = kDisable_User | kDisable_Script,
    };

    void SetKeyDisableState(UInt32 keycode, bool bDisable, UInt32 mask = 0)
    {
        if (!mask) mask = kDisable_All;
        if (keycode >= kMaxMacros) return;
        if (mask & kDisable_User)   m_keys[keycode].userDisable   = bDisable;
        if (mask & kDisable_Script) m_keys[keycode].scriptDisable = bDisable;
    }

private:
    // Must exactly match xNVSE's KeyInfo layout (7 bools, no padding).
    struct KeyInfo
    {
        bool rawState, gameState, insertedState, hold, tap, userDisable, scriptDisable;
    };
    KeyInfo m_keys[kMaxMacros]; // offset 4 (after vtable ptr)
};
