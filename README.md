# MeowSidebar

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

BDS（Bedrock Dedicated Server）26.60 侧边栏插件，LeviLamina 原生 C++23 插件。

配置驱动渲染标题与数据行，支持逐行帧动画、每位玩家独立的自定义行、MeowPAPI 占位符，
以及把游戏内原生计分板（`/scoreboard`）以只读占位符的形式搬进侧边栏。

> 26.60 起侧边栏协议层（objective 派生 / 发包 / 回读校验）交给
> [HologramLib](https://github.com/Ieckseyr/HologramLib) 的 `ISidebar`，
> 本插件只负责「这一拍显示什么」：配置 + 占位符 + 动画索引。

---

## 功能

- **配置驱动**：`config.json` 描述标题与数据行，支持字符串、数组、数组套数组三种写法。
- **帧动画**：数组套数组即帧序列，可逐行设置切换间隔（`[[帧...], 间隔ms]`）。
- **占位符**：MeowPAPI 全部占位符（`{server_tps}`、`{pl.ping}`、`{date.Y}`…）
  以及原生计分板占位符（`{sb.<objective>.top.1.name}`），展开结果还会继续走 PAPI。
- **按玩家**：`/sidebar on|off` 开关、自定义行持久化到 `players/<xuid>.json`，
  每位玩家看到的行可以完全不同。
- **原生计分板只读适配**：读取 `/scoreboard` 的 objective / 条目 / 分数，
  提供 `/sidebar scoreboard`、`/sidebar expand` 与 5 个 RemoteCall 查询接口；**绝不写入**计分板。
- **事件驱动刷新**：`ServerLevelTickEvent` 主线程刷新，`hz` 控制刷新间隔，
  `title_refresh_interval` 单独节流标题动画；玩家进服预加载、离服清理状态。
- **健壮配置**：原子写入、UTF-8 BOM 剥离、解析失败自动备份为 `config.json.bak` 再回退默认值。
- **自动部署 MeowPAPI**：内嵌 MeowPAPI.dll，首次加载自动释放到 `plugins/MeowPAPI/` 并初始化。

## 依赖

| 组件 | 版本 / 说明 |
|---|---|
| BDS | 26.60 |
| LeviLamina | 26.60.0 |
| HologramLib | 必需，提供 `ISidebar` 协议层 |
| LegacyRemoteCall | 可选（LSE 兼容） |
| MeowPAPI | 构建时同仓源码依赖，运行时由本插件内嵌部署 |

运行环境只需要 BDS + LeviLamina + HologramLib，把 `MeowSidebar.dll` 与 `manifest.json`
放进 `plugins/MeowSidebar/` 即可，MeowPAPI 会被自动释放。

## 安装

```
plugins/
├── HologramLib/            # 必需
│   ├── HologramLib.dll
│   └── manifest.json
└── MeowSidebar/
    ├── MeowSidebar.dll
    └── manifest.json
```

首次启动会在 `plugins/MeowSidebar/` 生成默认 `config.json` 与 `players/` 目录。

## 配置

`plugins/MeowSidebar/config.json`：

```jsonc
{
  // 侧边栏刷新间隔（毫秒，最小 50）
  "hz": 800,

  // 原生计分板快照缓存 TTL（毫秒），同一刷新周期内多个 {sb.*} 共用一份快照
  "scoreboard_cache_ms": 500,

  // 标题动画推进间隔（毫秒）；0 = 不节流，每次刷新都推进
  "title_refresh_interval": 0,

  // 标题：字符串 / 数组（顺序拼接）/ 数组套数组（帧动画）
  "title": [
    "§1§g",
    ["|", "/", "-", "\\"],
    "§9§b信标公益社区",
    ["|", "/", "-", "\\"]
  ],

  // 数据行：每一项可以是字符串、帧数组，或 [帧数组, 间隔ms]
  "data": [
    "§f§l | §a延迟{pl.ping}ms",
    "§f§l | §b在线：{server_online}",
    "§f§l | §bTPS: §a{server_tps}",
    ["§f§l | §d时长: {playtime.days}§g天", "§f§l | §d时长: {playtime.hours}§g时"],
    [["§a· ", "§b· ", "§c· "], 1000],
    "{mtps_tpa}"
  ]
}
```

规则：

- `title` 的各段按顺序拼接；展开计分板占位符后再交给 MeowPAPI 翻译。
- `data` 每项展开后若含 `\n`（如 `{sb.sidebar.lines}`）会拆成多行。
- 空行会被跳过；重复行自动追加空格去重，避免客户端合并同分条目。
- `"{...}"` 在 JSON 里是普通字符串，直接写占位符即可。

## 命令

| 命令 | 说明 | 权限 |
|---|---|---|
| `/sidebar` | 帮助 | 玩家 |
| `/sidebar on` / `off` | 开关自己的侧边栏（别名 `开`/`关`/`true`/`false`） | 玩家 |
| `/sidebar add <文本>` | 添加自定义行 | 玩家 |
| `/sidebar del <序号>` | 删除自定义行 | 玩家 |
| `/sidebar clear` | 清空自定义行 | 玩家 |
| `/sidebar list` | 列出自己的自定义行 | 玩家 |
| `/sidebar reload` | 重载配置 | OP / 控制台 |
| `/sidebar scoreboard [objective]` | 列出原生计分板，或查看某个 objective 前 10 名（别名 `计分板`/`sb`） | 玩家 / 控制台 |
| `/sidebar expand <文本>` | 试算 `{sb.*}` 占位符 | 玩家 / 控制台 |

## 原生计分板占位符

统一前缀 `{sb.`，长写 `{scoreboard.` 等价。

| 占位符 | 含义 |
|---|---|
| `{sb.<objective>}` | objective 显示名（同 `.title` / `.display` / `.name`） |
| `{sb.<objective>.objective}` | objective 名（同 `.id`） |
| `{sb.<objective>.count}` | 条目数量（同 `.size`） |
| `{sb.<objective>.criteria}` | 判据（通常是 `dummy`） |
| `{sb.<objective>.slots}` | 显示槽，逗号分隔（如 `sidebar,list`） |
| `{sb.<objective>.lines}` | 全部条目，每行 `名字 分数`，`\n` 分隔 → 展开成多行 |
| `{sb.<objective>.names}` / `.scores` | 所有条目名 / 分数 |
| `{sb.<objective>.top.<N>}` | 第 N 名（1-based），输出 `名字 分数` |
| `{sb.<objective>.top.<N>.name}` / `.score` | 第 N 名的名字 / 分数 |
| `{sb.<objective>.<条目名>}` | 该条目分数；名字与关键字冲突时用 `{sb.<objective>.of.<条目名>}` |
| `{sb.<objective>.me}` / `.rank` | 当前玩家的分数 / 名次 |
| `{sb.sidebar.*}` | 同上，但目标换成「当前显示在 sidebar 槽的那个 objective」 |
| `{sb.list}` | 所有 objective 名 |

行为约定：

- 排序为分数降序、同分名字升序；`top.N` 上限 100，`lines` 最多 30 条。
- 引用的 objective 不存在 → **保留原文**，便于发现拼写错误。
- 条目 / 名次不存在，或原生计分板没显示在 sidebar 槽 → 输出空串。
- objective 真叫 `sidebar` 时用 `{sb.objective.sidebar}` 指定。
- 读取只发生在确实使用 `{sb.*}` 时；非主线程调用只返回缓存快照，不触碰 BDS 内部结构。

## RemoteCall API

命名空间 `MeowSidebar`（LSE 侧用 `ll.import("MeowSidebar", "<函数>")`）：

| 函数 | 返回 | 说明 |
|---|---|---|
| `reloadConfig()` | `bool` | 重载配置 |
| `getPlayerLines(xuid)` | JSON 数组字符串 | 玩家自定义行 |
| `addPlayerLine(xuid, line)` | `bool` | 添加自定义行 |
| `updatePlayerLine(xuid, index, line)` | `bool` | 修改指定行 |
| `delPlayerLine(xuid, index)` | `bool` | 删除指定行 |
| `clearPlayerLines(xuid)` | `bool` | 清空自定义行 |
| `getPlayerLineCount(xuid)` | `int` | 自定义行数 |
| `getPlayerSidebarEnabled(xuid)` | `bool` | 开关状态 |
| `setPlayerSidebarEnabled(xuid, enabled)` | `bool` | 设置开关 |
| `getDefaultShowSidebar()` | `bool` | 默认显示状态 |
| `setDefaultShowSidebar(enabled)` | `bool` | 兼容用占位接口 |
| `refreshPlayerByXuid(xuid)` | `bool` | 立即刷新某玩家 |
| `getScoreboardObjectives()` | JSON 数组字符串 | `[{name, display, criteria, count, slots:[]}]` |
| `getScoreboardObjective(objective)` | JSON 对象字符串 | 含 `entries:[{name, score, type, hidden}]`，不存在为 `{}` |
| `getScoreboardSidebar()` | JSON 对象字符串 | 当前 sidebar 槽原生计分板，无则 `{}` |
| `getScoreboardScore(objective, entry)` | 字符串 | 条目分数，找不到为空串 |
| `expandScoreboard(text, playerName)` | 字符串 | 用同一套 `{sb.*}` 语法展开文本 |

```javascript
// LSE 示例：读取当前 sidebar 槽的原生计分板
const sb = ll.import("MeowSidebar", "getScoreboardSidebar");
let data = {};
try { data = JSON.parse(sb()); } catch (e) {}
if (data.name) logger.info(`计分板: ${data.name} (${data.display})，共 ${data.count} 条`);

// 试算占位符
const expand = ll.import("MeowSidebar", "expandScoreboard");
logger.info(expand("金币榜第一: {sb.money.top.1.name}", "Steve"));
```

RemoteCall 建议在主线程（命令 / 事件回调）里调用；非主线程只能拿到缓存快照。
占位符注册请使用 MeowPAPI 的接口（`ll.import("MeowPAPI", ...)`，见其 API.md）。

## 项目结构

```
MeowSidebar/
├── manifest.json                  # 插件清单
├── xmake.lua                      # 构建脚本
├── src/
│   ├── MeowSidebar.{h,cpp}        # 插件主模块：加载/启用/停用、RemoteCall 导出
│   ├── SidebarManager.{h,cpp}     # 刷新调度、渲染、动画索引、per-player 状态
│   ├── Config.{h,cpp}             # config.json 读写（原子写 / BOM / 备份回退）
│   ├── PlayerConfigManager.{h,cpp}# 玩家自定义行持久化
│   ├── ScoreboardAdapter.{h,cpp}  # 原生计分板只读适配（主线程读取 + TTL 缓存）
│   ├── ScoreboardFormat.{h,cpp}   # {sb.*} 纯解析/格式化（无 BDS 依赖，可离线单测）
│   ├── CommandRegistration.cpp    # /sidebar 命令
│   ├── EventRegistration.cpp      # 玩家事件监听
│   ├── DataMigrator.{h,cpp}       # BetterSidebar 玩家数据迁移
│   └── MemoryOperators.cpp        # LeviLamina 内存操作符
└── tests/
    └── scoreboard_format_test.cpp # 计分板占位符离线单测
```

## 构建

需要 xmake、LLVM（clang-cl）与 MSVC 构建工具链。

本仓库是插件工程，构建时依赖两个同级目录（不在本仓库内）：

```
plugins/
├── MeowPAPI/      # 占位符引擎源码 + 内嵌 DLL 生成
├── HologramLib/   # 侧边栏协议层（提供 HologramLib.lib）
└── MeowSidebar/   # 本仓库
```

```bash
# 1) 先构建 HologramLib，产出 plugins/HologramLib/HologramLib.lib
cd plugins/HologramLib && xmake build

# 2) 构建本插件
cd ../MeowSidebar && xmake build
# 产物：build/windows/x64/release/MeowSidebar.dll
# 同时复制到 bin/MeowSidebar/ 与插件目录

# 3) 计分板占位符离线单测
xmake build scoreboard_format_test
xmake run scoreboard_format_test
```

`xmake.lua` 会优先使用本地 `liteldev-repo` 缓存（`.xmake/…` 或服务器根目录 `.llsdk/liteldev-repo`），
找不到时回退到 GitHub 远程仓库；候选仓库必须包含 `levilamina 26.60.0` 版本登记。

## 说明

- 侧边栏不写服务端计分板：objective 由 HologramLib 按玩家派生，不污染存档，
  也不会和计分板插件抢数据。
- 兼容性补丁：LeviLamina 26.40+ 把事件类移进 inline namespace 后，MSVC 编译的
  插件与官方 Clang 构建的 `EventId` 哈希不一致；本插件显式特化了
  `getEventId<...>` 强制对齐 Clang 侧名称，保证 tick / 玩家事件能收到。
- `players/<xuid>.json`、`config.json`、`.migrated` 均为运行时数据，不纳入版本控制。

## 许可证

[MIT](LICENSE) © 2026 Ieckseyr
