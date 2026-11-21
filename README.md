# CreatorX

CreatorX 是基于 LeviLamina 的 Minecraft 基岩版WorldEdit 式快速建造模组。提供长方体选区、图形化表单菜单、图案与掩码替换、剪贴板复制粘贴、多步撤销重做、分 tick 异步执行与大操作确认等功能。

> 详细使用指南见 [docs/USAGE.md](docs/USAGE.md)，功能规划与架构见 [docs/FEATURES.md](docs/FEATURES.md)、[docs/DESIGN.md](docs/DESIGN.md)。

## 快速上手

1. 领取魔杖或打开菜单：输入 `/cx wand` 领取选区魔杖，或输入 `/cx menu`（也可以拿着魔杖**蹲下右键**）打开图形化主菜单。
2. 框选区域：手持魔杖**左键**方块设为点 1，**右键**方块设为点 2；也可用 `/cx pos1`、`/cx pos2` 或 `/cx hpos1`、`/cx hpos2` 设点。
3. 执行建造：在菜单中选择操作，或执行 `/cx set stone`、`/cx replace dirt grass_block`、`/cx walls glass`、`/cx copy`、`/cx paste`。
4. 撤销恢复：操作失误可执行 `/cx undo` 撤销、`/cx redo` 重做；大操作执行中可执行 `/cx cancel` 中止。

> 默认需要 OP 权限。启用 `command.shortCommands`（默认开启）后，所有子命令均可省略 `/cx` 前缀（例如 `/pos1`、`/set stone`、`/undo`）。

## 已实现功能概览

- **选区**：魔杖左右键设点、脚下/坐标设点（`/cx pos1`、`/cx pos2`，支持 `~` `^`）、准星视线设点（`/cx hpos1`、`/cx hpos2`）、尺寸查看（`/cx size`）、清除（`/cx desel`）、扩展/收缩（`/cx expand`、`/cx expand vert`、`/cx contract`）、魔杖开关（`/cx togglewand`）、线框/粒子选区可视化。
- **区域编辑**：填充（`/cx set`）、替换（`/cx replace`）、四壁（`/cx walls`）、六面外壳（`/cx faces` / `/cx outline`）。
- **图案与掩码**：支持单方块、方块状态（`wool[color=red]`）、随机权重（`60%stone,40%andesite`）、手持方块（`hand` / `offhand`）、掩码过滤（`!air`、`#existing`、`#solid`、`#air` 与多 Term 空格交集）及拼写纠错提示。
- **剪贴板**：复制（`/cx copy`）、剪切（`/cx cut [pattern]`）、粘贴（`/cx paste [-a] [-o] [-s]`）、清空（`/cx clearclipboard`），自动保留含水层第二层与箱子/告示牌等方块实体 NBT。
- **历史记录**：多步撤销（`/cx undo [times]`）、重做（`/cx redo [times]`）、清空历史（`/cx clearhistory`），支持任务中途取消后保留已修改部分的撤销能力。
- **图形化菜单**：`/cx menu` 或蹲下右键魔杖打开，内置方块关键词搜索、最近 10 条图案/掩码记录、交互式选区调整与确认弹窗，全程无需手打长命令。
- **性能与安全**：分 tick 预算调度与动作栏进度条、大操作弹窗确认、单次体积与改动上限、禁止放置方块黑名单。

## 客户端快捷键

在客户端安装 CreatorX 后，可在游戏「设置 → 键盘和鼠标」中修改快捷键（自带本地化名称资源包 `creatorx_shortcuts`）。联机时需服务端同样安装 CreatorX 且玩家拥有执行 `/cx` 的权限。

| 默认按键 | 对应命令 | 操作 |
| --- | --- | --- |
| `F6` | `/cx hpos1` | 将准星指向的方块设为选区点 1 |
| `F7` | `/cx hpos2` | 将准星指向的方块设为选区点 2 |
| `F8` | `/cx undo` | 撤销上一步 |
| `F9` | `/cx redo` | 重做上一步 |
| `F10` | `/cx menu` | 打开 CreatorX 主菜单 |

## License

CC0-1.0 © LeviMC (LiteLDev)
