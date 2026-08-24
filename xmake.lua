add_rules("mode.debug", "mode.release")

-- 优先使用本地缓存的 liteldev-repo，回退到 GitHub 远程
local function find_local_repo()
    local candidates = {
        path.join(os.projectdir(), ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
        path.join(os.projectdir(), "..", ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
        path.join(os.projectdir(), "..", "MeowPAPI", ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
        path.join(os.projectdir(), "..", "MeowAchievement", ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
        path.join(os.projectdir(), "..", "RecipeExporter", ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
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
    set_exceptions("on")
    set_kind("shared")
    set_languages("c++23")

    add_deps("MeowPAPI")
    add_includedirs("../MeowPAPI/include")

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
        -- 复制到 bin/MeowSidebar/ (modpack 输出)
        local output_dir = "$(projectdir)/bin/MeowSidebar/"
        os.mkdir(output_dir)
        os.cp(target:targetfile(), output_dir)
        local pdb_file = path.join(path.directory(target:targetfile()), "MeowSidebar.pdb")
        if os.exists(pdb_file) then
            os.cp(pdb_file, output_dir)
        end
        -- 同时复制到插件源码目录 (服务器实际加载位置: plugins/MeowSidebar/)
        os.cp(target:targetfile(), "$(projectdir)/")
        if os.exists(pdb_file) then
            os.cp(pdb_file, "$(projectdir)/")
        end
    end)
