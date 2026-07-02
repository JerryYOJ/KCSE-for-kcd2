#pragma once

#include "KCSE/KCSEAPI.h"

using KCSE::PluginHandle;
using KCSE::TaskFn;

class MessagingInterface final : public KCSE::IMessagingInterface {
public:
    uint32_t GetInterfaceVersion() const override { return kInterfaceVersion; }
    bool RegisterListener(EventCallback handler) override;
    bool RegisterListener(const char* sender, EventCallback handler) override;
    bool Dispatch(uint32_t messageType, void* data, uint32_t dataLen, const char* receiver) override;
};

class TaskInterfaceImpl final : public KCSE::ITaskInterface {
public:
    uint32_t GetInterfaceVersion() const override { return kInterfaceVersion; }
    void AddTask(TaskFn task) override;
};

class KCSEInterfaceImpl final : public KCSE::IKCSEInterface {
public:
    uint32_t GetKCSEVersion() const override;
    uint32_t GetGameVersion() const override;
    uint32_t GetReleaseIndex() const override;
    PluginHandle GetPluginHandle() const override;
    const KCSE::PluginInfo* GetPluginInfo(const char* name) const override;
    void* QueryInterface(uint32_t id) const override;
};

extern MessagingInterface  g_messagingInterface;
extern TaskInterfaceImpl   g_taskInterface;
extern KCSEInterfaceImpl   g_kcseInterface;
