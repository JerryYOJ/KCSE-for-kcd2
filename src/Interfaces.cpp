#include "Interfaces.h"
#include "PluginManager.h"
#include "TaskInterface.h"

extern uint32_t g_kcseVersion;
extern uint32_t g_gameVersion;

MessagingInterface  g_messagingInterface;
TaskInterfaceImpl   g_taskInterface;
KCSEInterfaceImpl   g_kcseInterface;

// ---- Messaging ----

bool MessagingInterface::RegisterListener(EventCallback handler)
{
    return PluginManager::RegisterListener(PluginManager::GetCurrentPluginHandle(), "KCSE", handler);
}

bool MessagingInterface::RegisterListener(const char* sender, EventCallback handler)
{
    return PluginManager::RegisterListener(PluginManager::GetCurrentPluginHandle(), sender, handler);
}

bool MessagingInterface::Dispatch(uint32_t messageType, void* data, uint32_t dataLen, const char* receiver)
{
    return PluginManager::Dispatch(PluginManager::GetCurrentPluginHandle(), messageType, data, dataLen, receiver);
}

// ---- Task ----

void TaskInterfaceImpl::AddTask(TaskFn task)
{
    TaskInterface::AddTask(task);
}

// ---- Root ----

uint32_t KCSEInterfaceImpl::GetKCSEVersion() const  { return g_kcseVersion; }
uint32_t KCSEInterfaceImpl::GetGameVersion() const   { return g_gameVersion; }
uint32_t KCSEInterfaceImpl::GetReleaseIndex() const  { return g_kcseVersion; }

PluginHandle KCSEInterfaceImpl::GetPluginHandle() const
{
    return PluginManager::GetCurrentPluginHandle();
}

const KCSE::PluginInfo* KCSEInterfaceImpl::GetPluginInfo(const char* name) const
{
    return PluginManager::GetPluginInfo(name);
}

void* KCSEInterfaceImpl::QueryInterface(uint32_t id) const
{
    switch (id) {
    case KCSE::kInterface_Messaging: return &g_messagingInterface;
    case KCSE::kInterface_Task:      return &g_taskInterface;
    default:                         return nullptr;
    }
}
