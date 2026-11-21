# CreatorX 功能清单

> 架构与实现细节见 [DESIGN.md](DESIGN.md)。
>
> **优先级**：P0 = MVP 必须；P1 = v0.2–v0.3；P2 = v0.4–v1.0；P3 = 以后再说。
> **端**：S = 服务端（含单人内置服务端）；C = 仅客户端增强。
> **风险**：标 ⚠ 的依赖某个 Spike（见 DESIGN.md §15）的结论。
> 下表使用 `/cx <子命令>`。启用短命令时，已注册的短命令也可以直接输入，例如 `/pos1`、`/set stone`。

---

## 1. 选区

| 功能 | 命令 / 操作 | 说明 | 优先级 | 端 |
| --- | --- | --- | --- | --- |
| 获取魔杖 | `/cx wand` | 发放带标记的木斧（物品可配置） | P0 | S |
| 魔杖设点 | 左键 / 右键方块 | 左键点 1、右键点 2，取消破坏/交互 | P0 | S |
| 命令设点 | `/cx pos1 [x y z]`、`/cx pos2 [x y z]` | 缺省为脚下方块，支持 `~` `^` | P0 | S |
| 视线设点 | `/cx hpos1`、`/cx hpos2` | 取准星所指方块（`traceRay`，距离可配） | P0 | S |
| 清除选区 | `/cx desel` | | P0 | S |
| 选区信息 | `/cx size` | 尺寸、体积、两个角点 | P0 | S |
| 扩展 / 收缩 | `/cx expand <n> [反向n] [方向]`、`/cx expand vert`、`/cx contract` | 方向缺省为玩家朝向 | P0 | S |
| 平移选区 | `/cx shift <n> [方向]` | 只移选区不改方块 | P1 | S |
| 外扩 / 内缩 | `/cx outset <n> [-h/-v]`、`/cx inset <n>` | 六面同时 | P1 | S |
| 选区模式 | `/cx sel cuboid\|extend\|poly\|ellipsoid\|sphere\|cyl` | | P1 | S |
| 凸包选区 | `/cx sel convex` | | P3 | S |
| 区块选区 | `/cx chunk` | 选中当前区块整列 | P1 | S |
| 统计方块 | `/cx count <mask>` | | P1 | S |
| 方块分布 | `/cx distr [-c]` | 按数量排序，`-c` 统计剪贴板 | P1 | S |
| 选区可视化（粒子） | 自动 | 选区棱线粒子 | P0 | S |
| 选区可视化（线框包） | 自动 | `PrimitiveShapesPacket` Box ⚠S2 | P1 | S |
| 选区可视化（客户端渲染） | 自动 | 实线框、角点高亮 ⚠S7 | P2 | C |

## 2. 区域操作

| 功能 | 命令 | 说明 | 优先级 | 端 |
| --- | --- | --- | --- | --- |
| 填充 | `/cx set <pattern>` | | P0 | S |
| 替换 | `/cx replace [from-mask] <to-pattern>` | 缺省 from = `#existing` | P0 | S |
| 四壁 | `/cx walls <pattern>` | | P0 | S |
| 外壳 | `/cx faces <pattern>`（别名 `/cx outline`） | 六个面 | P0 | S |
| 掏空 | `/cx hollow [厚度] [pattern]` | 只保留外壳 | P1 | S |
| 覆盖表层 | `/cx overlay <pattern>` | 每列最高的实心方块上方放一层 | P1 | S |
| 中心标记 | `/cx center <pattern>` | | P1 | S |
| 自然化 | `/cx naturalize` | 顶层草、3 层泥土、其余石头 | P1 | S |
| 平滑地形 | `/cx smooth [次数] [mask]` | 高度图高斯平滑 | P1 | S |
| 移动 | `/cx move [n] [方向] [-s 选区跟随] [-a 跳过空气]` | 可撤销 | P1 | S |
| 堆叠 | `/cx stack [次数] [方向] [-s] [-a]` | | P1 | S |
| 连线 | `/cx line <pattern> [粗细] [-h]` | 点 1 到点 2 | P1 | S |
| 曲线 | `/cx curve <pattern> [粗细]` | 多边形选区顶点做样条 | P2 | S |
| 铺雪 / 融雪 | `/cx snow`、`/cx thaw` | | P2 | S |
| 变绿 | `/cx green` | 泥土变草方块 | P2 | S |
| 改生物群系 | `/cx setbiome <biome>` | `LevelChunk::_setBiome`，客户端同步需验证 | P3 | S |
| 地形重生成 | `/cx regen` | 无可用 API，不做 | — | — |

