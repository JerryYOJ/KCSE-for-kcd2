#include "EventDispatcher.h"
#include "PluginManager.h"
#include "KCSE/KCSEAPI.h"
#include "KCSE/Trampoline.h"
#include "Offsets/Offsets.h"
#include "REL.h"
#include <cstdint>
#include <Windows.h>
#include <spdlog/spdlog.h>

// CCryAction (IGameFramework) vtable slots (KCD2).
static constexpr std::size_t kSlot_CompleteInit  = 8;    // DataLoaded one-shot (0x180BF3808: loads main.lua)
static constexpr std::size_t kSlot_OnActionEvent = 120;  // OnActionEvent broadcaster (sub_180668D48)

// WHGame.dll call sites to redirect (image-relative RVAs). Both mirror KCD1: hook the
// (manager, message) dispatch call, fire the plugin event, then call the original.
static constexpr std::size_t kCall_CreateSaveGame  = 0xFC334F;   // commitQueuedSave -> CreateSaveGame
static constexpr std::size_t kCall_NewGameDispatch = 0x194FF50;  // PrepareNewGame -> module-message broadcast
                                                                 // of C_NewGamePrepareMessage (id 48)

// ---- CompleteInit vtable hook (DataLoaded, one-shot) ----

using CompleteInitFn = void(__fastcall*)(void* pThis);
static CompleteInitFn g_origCompleteInit = nullptr;

void __fastcall Hooked_CompleteInit(void* pThis)
{
    g_origCompleteInit(pThis);
    spdlog::info("DataLoaded");
    PluginManager::Dispatch(KCSE::kPluginHandle_KCSE, KCSE::IMessagingInterface::kMessage_DataLoaded, nullptr, 0, nullptr);
    REL::Relocation<>{ *reinterpret_cast<std::uintptr_t*>(pThis) }.write_vfunc(kSlot_CompleteInit, g_origCompleteInit);
    g_origCompleteInit = nullptr;
}

// ---- OnActionEvent vtable hook (LoadGame) ----
// eAE_PostLoad (0xD) fires from CCryAction::LoadGame after deserialize -- the KCD1 hook,
// same ordinal. (0xE is the WH C_SaveGameManager::PostLoadGame event; if 0xD proves not
// to fire for a WH savegame load at runtime, switch to eAE_InGame.)

using OnActionEventFn = void(__fastcall*)(void* pThis, SActionEvent* pEvent);
static OnActionEventFn g_origOnActionEvent = nullptr;

void __fastcall Hooked_OnActionEvent(void* pThis, SActionEvent* pEvent)
{
    g_origOnActionEvent(pThis, pEvent);

    if (pEvent->m_event == SActionEvent::eAE_PostLoad) {
        spdlog::info("LoadGame");
        PluginManager::Dispatch(KCSE::kPluginHandle_KCSE, KCSE::IMessagingInterface::kMessage_LoadGame, nullptr, 0, nullptr);
    }
}

// ---- SaveGame: redirect the sole call to C_SaveGameManager::CreateSaveGame ----
// commitQueuedSave is the only caller, and CreateSaveGame the universal choke for every
// save type, so this fires once per save, before the write (matches KCD1 ordering).

using CreateSaveGameFn = char(__fastcall*)(void* pThis, uint8_t type, int index, const char* name);
static CreateSaveGameFn g_origCreateSaveGame = nullptr;

char __fastcall Hooked_CreateSaveGame(void* pThis, uint8_t type, int index, const char* name)
{
    spdlog::info("SaveGame");
    PluginManager::Dispatch(KCSE::kPluginHandle_KCSE, KCSE::IMessagingInterface::kMessage_SaveGame, nullptr, 0, nullptr);
    return g_origCreateSaveGame(pThis, type, index, name);
}

// ---- NewGame: redirect the module-message broadcast inside PrepareNewGame ----

using DispatchMessageFn = void(__fastcall*)(void* pManager, void* pMessage);
static DispatchMessageFn g_origNewGameDispatch = nullptr;

void __fastcall Hooked_NewGameDispatch(void* pManager, void* pMessage)
{
    spdlog::info("NewGame");
    PluginManager::Dispatch(KCSE::kPluginHandle_KCSE, KCSE::IMessagingInterface::kMessage_NewGame, nullptr, 0, nullptr);
    g_origNewGameDispatch(pManager, pMessage);
}

namespace EventDispatcher {

void Install()
{
    auto* pFramework = Offsets::GetCCryAction();

    REL::Relocation<> fwVtbl{ *reinterpret_cast<std::uintptr_t*>(pFramework) };
    g_origCompleteInit = reinterpret_cast<CompleteInitFn>(
        fwVtbl.write_vfunc(kSlot_CompleteInit, Hooked_CompleteInit));
    g_origOnActionEvent = reinterpret_cast<OnActionEventFn>(
        fwVtbl.write_vfunc(kSlot_OnActionEvent, Hooked_OnActionEvent));

    g_origCreateSaveGame = reinterpret_cast<CreateSaveGameFn>(
        REL::Relocation<>{ REL::Offset(kCall_CreateSaveGame) }.write_call<5>(Hooked_CreateSaveGame));

    g_origNewGameDispatch = reinterpret_cast<DispatchMessageFn>(
        REL::Relocation<>{ REL::Offset(kCall_NewGameDispatch) }.write_call<5>(Hooked_NewGameDispatch));

    spdlog::info("Hooks installed");
}

void Remove()
{
    auto* pFramework = Offsets::GetCCryAction();
    if (pFramework) {
        REL::Relocation<> fwVtbl{ *reinterpret_cast<std::uintptr_t*>(pFramework) };
        if (g_origCompleteInit)
            fwVtbl.write_vfunc(kSlot_CompleteInit, g_origCompleteInit);
        if (g_origOnActionEvent)
            fwVtbl.write_vfunc(kSlot_OnActionEvent, g_origOnActionEvent);
    }
    // The CreateSaveGame / NewGame call-site patches are restored by
    // KCSE::GetTrampoline().destroy() (DllMain).
}

}  // namespace EventDispatcher
