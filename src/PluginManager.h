#pragma once

#include "KCSE/KCSEAPI.h"
#include <string>
#include <vector>

using KCSE::PluginHandle;

namespace PluginManager {

bool Init();
void LoadAll(const KCSE::IKCSEInterface* kcseInterface);
void DispatchPostLoad();

PluginHandle LookupHandleFromName(const char* name);
const char* LookupNameFromHandle(PluginHandle handle);
const KCSE::PluginInfo* GetPluginInfo(const char* name);
PluginHandle GetCurrentPluginHandle();
void SetCurrentPluginHandle(PluginHandle handle);

bool RegisterListener(PluginHandle listener, const char* sender, KCSE::IMessagingInterface::EventCallback handler);
bool Dispatch(PluginHandle sender, uint32_t messageType, void* data, uint32_t dataLen, const char* receiver);

}  // namespace PluginManager
