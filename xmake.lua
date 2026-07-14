-- MeowSidebar - C++ 侧边栏插件 + PAPI 注册中心
-- 链接 MeowPAPI 静态库，提供侧边栏显示和占位符管理
add_rules("mode.debug", "mode.release")

-- 优先使用本地缓存的 liteldev-repo，回退到 GitHub 远程
local function find_local_repo()
    local candidates = {
        path.join(os.projectdir(), ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
        path.join(os.projectdir(), "..", "RecipeExporter", ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
        path.join(os.projectdir(), "..", "MeowEntityModel", ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
        path.join(os.projectdir(), "..", ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
    }
    for _, p in ipairs(candidates) do
        if os.exists(p) then return p end
    end
    return nil
end
local local_repo = find_local_repo()
if local_repo then
    add_repositories("liteldev-repo " .. local_repo)
else
    add_repositories("liteldev-repo https://github.com/LiteLDev/xmake-repo.git")
end

if is_config("target_type", "server") then
    add_requires("levilamina 26.10.3", {configs = {target_type = "server"}})
else
    add_requires("levilamina 26.10.3", {configs = {target_type = "client"}})
end

add_requires("levibuildscript")
-- legacyremotecall 不再需要：MeowPAPI 静态库通过 DLL 转发所有 RemoteCall 调用
add_requires("magic_enum v0.9.7")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

option("target_type")
    set_default("server")
    set_showmenu(true)
    set_values("server", "client")
option_end()

-- 包含 MeowPAPI 静态库项目（使其成为可被 add_deps 引用的 target）
includes("../MeowPAPI/xmake.lua")

target("MeowSidebar")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker")
    add_cxflags("/EHa", "/utf-8", "/W4", "/Zm2000", "/wd4100", {force = true})
    add_defines("NOMINMAX", "UNICODE", "_AMD64_", "LL_MEMORY_OPERATORS")
    add_packages("levilamina")
    -- legacyremotecall 不再需要
    add_packages("magic_enum")
    set_exceptions("on")
    set_kind("shared")
    set_languages("c++23")

    -- 依赖 MeowPAPI 静态库（PAPI 占位符翻译 + RemoteCall 代理）
    -- 注意：不 add_deps("MeowPAPI_DLL")，否则会干扰 prelink 导致 LNK2019
    -- 需先执行 xmake build MeowPAPI_DLL 生成 embedded_dll.h 和 embedded_manifest.h
    add_deps("MeowPAPI")
    add_includedirs("../MeowPAPI/include")

    -- 插件源文件
    add_headerfiles("src/MeowSidebar.h")
    add_headerfiles("src/Config.h")
    add_headerfiles("src/SidebarManager.h")
    add_headerfiles("src/PlayerConfigManager.h")
    add_headerfiles("src/DataMigrator.h")
    add_files("src/MeowSidebar.cpp")
    add_files("src/Config.cpp")
    add_files("src/SidebarManager.cpp")
    add_files("src/PlayerConfigManager.cpp")
    add_files("src/DataMigrator.cpp")
    add_files("src/CommandRegistration.cpp")
    add_files("src/EventRegistration.cpp")
    add_files("src/MemoryOperators.cpp")

    add_includedirs("src")
    set_symbols("hidden")
    add_ldflags("/OPT:REF", "/OPT:ICF", "/DEBUG:NONE")

    if is_config("target_type", "server") then
        add_defines("LL_PLAT_S")
    else
        add_defines("LL_PLAT_C")
    end

    after_build(function (target)
        local output_dir = "$(projectdir)/"
        os.mkdir(output_dir)
        os.cp(target:targetfile(), output_dir)
        local pdb_file = path.join(path.directory(target:targetfile()), "MeowSidebar.pdb")
        if os.exists(pdb_file) then
            os.cp(pdb_file, output_dir)
        end
    end)
