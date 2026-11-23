# CreatorX 命令速查手册

> - **权限**：所有命令与工具需在游戏内以 **OP（`GameDirectors`）** 身份执行，不支持服务器控制台直接运行。
> - **短命令**：`command.shortCommands: true`（默认开启）时，第 1、3、5、6 节命令均可省略 `/cx`（如 `/pos1`、`/set stone`、`/undo`）；第 4 节**笔刷与工具命令**需带 `/cx` 前缀。

---

## 1. 选区与统计命令

选区为闭区间三维长方体，按维度隔离。魔杖操作：**左键设点 1**、**右键设点 2**、**蹲下 + 右键打开主菜单**。

| 命令 | 语法 | 说明 |
| --- | --- | --- |
| `wand` | `/cx wand` | 领取带专属标记的选区魔杖（默认木斧） |
| `togglewand` | `/cx togglewand` | 停用或重新启用魔杖功能 |
| `pos1` / `pos2` | `/cx pos1 [x y z]`<br>`/cx pos2 [x y z]` | 设点 1 / 点 2（省略坐标取脚下，支持 `~` 与 `^`） |
| `hpos1` / `hpos2` | `/cx hpos1` / `/cx hpos2` | 将准星瞄准的方块设为点 1 / 点 2（默认最远 128 格） |
| `size` | `/cx size` | 查看选区坐标、尺寸与体积 |
| `desel` | `/cx desel` | 清除当前选区与边框 |
| `expand` | `/cx expand <格数> [反向格数] [方向]`<br>`/cx expand vert` | 向指定方向扩大选区；`vert` 贯通世界最低到最高高度 |
| `contract` | `/cx contract <格数> [反向格数] [方向]` | 向指定方向收缩选区（至少保留 1 格） |
| `shift` | `/cx shift <格数> [方向]` | 整体平移选区框（不移动方块） |
| `outset` / `inset` | `/cx outset <格数> [-h \| -v]`<br>`/cx inset <格数> [-h \| -v]` | 六面同时外扩或内缩；`-h` 仅水平，`-v` 仅垂直 |
| `count` | `/cx count <掩码>` | 统计选区内符合掩码的方块数量 |
| `distr` | `/cx distr [-c]` | 列出前 12 种方块数量；`-c` 统计当前剪贴板 |

**方向参数**：`me`（默认，当前视线主方向）、`up`（+Y）、`down`（-Y）、`north`（-Z）、`south`（+Z）、`east`（+X）、`west`（-X）。

---

## 2. 图案（Pattern）与掩码（Mask）

### 图案（放什么方块）
- **方块名**：`stone`、`minecraft:oak_planks`（可省略 `minecraft:`）
- **手持方块**：`hand`（主手）、`offhand`（副手）
- **精确状态**：`wool[color=red]`、`oak_stairs["upside_down_bit"=true,"weirdo_direction"=2]`
- **随机混合**：`60%stone,30%cobblestone,andesite`（未写百分比项平分 `100%` 剩余比例）

### 掩码（匹配哪些方块）
- **方块 / 并集（或）**：`dirt,grass_block`、`wool[color=red]`（不写 `[...]` 匹配全部变体）、`hand`、`offhand`
- **内置集合**：`#existing`（非空气）、`#solid`（实心方块）、`#air`（空气）
- **取反（非）**：`!air`、`!water,lava`、`!#solid`
- **交集（且）**：空格分隔多条件，如 `#solid !bedrock`、`!air !water`

---

## 3. 区域编辑与几何生成命令

### 区域编辑（需先设选区）

| 命令 | 语法 | 说明 |
| --- | --- | --- |
| `set` | `/cx set <图案>` | 填充整个选区（单方块名支持命令补全） |
| `replace` | `/cx replace [原方块掩码] <新图案>` | 替换匹配方块；省略掩码默认替换 `#existing` |
| `walls` | `/cx walls <图案>` | 生成四周垂直围墙 |
| `faces` / `outline` | `/cx faces <图案>` / `/cx outline <图案>` | 生成六个外表面（两者等价） |
| `hollow` | `/cx hollow [厚度] [内部图案]` | 保留外壳（默认厚 1 格），内部填为指定图案（默认 `air`） |
| `overlay` | `/cx overlay <图案>` | 在每列最高实心方块正上方铺设一层 |
| `center` | `/cx center <图案>` | 在选区几何中心放置方块 |
| `move` | `/cx move [格数] [方向] [-a] [-s] [-e] [-m <掩码>]` | 移动选区内建筑并清空原位 |
| `stack` | `/cx stack [次数] [方向] [-a] [-s] [-e] [-m <掩码>]` | 沿指定方向连续复制堆叠 |

> `move` / `stack` 参数：`-a` 跳过空气，`-s` 选区跟随，`-e` 含非玩家实体，`-m <掩码>` 仅处理匹配方块（须放最后）。

### 几何形状生成（默认以脚下为基准，加 `-c` 改以选区中心为基准）