## 3. 生成

| 功能 | 命令 | 说明 | 优先级 | 端 |
| --- | --- | --- | --- | --- |
| 球 / 空心球 | `/cx sphere`、`/cx hsphere <pattern> <r>[,ry,rz] [-r 抬高]` | 以脚下为中心 | P1 | S |
| 圆柱 / 空心圆柱 | `/cx cyl`、`/cx hcyl <pattern> <r>[,rz] [高度]` | | P1 | S |
| 金字塔 | `/cx pyramid`、`/cx hpyramid <pattern> <层数>` | | P1 | S |
| 圆锥 / 圆环 / 拱 | `/cx cone`、`/cx torus`、`/cx arch` | 基岩扩展，建筑常用 | P2 | S |
| 螺旋楼梯 | `/cx spiral <pattern> <半径> <高度> [圈数]` | 基岩扩展 | P2 | S |
| 屋顶生成 | `/cx roof <stair-block> [坡度] [-hip]` | 依据选区生成双坡/四坡屋顶，自动楼梯朝向 ⚠S4 | P2 | S |
| 文字 | `/cx text <pattern> <文字> [字号] [朝向]` | 内置像素字体 | P3 | S |
| 像素画 | `/cx image <文件> [宽度] [调色板]` | 图片 → 方块颜色量化 | P3 | S |
| 表达式生成 | `/cx generate <pattern> <表达式>` | | P3 | S |
| 树林 | `/cx forest <类型> [密度]` | 需找到可用的地物放置 API | P3 | S |

## 4. 剪贴板

| 功能 | 命令 | 说明 | 优先级 | 端 |
| --- | --- | --- | --- | --- |
| 复制 | `/cx copy [-e 含实体] [-m mask]` | 记录玩家相对位置为原点 | P0 | S |
| 剪切 | `/cx cut [pattern 留下的方块] [-e] [-m mask]` | | P0 | S |
| 粘贴 | `/cx paste [-a 跳过空气] [-o 原位] [-s 选中粘贴区] [-e] [-m mask]` | | P0 | S |
| 旋转 | `/cx rotate <y> [x] [z]` | 90° 倍数，状态同步旋转 ⚠S4 | P1 | S |
| 翻转 | `/cx flip [方向]` | | P1 | S |
| 清空 | `/cx clearclipboard` | | P0 | S |
| 方块实体 | 自动 | 箱子、告示牌、旗帜等内容随复制粘贴 | P0 | S |
| 含水方块 | 自动 | 第二层一并处理 | P1 | S |
| 实体复制 | `-e` | 借助 StructureTemplate | P2 | S |
| 粘贴预览 | `/cx paste -p` 或表单 | 假方块只给本人看，确认后执行 | P1 | S |
| 预览微调 | 方向键 / 表单 | 移动、旋转预览后再确认 | P2 | S/C |

## 5. 历史

| 功能 | 命令 | 说明 | 优先级 | 端 |
| --- | --- | --- | --- | --- |
| 撤销 / 重做 | `/cx undo [n] [玩家]`、`/cx redo [n] [玩家]` | 管理员可撤销他人 | P0 | S |
| 清空历史 | `/cx clearhistory` | | P0 | S |
| 历史磁盘溢写 | 自动 | 超内存阈值写盘，退出后保留 N 分钟 | P1 | S |
| 历史列表 | `/cx history` | 列出最近操作、改动数、时间 | P2 | S |

## 6. 图案与掩码

| 功能 | 语法示例 | 优先级 |
| --- | --- | --- |
| 单方块 / 带状态 | `stone`、`wool[color=red]`、`wool["color"="red"]` | P0 |
| 随机加权 | `60%stone,30%cobblestone,andesite` | P0 |
| 手持方块 | `hand`、`offhand` | P0 |
| 拼写建议 | 输错时提示最接近的方块名 | P0 |
| 剪贴板平铺 | `#clipboard` | P2 |
| 只改状态 | `^[facing=north]` | P2 |
| 随机状态 | `*oak_log` | P3 |
| 渐变 | `#gradient:white_wool,gray_wool,black_wool` | P3 |
| 基础掩码 | `stone,dirt`、`!air`、`#existing`、`#solid` | P0 |
| 位置掩码 | `>stone`、`<grass`、`#surface` | P1 |
| 概率掩码 | `%30` | P1 |
| 群系掩码 | `$plains` | P2 |
| 标签掩码 | `#tag:xxx` | P2 |
| 交集 | `!air >stone` | P1 |
| 全局掩码 | `/cx gmask <mask>` | P1 |

