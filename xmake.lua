add_rules("mode.debug", "mode.release")

-- 优先使用本地离线 liteldev-repo（可选），回退到 GitHub 远程
-- 候选 repo 必须含 levilamina 26.60.0 版本登记(26_60_0.lua)：
-- 空目录(仅 xmake 元数据骨架)与旧版 repo(无 26.60.0 登记)会导致
-- "Unknown version" 回退 main 分支依赖 → 源码构建失败。
local function find_local_repo()
    local candidates = {
        -- 服务器根目录的离线仓库（26.60 SDK，预构建 levilamina 26.60.0）
        path.join(os.projectdir(), "..", "..", ".llsdk", "liteldev-repo"),
        -- xmake 自己的仓库缓存（联网构建过一次后存在）
        path.join(os.projectdir(), ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
    }
    for _, p in ipairs(candidates) do
        if os.exists(path.join(p, "packages", "l", "levilamina", "versions", "26_60_0.lua")) then return p end
    end
    return nil
end
local local_repo = find_local_repo()
if local_repo then
    add_repositories("liteldev-repo " .. local_repo)
else
    add_repositories("liteldev-repo https://github.com/LiteLDev/xmake-repo.git")
end

-- 插件统一用 clang-cl（与 LeviLamina 本体一致；缓存里的依赖包也是这套工具链）
if is_plat("windows") then
    set_toolchains("clang-cl")
end

if is_config("target_type", "server") then
    add_requires("levilamina 26.60.0", {configs = {target_type = "server"}})
else
    add_requires("levilamina 26.60.0", {configs = {target_type = "client"}})
end

add_requires("levibuildscript")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

option("target_type")
    set_default("server")
    set_showmenu(true)
    set_values("server", "client")
option_end()

includes("../MeowPAPI/xmake.lua")

target("MeowSidebar")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker")
    add_cxflags("/EHa", "/utf-8", "/W4", "/Zm2000", "/wd4100", "/wd4244", "/wd4267", "/wd4189", {force = true})
    add_defines("NOMINMAX", "UNICODE", "_AMD64_", "LL_MEMORY_OPERATORS")
    add_packages("levilamina")
    -- 异常经 add_cxflags("/EHa") 开启（set_exceptions("on") 非合法枚举值, 已移除）
    set_kind("shared")
    set_languages("c++23")

    add_deps("MeowPAPI")
    add_includedirs("../MeowPAPI/include")

    -- HologramLib 前置（26.60 起侧边栏不再自己构造/发送协议包，全部走 HologramLib 的 ISidebar）
    -- 构建顺序：先 xmake build（在 HologramLib 目录），产出 mods/HologramLib/HologramLib.lib
    add_includedirs("../HologramLib/include")
    add_linkdirs("../HologramLib")
    add_links("HologramLib")

    add_headerfiles("src/**.h")
    add_files("src/**.cpp")

    add_includedirs("src")
    set_symbols("hidden")
    add_ldflags("/OPT:REF", "/OPT:ICF", "/DEBUG:NONE")

    if is_config("target_type", "server") then
        add_defines("LL_PLAT_S")
    else
        add_defines("LL_PLAT_C")
    end

    after_build(function (target)
        local projectdir = os.projectdir()
        -- 部署单个文件：直接 cp 失败（目标被正在跑的服务端加载占用）就改名旧文件再拷。
        -- 注意：xmake 回调沙箱里没有 pcall，用 try/catch（在回调环境里才有）。
        local function deploy(src, dst)
            if not os.isfile(src) then
                return false
            end
            local ok = false
            try {
                function ()
                    os.cp(src, dst, {force = true})
                    ok = true
                end,
                catch {
                    function (e)
                    end
                }
            }
            if ok then
                return true
            end
            local backup = dst .. ".old"
            try {
                function ()
                    os.rm(backup)
                end,
                catch {
                    function (e)
                    end
                }
            }
            try {
                function ()
                    os.mv(dst, backup)
                end,
                catch {
                    function (e)
                    end
                }
            }
            try {
                function ()
                    os.cp(src, dst, {force = true})
                    ok = true
                end,
                catch {
                    function (e)
                    end
                }
            }
            if ok then
                cprint("${yellow}[deploy]: ${reset}目标被占用，旧文件改名为 " .. backup)
            else
                print("warn: 部署失败（文件被占用且改名失败）: " .. dst)
            end
            return ok
        end

        local output_dir = path.join(projectdir, "bin", "MeowSidebar")
        os.mkdir(output_dir)
        local dll = target:targetfile()
        local pdb = path.join(path.directory(dll), "MeowSidebar.pdb")
        deploy(dll, path.join(output_dir, "MeowSidebar.dll"))
        deploy(pdb, path.join(output_dir, "MeowSidebar.pdb"))
        -- 服务器实际加载位置: mods/MeowSidebar/
        deploy(dll, path.join(projectdir, "MeowSidebar.dll"))
        deploy(pdb, path.join(projectdir, "MeowSidebar.pdb"))
    end)

-- 原生计分板占位符解析/格式化的离线单测（纯逻辑，不链接 BDS/LeviLamina）
-- 运行: xmake build scoreboard_format_test && xmake run scoreboard_format_test
target("scoreboard_format_test")
    set_kind("binary")
    set_languages("c++20")
    add_cxflags("/utf-8", "/EHsc", {force = true})
    add_includedirs("src")
    add_files("tests/scoreboard_format_test.cpp", "src/ScoreboardFormat.cpp")
    set_rundir("$(projectdir)")
target_end()