| 命令 | 语法 | 说明 |
| --- | --- | --- |
| `sphere` / `hsphere` | `/cx sphere <图案> <r>[,ry,rz] [-r] [-c] [-m <掩码>]` | 实心/空心球或椭球；`-r` 将球底抬至基准点上方 |
| `cyl` / `hcyl` | `/cx cyl <图案> <r>[,rz] [高度] [-c] [-m <掩码>]` | 实心/空心圆柱或椭圆柱（默认高度 1） |
| `pyramid` / `hpyramid` | `/cx pyramid <图案> <层数> [-c] [-m <掩码>]` | 实心/空心四棱锥金字塔 |

---

## 4. 笔刷与手持工具命令（须带 `/cx` 前缀）

将功能绑定到当前主手拿着的物品上，绑定后右键对准远处方块使用（`tool` 还支持左键）。

| 命令 | 语法 | 说明 |
| --- | --- | --- |
| `brush sphere` | `/cx brush sphere <图案> [半径] [-h]` | 球形笔刷（`-h` 空心） |
| `brush cyl` | `/cx brush cyl <图案> [半径] [高度] [-h]` | 圆柱笔刷（`-h` 空心） |
| `brush smooth` | `/cx brush smooth [半径] [次数]` | 地形平滑笔刷 |
| `brush gravity` | `/cx brush gravity [半径]` | 重力下落笔刷（使悬空方块落到下方实心面） |
| `brush clipboard` | `/cx brush clipboard [-a] [-o]` | 剪贴板笔刷（`-a` 跳过空气，`-o` 原位） |
| `tool info` | `/cx tool info` | 信息工具：左/右键查看方块 ID、状态与方块实体 NBT |
| `tool repl` | `/cx tool repl <图案>` | 单格替换工具：左键吸取目标方块，右键替换为指定图案 |
| `size` | `/cx size <1-64>` | 调整当前球形/圆柱/平滑/重力笔刷半径 |
| `mat` | `/cx mat <图案>` | 修改当前笔刷或替换工具的材质 |
| `mask` | `/cx mask [掩码]` | 设置笔刷过滤掩码（省略掩码即清除） |
| `range` | `/cx range <1-128>` | 设置当前笔刷或工具的射线距离 |
| `none` | `/cx none` | 解绑当前手持物品的笔刷或工具 |

---

## 5. 剪贴板与原理图命令

| 命令 | 语法 | 说明 |
| --- | --- | --- |
| `copy` | `/cx copy [-e] [-m <掩码>]` | 复制选区并记录相对脚下的偏移 |
| `cut` | `/cx cut [留下的图案] [-e] [-m <掩码>]` | 复制选区后将原位填为指定图案（默认 `air`） |
| `paste` | `/cx paste [-a] [-o] [-s] [-p] [-e] [-m <掩码>]` | `-a` 跳空气，`-o` 原位，`-s` 选中，`-p` 预览，`-e` 含实体，`-m` 掩码（放最后） |
| `pastepreview` | `/cx pastepreview <dx> <dy> <dz> [Y轴角度]` | 预览期间平移和旋转落点 |
| `confirm` | `/cx confirm` | 确认当前粘贴预览并落地 |
| `rotate` | `/cx rotate <Y角度> [X角度] [Z角度]` | 按 90° 倍数三维旋转剪贴板（同步旋转方块朝向与实体） |
| `flip` | `/cx flip [方向]` | 沿指定轴镜像翻转（省略默认当前朝向） |
| `clearclipboard` | `/cx clearclipboard` | 清空剪贴板 |
| `schem` | `/cx schem export <文件> [格式] [-e]`<br>`/cx schem place <文件> [格式] [-a] [-e]`<br>`/cx schem save/load/delete <文件> [格式]`<br>`/cx schem list [关键词]` | 原理图导出、放置、保存、加载、删除与列表。支持 `.mcstructure`、`.cxs`、`.schem`、`.litematic`，文件位于 `mods/creatorx/data/schematics/` |

---

## 6. 历史、菜单与系统命令

| 命令 | 语法 | 说明 |
| --- | --- | --- |
| `undo` / `redo` | `/cx undo [步数] [玩家名或UUID]`<br>`/cx redo [步数] [玩家名或UUID]` | 撤销或重做（默认 1 步；管理员可指定他人回滚） |
| `history` | `/cx history` | 查看可撤销与可重做历史列表 |
| `clearhistory` | `/cx clearhistory` | 清空自己的撤销与重做记录 |
| `cancel` | `/cx cancel` | 中止当前运行任务，或取消粘贴预览 |
| `menu` | `/cx menu` | 打开 CreatorX 图形主菜单 |
| `reload` | `/cx reload` | 热重载 `config.jsonc`、语言包与黑名单 |
| `help` | `/cx help [命令名]` | 查看命令列表或指定命令语法 |

---

## 7. 客户端快捷键（Windows 客户端）

可在游戏「设置 → 键盘和鼠标」中修改键位：

| 默认按键 | 对应命令 | 作用 |
| :---: | --- | --- |
| `F6` | `/cx hpos1` | 准星指向方块设为点 1 |
| `F7` | `/cx hpos2` | 准星指向方块设为点 2 |
| `F8` | `/cx undo` | 撤销上一步 |
| `F9` | `/cx redo` | 重做上一步 |
| `F10` | `/cx menu` | 打开 CreatorX 主菜单 |