## 7. 笔刷与工具

| 功能 | 命令 | 说明 | 优先级 | 端 |
| --- | --- | --- | --- | --- |
| 球笔刷 | `/cx brush sphere <pattern> [r] [-h]` | 绑定到手持物品 | P1 | S |
| 圆柱笔刷 | `/cx brush cyl <pattern> [r] [h] [-h]` | | P1 | S |
| 平滑笔刷 | `/cx brush smooth [r] [次数]` | | P1 | S |
| 重力笔刷 | `/cx brush gravity [r]` | 让悬空方块落下 | P2 | S |
| 剪贴板笔刷 | `/cx brush clipboard [-a] [-o]` | 点哪贴哪 | P2 | S |
| 侵蚀 / 填充笔刷 | `/cx brush erode`、`/cx brush fill` | | P3 | S |
| 笔刷参数 | `/cx size`、`/cx mat <pattern>`、`/cx mask <mask>`、`/cx range <n>` | 修改当前笔刷 | P1 | S |
| 解绑 | `/cx none` | | P1 | S |
| 信息工具 | `/cx tool info` | 显示方块名、状态、NBT | P1 | S |
| 远程魔杖 | `/cx tool farwand` | 远距离设点 | P1 | S |
| 替换工具 | `/cx tool repl <pattern>` | 右键替换单个方块，左键吸取 | P2 | S |
| 魔杖开关 | `/cx togglewand` | 临时停用魔杖拦截 | P0 | S |

## 8. 导航

| 功能 | 命令 | 优先级 |
| --- | --- | --- |
| 上升到平台 | `/cx up <n>`（脚下放玻璃） | P1 |
| 上一层 / 下一层 | `/cx ascend [n]`、`/cx descend [n]` | P1 |
| 到天花板 | `/cx ceil` | P2 |
| 穿墙 | `/cx thru` | P2 |
| 传送到视线点 | `/cx jumpto`（指南针左键） | P1 |
| 脱困 | `/cx unstuck` | P2 |

## 9. 实用命令

| 功能 | 命令 | 优先级 |
| --- | --- | --- |
| 抽干液体 | `/cx drain <r>` | P1 |
| 修复水 / 岩浆 | `/cx fixwater <r>`、`/cx fixlava <r>` | P2 |
| 灭火 | `/cx ex [r]` | P2 |
| 清除上方 / 下方 | `/cx removeabove [size] [h]`、`/cx removebelow` | P1 |
| 附近清除 / 替换 | `/cx removenear <mask> [r]`、`/cx replacenear <r> <from> <to>` | P1 |
| 清除生物 | `/cx butcher [r] [-a 含动物]` | P2 |
| 清除掉落物 | `/cx remove items [r]` | P2 |
| 计算器 | `/cx calc <表达式>` | P3 |
| 取消任务 | `/cx cancel` | P0 |
| 改动上限 | `/cx limit [n]` | P1 |
| 物理开关 | `/cx fast`、`/cx perf physics on\|off` | P1 |
| 帮助 | `/cx help [命令]` | P0 |

## 10. 原理图

| 功能 | 命令 | 说明 | 优先级 |
| --- | --- | --- | --- |
| 保存 | `/cx schem save <名称> [格式]` | 保存剪贴板 | P1 |
| 加载 | `/cx schem load <名称>` | 读入剪贴板 | P1 |
| 列表 / 搜索 | `/cx schem list [页] [关键字]` | 名称补全 | P1 |
| 删除 | `/cx schem delete <名称>` | | P1 |
| `.mcstructure` | 读 / 写 | 基岩原生，结构方块和其他工具通用 | P1 |
| `.cxs` | 读 / 写 | 自有格式，带作者、描述、标签 | P1 |
| `.schem`（Sponge） | 读 | Java 版导入，需方块映射表 | P2 |
| `.schem` | 写 | | P3 |
| 公共 / 私有目录 | 配置 | | P2 |
| 原理图浏览器 | 表单 | 分页、详情、加载、删除 | P2 |

## 11. 便捷化（基岩特色）

