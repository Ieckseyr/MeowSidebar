// MeowSidebar.h - 侧边栏插件主模块
//
// 负责侧边栏渲染（配置 + 占位符）与 PAPI 注册中心（通过 MeowPAPI）。
// 协议层显示由 HologramLib 的 ISidebar 承担。
// 支持其他 C++ 插件和 LSE 插件通过 RemoteCall 注册占位符。
#pragma once

#include "ll/api/event/ListenerBase.h"
#include "ll/api/mod/NativeMod.h"

#include <vector>

namespace meowsidebar {

class MeowSidebar {
public:
    static MeowSidebar& getInstance();

    MeowSidebar() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

private:
    ll::mod::NativeMod&                  mSelf;
    std::vector<ll::event::ListenerPtr>  mListenerPtrs;
};

MeowSidebar& lib();

// MeowPAPI 是否已链上（内嵌部署成功，或连上了 LeviLamina 已加载的 MeowPAPI）
// 未就绪时侧边栏照常显示，占位符保持原文，不再让整插件 load 失败。
bool papiReady();

} // namespace meowsidebar
