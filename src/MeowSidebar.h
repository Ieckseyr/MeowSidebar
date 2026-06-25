// MeowSidebar.h - 侧边栏插件主模块
//
// 提供基于协议层手动发包的侧边栏显示功能，
// 同时作为 PAPI 注册中心（通过 MeowPAPI 静态库）。
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

} // namespace meowsidebar