| 功能 | 说明 | 优先级 | 端 |
| --- | --- | --- | --- |
| 主菜单 | `/cx menu` 或蹲下 + 右键魔杖打开表单主页（✅ 已实现，`wand.sneakOpensMenu` 可关） | P1 | S |
| 操作表单 | 填充 / 替换 / 形状 / 堆叠 等用表单填参数，默认值取手持方块（✅ 选区、填充、替换、剪贴板、历史、设置、帮助；形状 / 堆叠待对应功能） | P1 | S |
| 最近使用 | 表单下拉列出最近 10 个图案（✅ 图案与掩码分开记录） | P1 | S |
| 大操作确认 | 超阈值弹确认框，显示预计改动数和耗时 | P0 | S |
| 进度条 | 动作栏显示百分比、剩余时间 | P0 | S |
| 形状预览 | 生成前假方块预览 | P2 | S |
| 手机友好 | 全部功能都能通过表单 + 魔杖完成，不必打长命令 | P1 | S |
| 命令补全 | 子命令、方向、原理图名都有补全 | P0 | S |
| 快捷键 | F6/F7 准星设点，F8/F9 撤销与重做，F10 打开主菜单。可在键盘和鼠标设置里改键 ⚠S8 | P2 | C |
| HUD | 选区尺寸、当前笔刷、任务进度 | P2/P3 | C |
| 个人设置 | 魔杖物品、可视化模式、语言、粒子开关，持久化 | P1 | S |

## 12. 管理与安全

| 功能 | 说明 | 优先级 |
| --- | --- | --- |
| 命令权限 | 默认 OP；按命令组配置 | P0 |
| 限额 | 选区体积、单次改动数、笔刷半径、多边形点数 | P0 |
| 权限组限额 | 不同组不同限额 | P1 |
| 方块黑名单 | 禁止放置命令方块、结构方块、屏障等 | P0 |
| 维度限制 | 可禁止在某维度使用 | P1 |
| 保护插件接口 | 发布可取消事件 + 区域过滤器 | P1 |
| 操作日志 | 谁、何时、哪里、改了多少，写日志文件 | P1 |
| 并发控制 | 每人 1 个任务，全局上限 | P0 |
| 关服安全 | 关服时中止任务、落盘历史 | P1 |
| 热重载配置 | `/cx reload` | P1 |

## 13. 客户端增强（需客户端也装 CreatorX）

| 功能 | 说明 | 优先级 |
| --- | --- | --- |
| 握手检测 | 双方都装才启用，否则服务端自动退回粒子 | P2 |
| 选区实线框 | 世界内渲染，带角点颜色 ⚠S7 | P2 |
| 剪贴板落点框 | 显示 `/cx paste` 将落在哪里 | P2 |
| 笔刷落点 | 显示笔刷作用范围 | P3 |
| 快捷键 | 见第 11 节 | P2 |
| HUD 叠加 | 见第 11 节 | P2/P3 |
| 本地测量 | 未装服务端时也能测距离、显示坐标 | P3 |

## 14. 对外 API

| 功能 | 说明 | 优先级 |
| --- | --- | --- |
| `CreatorXEditStartEvent` | 可取消，含玩家、维度、包围盒、类型、预计改动数 | P1 |
| `CreatorXBlockFilterEvent` | 保护插件提交区域过滤函数 | P1 |
| `CreatorXEditFinishedEvent` | 完成统计 | P2 |
| 原生 API | 其他模组读选区、提交批量写入 | P3 |

---

## MVP（v0.1）验收清单

- [x] 模板重命名为 `creatorx`，双目标编译通过
- [x] `/cx wand`、魔杖左右键、`/cx pos1`、`/cx pos2`、`/cx hpos1`、`/cx hpos2`、`/cx desel`、`/cx size`、`/cx expand`、`/cx contract`
- [x] `/cx set`、`/cx replace`、`/cx walls`、`/cx faces`
- [x] `/cx copy`、`/cx cut`、`/cx paste`（含方块实体，无旋转）、`/cx clearclipboard`
- [x] `/cx undo`、`/cx redo`、`/cx clearhistory`（`/cx undo [n] [玩家]` 的玩家参数未做）
- [x] 图案：单方块、带状态、加权随机、`hand`；掩码：列表、`!`、`#existing`
- [x] 分 tick 执行、动作栏进度、`/cx cancel`、大操作确认
- [x] 限额、黑名单、命令权限（按命令组配置权限未做，统一 OP）
- [x] 粒子选区可视化
- [x] 中英文语言文件
- [ ] 100×100×100 填充期间 TPS ≥ 15，撤销后与原世界逐方块一致（待进游戏实测）
