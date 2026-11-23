# CreatorX 功能清单与路线图

> - **使用说明**：请阅读 [README](../README.md) 与 [命令速查手册](USAGE.md)。
> - **开发说明**：请阅读 [第 7 章：开发者与编译指南](07-开发者与编译指南.md)；早期设计草案见 [DESIGN.md](DESIGN.md)。
>
> **状态说明**：✅ 已实现可用 · ⏳ 规划中。**端**：S = 服务端（含单人内置服务端）；C = 客户端增强。

---

## 1. 选区与统计

| 功能 | 命令 / 操作 | 状态 | 端 |
| --- | --- | :---: | :---: |
| 获取与开关魔杖 | `/cx wand`、`/cx togglewand` | ✅ | S |
| 魔杖设点与开菜单 | 左键点 1、右键点 2、蹲下右键菜单、远程魔杖 | ✅ | S |
| 坐标与视线设点 | `/cx pos1`、`pos2`、`hpos1`、`hpos2` | ✅ | S |
| 选区信息与清除 | `/cx size`、`/cx desel` | ✅ | S |
| 调整选区 | `/cx expand [vert]`、`contract`、`shift`、`outset`、`inset` | ✅ | S |
| 统计方块 | `/cx count <mask>`、`/cx distr [-c]` | ✅ | S |
| 选区可视化 | 粒子边框、`PrimitiveShapesPacket` 实线框、角点标记 | ✅ | S |
| 更多选区形状 | `/cx sel poly\|ellipsoid\|sphere\|cyl\|convex`、`/cx chunk` | ⏳ | S |

## 2. 区域操作与生成

| 功能 | 命令 | 状态 | 端 |
| --- | --- | :---: | :---: |
| 填充与替换 | `/cx set`、`/cx replace` | ✅ | S |
| 四壁、外壳、掏空 | `/cx walls`、`/cx faces`（`outline`）、`/cx hollow` | ✅ | S |
| 表层覆盖与中心 | `/cx overlay`、`/cx center` | ✅ | S |
| 移动与堆叠 | `/cx move`、`/cx stack`（支持 `-a`、`-s`、`-e`、`-m`） | ✅ | S |
| 球体 / 圆柱 / 金字塔 | `sphere`、`hsphere`、`cyl`、`hcyl`、`pyramid`、`hpyramid`（支持 `-r`、`-c`、`-m`） | ✅ | S |
| 地形自然化 / 平滑 / 群系 | `naturalize`、`smooth`、`line`、`snow`、`thaw`、`setbiome` 等 | ⏳ | S |
| 更多形状与文字/图像 | `cone`、`torus`、`arch`、`spiral`、`roof`、`text`、`image`、`generate` | ⏳ | S |

## 3. 剪贴板与原理图

| 功能 | 命令 / 说明 | 状态 | 端 |
| --- | --- | :---: | :---: |
| 复制 / 剪切 / 粘贴 / 清空 | `copy`、`cut`、`paste`（含 `-a`、`-o`、`-s`、`-p`、`-e`、`-m`）、`clearclipboard` | ✅ | S |
| 含水层、方块实体、实体 | 自动保留双层方块与方块实体 NBT（含大箱子配对），`-e` 含非玩家实体 | ✅ | S |
| 粘贴预览与微调 | `/cx paste -p`、`/cx pastepreview`、`/cx confirm` | ✅ | S |
| 旋转与翻转 | `/cx rotate <y> [x] [z]`、`/cx flip [方向]` | ✅ | S |
| 原理图命令与浏览器 | `/cx schem export\|place\|save\|load\|list\|delete` + GUI 浏览/搜索 | ✅ | S |
| 支持格式 | `.mcstructure`、`.cxs`、`.schem`（读 v1–v3 写 v3）、`.litematic` | ✅ | S |

## 4. 笔刷与工具

| 功能 | 命令 | 状态 | 端 |
| --- | --- | :---: | :---: |
| 形状与地形笔刷 | `/cx brush sphere\|cyl\|smooth\|gravity\|clipboard` | ✅ | S |
| 信息与替换工具 | `/cx tool info`、`/cx tool repl <pattern>` | ✅ | S |
| 参数调节与解绑 | `/cx size`、`/cx mat`、`/cx mask`、`/cx range`、`/cx none` | ✅ | S |
| 笔刷落点可视化与鼠标包 | 增强客户端显示落点目标并支持 `creatorx:brush_click` | ✅ | S/C |

## 5. 图案与掩码

| 功能 | 语法示例 | 状态 |
| --- | --- | :---: |
| 单方块 / 带状态 / 手持 | `stone`、`wool[color=red]`、`hand`、`offhand` | ✅ |
| 权重随机与拼写建议 | `60%stone,30%cobblestone,andesite`、自动编辑距离纠错 | ✅ |
| 基础与逻辑掩码 | `stone,dirt`、`!air`、`#existing`、`#solid`、`#air`、空格交集 | ✅ |
| 高级图案与掩码 | `#clipboard`、`#gradient`、`>stone`、`%30`、`$plains`、`/cx gmask` | ⏳ |

## 6. 历史、菜单、客户端与管理

| 功能 | 说明 | 状态 | 端 |
| --- | --- | :---: | :---: |
| 多步撤销 / 重做 / 历史 | `undo`、`redo`（支持指定他人）、`history`、`clearhistory`、自动磁盘溢写与持久化 | ✅ | S |
| 图形化表单菜单 | `/cx menu`（选区、填充、形状、替换、剪贴板、原理图、历史、设置、分类方块浏览器） | ✅ | S |
| 大操作确认与进度条 | 超阈值弹窗确认、动作栏进度与 ETA、`/cx cancel` 安全中止 | ✅ | S |
| 客户端快捷键与 HUD | `F6`–`F10` 可改键快捷键、`creatorx:status` 动作栏状态 HUD、远程本地坐标 | ✅ | C |
| 分 tick 调度与安全保护 | 方块数/耗时双预算、并发上限、黑名单、热重载 `/cx reload` | ✅ | S |
| 导航与实用命令 | `up`、`ascend`、`thru`、`jumpto`、`drain`、`butcher` 等 | ⏳ | S |
| 对外事件 API | `CreatorXEditStartEvent`、`CreatorXBlockFilterEvent` 等 | ⏳ | S |
