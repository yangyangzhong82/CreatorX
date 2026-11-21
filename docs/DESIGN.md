# CreatorX 开发文档

> 一个面向 Minecraft 基岩版（LeviLamina）的 WorldEdit 式快速建造模组。
> 基线：LeviLamina 1.2.0（`lip.toml` 版本 26.60.8，对应 BDS 1.26.60），本仓库模板 `tooth.json` 依赖 `26.60.*`。
> 头文件来源：`D:\kaifa\LeviLamina-1.2.0\LeviLamina\src`、`src-client`、`src-server`。
> 功能明细与优先级见 [FEATURES.md](FEATURES.md)。

---

## 1. 目标与定位

### 1.1 目标

1. 复刻 Java 版 WorldEdit 的核心体验：选区 → 操作 → 撤销，使用 `/cx set`、`/cx copy`、`/cx paste` 等命令。
2. 针对基岩版做"便捷化"：表单 GUI、手机触屏友好、粘贴预览确认、进度提示、手持方块即图案。
3. 大规模编辑不卡服：分 tick 预算执行，可取消，可撤销。
4. 一套代码两个目标：
   - **server**（BDS）：完整功能，服务端权威。
   - **client**（基岩版客户端）：单人/局域网主机下完整功能（内置服务端），外加客户端增强（选区线框渲染、快捷键、HUD）。

### 1.2 非目标（首版不做）

- 地形重新生成（`/cx regen`）：没有可用的区块重新生成 API。
- 脚本引擎（CraftScript）。
- 与 Java 版 1:1 的表达式生成器（`/cx generate`）——放到 P3。

---

## 2. 运行形态

| 场景 | 服务端装 CreatorX | 客户端装 CreatorX | 可用能力 |
| --- | --- | --- | --- |
| A. BDS + 原版客户端 | 是 | 否 | 全部编辑功能；可视化靠粒子 / `PrimitiveShapesPacket` / 假方块 |
| B. BDS + 装了 CreatorX 的客户端 | 是 | 是 | A 的全部 + 线框渲染、快捷键、HUD（通过 LL 自定义包同步） |
| C. 单人 / 局域网主机 | — | 是 | 内置服务端跑编辑逻辑 + 客户端增强 |
| D. 第三方服务器（未装） | 否 | 是 | 不能编辑（世界不归客户端管）；仅本地辅助（测量、坐标显示） |

> 依据：`src/` 下的通用事件 Hook（`GameMode::$useItemOn`、`ServerNetworkHandler::$handle` 等）在 client 目标中同样编译，只对内置服务端生效；连接远程服务器时 `ll::service::getServerInstance()` 为空。

---

## 3. 总体架构

```
┌────────────────────────── 交互层 ──────────────────────────┐
│ 命令(/cx, /xxx)  表单GUI  物品工具(魔杖/笔刷)  快捷键(客户端)             │
└──────────────┬─────────────────────────────────────────────┘
               │ 解析 → Pattern / Mask / Region / 参数
┌──────────────▼──────────── 会话层 ─────────────────────────┐
│ SessionManager → PlayerSession(选区、剪贴板、历史、工具、限额、偏好)   │
└──────────────┬─────────────────────────────────────────────┘
               │ 提交 EditJob
┌──────────────▼──────────── 核心层（尽量与 MC 解耦）──────────┐
│ Region  Pattern  Mask  Transform  Clipboard  Generator/Operation │
│ EditSession(变更缓冲) → ChangeSet(撤销记录) → History            │
└──────────────┬─────────────────────────────────────────────┘
               │ 按区块分批
┌──────────────▼──────────── 执行与平台层 ───────────────────┐
│ JobScheduler(每 tick 预算、协程、取消)  WorldAdapter(BlockSource)   │
│ ChunkGuard(加载检测/常加载区)  Visualizer  Permission  Storage     │
└────────────────────────────────────────────────────────────┘
               │
         LeviLamina / BDS API
```

设计原则：

- **核心层不直接碰 MC 类型**。方块用 `BlockId`（见 §5.1）表示，坐标用自有 `Vec3i`。这样核心逻辑可以写 GTest 单测，不依赖游戏进程。
- **所有世界读写只在服务端线程**；纯计算（大形状生成、文件压缩/解析）放线程池。
- **编辑一律走 EditSession**，保证每个改动都可撤销、受限额约束、能被保护插件否决。

---

## 4. 目录结构

模板初始化后（`./scripts/init-template.ps1 -ModName creatorx -Tooth github.com/<owner>/CreatorX`），按下列结构组织：

```
src/
├─ mod/                 Entry、Global（模板已有）
├─ Config/  I18n/       配置与多语言（模板已有）
├─ core/                与 MC 解耦的纯逻辑
│  ├─ math/             Vec3i、Box、Direction、矩阵/旋转
│  ├─ block/            BlockId、BlockStateView、状态旋转表
│  ├─ region/           Region 接口与实现：Cuboid、Polygon2D、Ellipsoid、Cylinder、Convex
│  ├─ pattern/          Pattern 解析与实现
│  ├─ mask/             Mask 解析与实现
│  ├─ clipboard/        BlockArray、Clipboard、Transform
│  ├─ history/          ChangeSet、History、磁盘溢写
│  ├─ edit/             EditSession、EditLimit、BatchBuilder
│  └─ op/               生成器与操作（set/replace/walls/sphere/smooth/stack…）
├─ world/               WorldAdapter（BlockSource 封装）、BlockRegistry 缓存、ChunkGuard
├─ exec/                JobScheduler、EditJob、进度汇报、取消
├─ session/             PlayerSession、SessionManager、持久化偏好
├─ command/             各类命令注册、参数解析、聊天 // 拦截
├─ tool/                魔杖、信息工具、远程魔杖、笔刷
├─ ui/                  表单、消息、进度条
├─ visual/              服务端可视化：粒子、PrimitiveShapes、假方块预览
├─ schematic/           .mcstructure / .cxs / .schem 读写
├─ net/                 LL 自定义包定义（双端共用）
└─ api/                 对外事件（供领地/保护插件订阅）
src-client/
├─ render/              选区线框渲染 Hook
├─ input/               快捷键
├─ hud/                 HUD 叠加
└─ net/                 客户端收包处理
test/                   核心层 GTest（独立 xmake target）
```

`xmake.lua` 需取消注释 `src-client` 的 `add_includedirs/add_files`，并新增 `creatorx-test` 目标。

---

## 5. 核心层设计

### 5.1 方块表示 `BlockId`

```cpp
// core/block/BlockId.h
struct BlockId {
    uint32_t runtimeId; // 对应 Block::mNetworkId；0 保留给"不修改"
};
```

- 运行期与 `Block const*` 互转由 `world/BlockRegistry` 负责，内部缓存 `runtimeId → Block const*`。
  - 名称 → 方块：`Block::tryGetFromRegistry(HashedString const& name, BlockStatesType const& states)`（LLNDAPI，`mc/world/level/block/Block.h`）。
  - runtimeId → 方块：`Block::tryGetFromRegistry(uint runtimeID)`。
  - 方块 → 名称：`Block::getTypeName()`；完整状态：`Block::mSerializationId`（CompoundTag）。
  - 空气：`Block::tryGetFromRegistry("minecraft:air")`，启动时缓存。
- **持久化时不存 runtimeId**（不同版本会变），存 `mSerializationId` 的 NBT（名称 + states + version），读回用 `Block::tryGetFromRegistry(CompoundTag const&)`。
- 含水（第二层）单独用 `extraId` 表示：读 `BlockSource::getExtraBlock`，写 `BlockSource::setExtraBlock`。

### 5.2 Region（选区）

```cpp
class Region {
public:
    virtual ~Region() = default;
    virtual Box      bounds() const = 0;              // 包围盒（闭区间）
    virtual bool     contains(Vec3i p) const = 0;
    virtual uint64_t volume() const = 0;
    virtual void     forEach(FunctionRef<void(Vec3i)>) const = 0;  // 按 chunk → y → x/z 顺序
    virtual bool     expand(std::span<Vec3i const> deltas) = 0;
    virtual bool     contract(std::span<Vec3i const> deltas) = 0;
    virtual void     shift(Vec3i d) = 0;
    virtual std::unique_ptr<Region> clone() const = 0;
    virtual std::string describe() const = 0;          // /cx size 输出
};
```

实现：`CuboidRegion`、`ExtendingCuboid`（`/cx sel extend`）、`Polygon2DRegion`、`EllipsoidRegion`、`SphereRegion`、`CylinderRegion`、`ConvexPolyhedralRegion`（P3）。

选区由 `RegionSelector` 管理"点击 → 区域"的逻辑（左键设点 1、右键设点 2 / 追加多边形顶点）。选区绑定维度（`DimensionType`），跨维度时自动清空或提示。

### 5.3 Pattern（图案）

接口：`BlockId apply(Vec3i pos) const`。语法（与 Java 版对齐，兼容基岩状态写法）：

| 语法 | 含义 |
| --- | --- |
| `stone` / `minecraft:stone` | 单方块 |
| `wool[color=red]` / `wool["color"="red"]` | 带状态 |
| `50%stone,30%dirt,grass` | 随机加权（无百分比的平分剩余） |
| `hand` / `offhand` | 手持方块 |
| `pos1` / `look` | 选区点 1 / 视线所指方块 |
| `#clipboard` / `#copy` | 以剪贴板为平铺图案 |
| `^[facing=north]` | 只改状态、保留方块类型（P2） |
| `*stone` | 按方块类型随机状态（P3） |
| `#gradient:a,b,c` | 渐变（P3） |

解析器：手写递归下降，输出 AST，再编译为 `Pattern` 对象。解析错误要给出位置（`第 7 个字符：未知方块 'stnoe'，你是不是想输入 'stone'？`——用编辑距离给建议）。

### 5.4 Mask（掩码）

接口：`bool test(Vec3i pos, WorldView const&) const`。语法：

| 语法 | 含义 |
| --- | --- |
| `stone,dirt` | 属于其中之一 |
| `!air` | 取反 |
| `#existing` / `#solid` / `#air` | 存在 / 实心 / 空气 |
| `>stone` / `<stone` | 正上方 / 正下方是 stone |
| `#surface` | 地表（上方为空气的实心方块） |
| `%30` | 30% 概率 |
| `$plains` | 生物群系（`BlockSource::getBiome`） |
| `#region` | 在当前选区内 |
| `#tag:xxx` | 方块标签（`Block::hasTag`） |
| `a b` | 空格 = 交集 |

全局掩码 `/cx gmask` 存在 `PlayerSession`，EditSession 写入前统一检查。

### 5.5 Clipboard 与 Transform

```cpp
struct BlockArray {
    Vec3i                       size;
    std::vector<BlockId>        palette;     // 去重
    std::vector<uint16_t>       indices;     // size.x*size.y*size.z，按 y,z,x
    std::vector<uint16_t>       extraIndices;// 含水层，可空
    std::unordered_map<uint32_t, CompoundTag> blockEntities; // 线性下标 → NBT
    std::vector<CompoundTag>    entities;    // 可选（-e）
};
struct Clipboard {
    BlockArray data;
    Vec3i      origin;   // 复制时玩家所在位置相对于最小角的偏移
    Transform  transform;// 累积的旋转/翻转，粘贴时应用
};
```

- 方块实体复制：`BlockActor::save(CompoundTag&, SaveContext const&)`，`SaveContext{SaveUseCase::Clone}`；粘贴：`BlockActor::create(CompoundTag const&, BlockPos const&)` + `BlockSource::setBlock(pos, block, flags, std::shared_ptr<BlockActor>, syncMsg, ctx)`。
- 实体复制（`-e`）：P2，借助 `StructureTemplate::create(name, BlockSource&, BoundingBox const&, ignoreBlocks=true, ignoreEntities=false)` 捕获实体，粘贴用 `StructureTemplate::placeInWorld`。
- **旋转/翻转**：坐标变换自己算；**方块状态**（朝向、轴、楼梯、门、铁轨等）需要旋转表。头文件里没有公开的"按旋转取方块变体"函数，所以做一份数据驱动的 `state_rotation.json`：

  ```jsonc
  {
    "facing_direction":          { "type": "int6",  "rotY90": [0,1,5,4,2,3] },
    "minecraft:cardinal_direction": { "type": "enum", "rotY90": {"north":"east","east":"south","south":"west","west":"north"} },
    "pillar_axis":               { "type": "enum", "rotY90": {"x":"z","z":"x","y":"y"} },
    "rail_direction":            { "type": "table", "rotY90": [1,0,5,4,2,3,7,8,9,6] }
    // ……
  }
  ```

  状态改写用 `Block::setState<T>(id, value)` 或重新 `tryGetFromRegistry(name, states)`。作为 Spike S4 先确认是否有内建可用的变换路径（例如结构放置内部）。

### 5.6 EditSession / ChangeSet / History

```cpp
class EditSession {
public:
    EditSession(WorldAdapter&, PlayerSession*, EditLimit);
    BlockId getBlock(Vec3i) const;             // 先查缓冲，再查世界
    bool    setBlock(Vec3i, BlockId, BlockId extra = {}); // 只写入缓冲 + 检查掩码/限额/黑名单
    void    setBlockEntity(Vec3i, CompoundTag);
    uint64_t pendingCount() const;
    std::unique_ptr<EditJob> commit();         // 把缓冲按区块分组，交给 JobScheduler
};
```

- **ChangeSet**：每个变更记录 `{pos, oldId, oldExtra, newId, newExtra, oldBENbt?}`，按区块分段存储，节省内存（坐标用区块内偏移 + 区块键）。
- **History**：每个玩家一个环形栈，`/cx undo [n]`、`/cx redo [n]`。
  - 限制：条数（`historySize`）+ 总方块数（`historyMaxBlocks`）。
  - 超出内存阈值时把旧 ChangeSet 序列化（NBT + 压缩）写到 `getDataDir()/history/<uuid>/`，撤销时读回。玩家退出后保留 `historyKeepMinutes` 分钟。
- 撤销本身也是一个 EditJob（反向应用），同样分批，同样会被保护插件检查。

### 5.7 生成器与操作

每个操作是一个"生产者"：给定参数，按区块顺序产出 `(pos, BlockId)`，写入 EditSession。大操作（球体半径 > 64 等）的产出在线程池跑，结果按区块切片回传服务端线程。清单见 FEATURES.md 的"区域操作 / 生成"章节。

---

## 6. 执行层：分 tick 调度

### 6.1 JobScheduler

- 基于 LL 协程：`ll::coro::CoroTask<>` + `ll::thread::ServerThreadExecutor::getDefault()`（`ll/api/thread/ServerThreadExecutor.h`）。
- 每个 tick 处理预算：`blocksPerTick`（默认 20000）或 `tickBudgetMs`（默认 15ms），任一到达即 `co_await ll::coro::yield;` 让出到下一轮。
- 纯计算部分：`co_await task.via(ll::thread::ThreadPoolExecutor::getDefault())`，完成后自动回到服务端线程。
- 每玩家同一时刻 1 个活动任务，全局最多 `maxConcurrentJobs` 个；多任务轮转公平分配预算。
- `/cx cancel` 取消：已写入的部分记入 ChangeSet，可撤销。
- 进度：每 20 tick 用动作栏（actionbar）推送 `已完成 45% (1.2M/2.7M) 预计 8s`。

示意：

```cpp
ll::coro::keepThis([job = std::move(job)]() -> ll::coro::CoroTask<> {
    for (auto& slice : job->slices()) {            // 一个 slice = 一个区块内的变更
        if (job->cancelled()) break;
        co_await job->ensureChunkLoaded(slice.chunk);
        job->apply(slice);                         // 内部记 ChangeSet + setBlock
        if (job->budgetExceeded()) co_await ll::coro::yield;
    }
    job->finish();                                 // 邻居更新、方块实体刷新、通知
}).launch(ll::thread::ServerThreadExecutor::getDefault());
```

### 6.2 区块加载（ChunkGuard）

- 写入前检查：`BlockSource::areChunksFullyLoaded(BlockPos const& min, BlockPos const& max)`（MCAPI）或 `hasChunksAt(...)`。
- 未加载区块：
  - `TickingAreasManager` 全部是 MCNAPI，不能直接调。
  - 方案：用 `CommandRegistrar::executeCommand("tickingarea add ...", origin)` 临时加常加载区，处理完 `tickingarea remove`。注意每个世界常加载区数量上限，ChunkGuard 负责排队复用。
  - 备选：`ChunkSource::getOrLoadChunk(ChunkPos const&, LoadMode, bool readOnly)`（virtual）——需 Spike 验证写入后是否会保存。
- 配置项 `unloadedChunkPolicy`: `skip` | `tickingArea` | `deny`，默认 `tickingArea`。

### 6.3 写方块的路径

| 路径 | API | 用途 | 状态 |
| --- | --- | --- | --- |
| 标准 | `BlockSource::setBlock(BlockPos const&, Block const&, int updateFlags, ActorBlockSyncMessage const*, BlockChangeContext const&)`（virtual） | 默认路径 | 需验证 flags 含义 |
| 带方块实体 | `BlockSource::setBlock(..., std::shared_ptr<BlockActor>, ActorBlockSyncMessage const*, BlockChangeContext const&)`（MCAPI） | 粘贴箱子/告示牌 | 可用 |
| 含水层 | `BlockSource::setExtraBlock(BlockPos const&, Block const&, int updateFlags)` | 含水方块 | 可用 |
| 快速 | `LevelChunk::setBlockSimple(ChunkBlockPos const&, Block const&)`（MCAPI） | 超大填充 | 需验证客户端同步、存盘、光照 |

- `updateFlags` 没有枚举定义（头文件中无 `BlockUpdateFlag`）。按基岩惯例 1=通知邻居、2=同步客户端、4=无渲染、8=优先，**未经确认**，由 Spike S1 实测后写死成常量。
- `BlockChangeContext` 只有字段无构造函数，`BlockChangeContext{}` 能否直接用需验证（S1）。
- "物理"开关：配置 `physics=false` 时不通知邻居（防止沙子掉落、水流动、红石触发），操作结束后对边界一圈调用 `BlockSource::updateNeighborsAt` 做一次收尾。
- 快速路径改完区块后，客户端同步方案：`UpdateSubChunkBlocksPacket`（id 172）批量下发，或对附近玩家重发区块（`LevelChunkPacket` 构造是 MCNAPI，只能 `MinecraftPackets::createPacket` 后填字段）。

---

## 7. 交互层

### 7.1 命令

#### 命名方案

基岩版命令名不能以 `/` 开头，所以分三层：

1. **主命令** `/cx <子命令> ...`：始终可用，用 `CommandRegistrar::getServerInstance().getOrCreateCommand("cx", ...)` + 多个 `overload<Params>().text("set").required("pattern")…` 注册，补全友好。
2. **短命令**：`/pos1`、`/pos2`、`/wand`、`/undo`、`/redo`、`/copy`、`/paste`… 单独注册（`CommandHandle::alias` 可加别名）。与原版冲突的（`/fill`、`/clone`）不注册短命令。配置 `shortCommands` 可整体关闭。

#### 参数类型

- 坐标：`CommandPosition`（`getBlockPos(version, origin, offset)` 支持 `~ ~ ~` 与 `^ ^ ^`）。
- 方块：简单场景用 `CommandBlockName`；**图案和掩码统一用 `CommandRawText`/`std::string` 交给自研解析器**（原版参数无法表达 `50%stone,dirt`）。
- 方向：自定义 `enum class Dir { me, up, down, north, south, east, west, ... }`，枚举自动注册。
- 原理图名：`ll::command::SoftEnum`，`setSoftEnumValues` 在保存/删除后刷新，实现命令补全。
- 标志位（`-a -s -e`）：以 `std::string flags` 可选参数接收，自行解析。

#### 执行入口

所有命令 lambda 只做三件事：取 `PlayerSession`、解析参数、构造操作交给 EditSession。禁止在 lambda 里直接循环写方块。

#### 权限

- 默认 `CommandPermissionLevel::GameDirectors`（OP）。
- 细粒度：配置文件里每个命令组（`selection`、`region`、`generation`、`clipboard`、`history`、`schematic`、`navigation`、`utility`、`brush`）配置允许的玩家/权限等级，PlayerSession 带 `EditLimit`（最大体积、最大笔刷半径、每次最大改动数）。
- 预留与权限插件对接的接口（`api/PermissionProvider`）。

### 7.2 物品工具

- **魔杖**（默认 `minecraft:wooden_axe`，可配置）：
  - 左键方块 → 点 1：`ll::event::PlayerDestroyBlockEvent`（可取消，`pos()`），取消破坏。
  - 右键方块 → 点 2：`ll::event::PlayerInteractBlockEvent`（可取消，`blockPos()`、`item()`、`face()`）。
  - 判断手持物：`Player::getSelectedItem().getTypeName()`；魔杖带自定义名/lore（`ItemStackBase::setCustomName`/`setCustomLore`），用 `mUserData` 里的标记区分"真魔杖"和普通斧头（配置 `wandRequireTag`）。
  - 创造模式左键是瞬间破坏，事件仍会触发；生存模式下按住破坏也只在开始时处理一次（去抖 200ms）。
- **远程魔杖**（指南针）：`Actor::traceRay(float tMax, bool includeActor, bool includeBlock, ...)`（LLNDAPI）取远处方块；左键空气无事件，用 `PlayerSwingEvent`（无坐标）+ `traceRay` 补位，右键用 `PlayerUseItemEvent`。
- **笔刷**：`/cx brush sphere stone 4` 把笔刷绑定到当前手持物品类型；右键空气/方块时 `traceRay(range)` 找落点，执行一次 EditSession（可撤销）。
- **信息工具**：右键显示方块名、状态、runtimeId、方块实体 NBT（`Tag::toSnbt`）。

### 7.3 表单 GUI（便捷化重点）

使用 `ll::form::SimpleForm` / `CustomForm` / `ModalForm`（`ll/api/form/`），新版数据驱动 UI `ll::ui::CustomForm` 可作为后续升级。

- `/cx menu`（或蹲下 + 右键魔杖）打开主菜单：选区 / 填充 / 替换 / 形状 / 剪贴板 / 历史 / 原理图 / 笔刷 / 设置。
- 每个操作页都是 `CustomForm`：
  - 方块输入框默认填手持方块；下拉框列出最近使用的 10 个图案（存 PlayerSession）。
  - 形状页：类型下拉、半径滑条（`appendSlider`，上限 = 玩家 `EditLimit`）、空心开关、中心点（脚下 / 视线 / 点 1）。
- 危险操作（预计改动 > `confirmThreshold`，默认 100 万）弹 `ModalForm` 二次确认。
- 原理图浏览器：分页 `SimpleForm`，按钮显示名称、尺寸、作者、日期；点开后 加载 / 预览 / 删除。

### 7.4 反馈与可视化（服务端）

| 方式 | API | 说明 |
| --- | --- | --- |
| 线框 | `PrimitiveShapesPacket`（id 328，`mc/network/packet/PrimitiveShapesPacket.h`），`PrimitiveShapeDataPayload` 支持 `Box`/`Line`/`Text`、颜色、持续时间、维度、最大渲染距离 | 首选，原版客户端是否渲染需 Spike S2 |
| 粒子 | `SpawnParticleEffectPacket`（id 118），沿选区棱每隔 n 格发一颗 | 兜底，性能可控 |
| 假方块预览 | `UpdateBlockPacket`（id 21）/ `UpdateSubChunkBlocksPacket`（id 172），只发给本人 | 粘贴预览、形状预览，确认后真正执行，取消则把真实方块发回去 |
| 文本 | `Player::sendMessage`、动作栏进度 | 所有文本走 `tr()` |

发包：`MinecraftPackets::createPacket(MinecraftPacketIds::...)` 后填字段，`Packet::sendTo(Player const&)`。

可视化刷新策略：选区变化时立即重发；线框持续时间设 2s，每 1.5s 续发一次；玩家距离 > `visualMaxDistance` 时停止。

---

## 8. 客户端增强（`src-client/`，`LL_PLAT_C`）

### 8.1 选区线框渲染

- LL 没有世界渲染事件，需要自己 Hook。推荐点：`LevelRendererPlayer::$renderBlockEntities(BaseActorRenderContext&, bool renderAlphaLayer)`（MCAPI，可 Hook），只在 `renderAlphaLayer == false` 时画一次。
- 绘制：`ctx.mScreenContext.tessellator`：
  - `Tessellator::begin({}, mce::PrimitiveMode::LineList, 24, false)` → `color(r,g,b,a)` → 12 条棱 `vertex(x,y,z)`（坐标减去 `ctx.mImpl->mCameraPosition`）。
  - `MeshHelpers::renderMeshImmediately(screenCtx, tess, material, {})`，材质先试 `LevelRendererPlayer` 的 `mOutlineSelectionMaterial`。
- 待验证（S7）：该材质是否支持 LineList、深度测试状态。失败则改用细长四边形（TriangleList）画边。
- 内容：点 1（红）/点 2（蓝）小方块、选区包围框、多边形顶点连线、剪贴板粘贴落点虚影框、笔刷落点球形网格。

### 8.2 快捷键

客户端通过 `ll::input::KeyRegistry` 注册 Windows 虚拟键码。玩家可在「设置 → 键盘和鼠标」中改键。

| 名称 | 默认键 | 动作 |
| --- | --- | --- |
| `hpos1` / `hpos2` | F6 / F7 | 以准星方块设点 |
| `undo` / `redo` | F8 / F9 | 撤销 / 重做 |
| `menu` | F10 | 打开主菜单 |
| `paste_preview`（规划） | 待定 | 切换粘贴预览 |
| `nudge_*`（规划） | 方向键 | 微调选区/预览位置 |

这五项操作通过游戏的命令请求发送对应的 `/cx` 子命令，由服务端校验权限并执行。准星设点沿用 `/cx hpos1` 和 `/cx hpos2` 的服务端射线检测。

### 8.3 HUD

- 简单版：`GuiData::setActionBarMessage`（MCAPI）。
- 进阶版：`ll::event::AfterUIRenderEvent` 拿到 `MinecraftUIRenderContext`，`drawText` 画选区尺寸、体积、当前图案、任务进度。需要解决 `Font&` 获取（`ClientInstance::getFontHandle()`）——列为 P3。

### 8.4 双端通信

- 用 LL 自定义包（`ll/api/network/packet/`：`ll::network::Packet`、`PacketRegistrar`、`PacketSide`）。握手阶段 LL 会比对两端包集合，只有双方都装了 CreatorX 才启用增强模式，否则服务端自动退回粒子/PrimitiveShapes。
- 包定义（`src/net/`，双端共享）：

| 包 | 方向 | 内容 |
| --- | --- | --- |
| `CxHello` | C→S | 客户端版本、能力位 |
| `CxSelectionSync` | S→C | 维度、选区类型、顶点列表 |
| `CxClipboardSync` | S→C | 剪贴板尺寸、原点（不发方块数据） |
| `CxPreviewSync` | S→C | 预览框位置、旋转 |
| `CxJobProgress` | S→C | 任务 id、百分比、剩余时间 |
| `CxAction` | C→S | 预览位置微调等后续客户端动作，服务端校验权限 |

客户端发来的坐标只当"建议"，服务端校验距离（≤ `maxRemoteSelectDistance`）与权限。

---

## 9. 原理图（Schematic）

| 格式 | 读 | 写 | 实现 |
| --- | --- | --- | --- |
| `.mcstructure`（基岩原生） | 是 | 是 | `StructureTemplate::create(name, CompoundTag const&)` / `save()`；文件用 `CompoundTag::fromBinaryNbt`/`toBinaryNbt`（小端） |
| `.cxs`（自有） | 是 | 是 | gzip(NBT)：版本、尺寸、调色板（serializationId NBT）、索引（varint）、方块实体、元数据（作者、时间、描述、标签） |
| `.schem`（Sponge v2/v3，Java） | 是 | P3 | 需要 Java→基岩方块映射表 `java_bedrock_mapping.json`（数据来源：GeyserMC mappings，注意许可证） |

- 存储：`getDataDir()/schematics/`，可配置每玩家子目录、公共目录。
- 文件读写和压缩在线程池里跑。
- `/cx schem list` 支持分页、搜索；名称写入 SoftEnum 供补全。
- 安全：文件名只允许 `[A-Za-z0-9_\-一-龥]`，禁止路径穿越；加载前校验体积上限。

---

## 10. 配置（`config.jsonc`）

在 `src/Config/Config.h` 增加嵌套结构（嵌套结构体要特化 `is_reflectable<T>`），同步改 `config.jsonc` 注释：

```jsonc
{
  "version": 2,
  "logLevel": "Info",
  "language": "zh_CN",
  "command": {
    "shortCommands": true           // 注册 /pos1 /undo 等短命令
    "defaultPermission": "GameDirectors"
  },
  "wand": {
    "item": "minecraft:wooden_axe",
    "navItem": "minecraft:compass",
    "requireTag": false              // 只有 /cx wand 发的魔杖才生效
  },
  "limits": {
    "maxSelectionVolume": 8000000,
    "maxChangesPerOperation": 4000000,
    "maxBrushRadius": 12,
    "maxPolygonPoints": 64,
    "confirmThreshold": 1000000,
    "disallowedBlocks": ["minecraft:command_block", "minecraft:structure_block", "minecraft:barrier"]
  },
  "performance": {
    "blocksPerTick": 20000,
    "tickBudgetMs": 15,
    "maxConcurrentJobs": 4,
    "physics": false,
    "unloadedChunkPolicy": "tickingArea"
  },
  "history": {
    "size": 30,
    "maxBlocksInMemory": 5000000,
    "keepMinutesAfterQuit": 30
  },
  "visual": {
    "mode": "auto",                  // auto | shapes | particles | none
    "maxDistance": 128,
    "particle": "minecraft:villager_happy"
  },
  "schematic": {
    "dir": "schematics",
    "perPlayerDir": false,
    "maxFileSizeMB": 64
  },
  "groups": {
    // 权限组 → 限额覆盖
  }
}
```

---

## 11. 数据持久化

| 数据 | 位置 | 方式 |
| --- | --- | --- |
| 玩家偏好（魔杖、笔刷绑定、最近图案、gmask） | `getDataDir()/players.db` | `ll::data::KeyValueDB`，键 `uuid` → JSON |
| 原理图 | `getDataDir()/schematics/` | 文件 |
| 历史溢写 | `getDataDir()/history/<uuid>/` | 压缩 NBT，启动时清理过期 |
| 剪贴板 | 内存；可选 `/cx clipboard save` 落盘 | `.cxs` |

---

## 12. 对外 API（与领地/保护插件协作）

在 `src/api/` 定义 LL 事件（必须 `final`），通过 `ll::event::EventBus::getInstance().publish()` 发布：

- `CreatorXEditStartEvent`（可取消）：玩家、维度、包围盒、操作类型、预计改动数。保护插件可整体否决。
- `CreatorXBlockFilterEvent`：一次性提交一个 `std::function<bool(BlockPos)>` 过滤器（避免逐方块发事件的开销），保护插件返回"禁止写入"的判定函数。
- `CreatorXEditFinishedEvent`：统计信息，方便日志插件记录。

另外提供头文件 + 导出函数，让其他原生模组直接调用 `creatorx::api::getSelection(player)`、`setBlocks(...)`（P3）。

---

## 13. 多语言

- 所有玩家可见文本走 `tr("key", args...)`，同时写 `lang/zh_CN.json`、`lang/en_US.json`。
- 键名：`cmd.set.done`、`err.no_selection`、`err.pattern.unknown_block`、`job.progress` 等，模块前缀与目录对应。
- 理想状态是按玩家客户端语言选择（`CommandOrigin::getLocaleCode()` 为 LLNDAPI 可用），模板 I18n 需扩展为按 locale 查找。

---

## 14. 测试策略

1. **核心层单测**（GTest，独立 target，不链接 BDS）：Region 迭代与体积、扩展/收缩、Pattern/Mask 解析（含错误位置）、Transform 坐标与状态旋转表、BlockArray 序列化往返、ChangeSet 合并。
2. **游戏内测试清单**（每个里程碑手测，写在 `docs/TESTPLAN.md`）：
   - 100×100×100 填充：TPS 不低于 18，耗时、撤销后恢复一致。
   - 箱子/告示牌/旗帜复制粘贴后内容一致。
   - 含水方块、双层门、床、楼梯旋转粘贴正确。
   - 跨未加载区块填充。
   - 并发 4 人同时大操作。
   - 服务器关闭时进行中的任务：安全中止、历史可恢复。
3. **双目标编译**：CI 中分别 `--target_type=server` 和 `--target_type=client` 构建（`.github/workflows/build.yml` 已有骨架）。

---

## 15. 技术验证（Spike）清单——开发前先做

| ID | 问题 | 验证方法 | 不通过时的退路 |
| --- | --- | --- | --- |
| S1 | `setBlock` 的 `updateFlags` 各位含义；`BlockChangeContext{}` 是否可用 | 在测试世界分别用 0/1/2/3/… 写方块，观察邻居更新、客户端可见性 | 只用经过验证的组合 |
| S2 | 原版客户端是否渲染 `PrimitiveShapesPacket` 的 Box/Line | 服务端发包给原版客户端 | 粒子线框 |
| S4 | 方块状态旋转是否有内建路径 | 查 `BlockType`/结构放置相关 MCAPI | 自建 `state_rotation.json` |
| S5 | 未加载区块写入：`tickingarea` 命令 / `getOrLoadChunk` | 远处填充后重进世界检查是否保存 | 拒绝并提示玩家靠近 |
| S6 | `LevelChunk::setBlockSimple` 快速路径的同步/存盘/光照 | 写入后重连、重启服务器检查 | 只用标准路径 |
| S7 | 客户端线框：Hook 点、材质、LineList | 画一个固定盒子 | TriangleList 细条；或退回服务端可视化 |
| S8 | `KeyRegistry` 键码约定 | 注册几个键看设置界面显示 | `KeyInputEvent` 原始键码 |
| S9 | 单人模式下表单回调（内置服务端 + 本地玩家） | 单人世界打开 `/cx menu` | 单人模式只用命令 |
| S10 | `ItemStackBase::setCustomName` 的 `RedactableString` 构造 | 发魔杖 | 不设名字，只用 `mUserData` 标记 |


S1 当前实现（v0.1，待实测）：`BlockChangeContext{}` 可直接构造（`TypedStorage` 默认构造各成员，variant 为 `monostate`）；`updateFlags` 取 `physics ? 3 : 2`，即 1 = 通知邻居、2 = 同步客户端，集中在 `src/edit/ChangeSet.cpp` 的 `updateFlags()`。实测不符时只改这一处。

S5 当前实现（v0.1）：按 `deny` 处理。提交前用 `BlockSource::areChunksFullyLoaded(min, max)` 检查目标区域；执行中每进入一个新区块用 `hasChunksAt(pos, 0, false)` 复查，未加载即中止，已写入部分可撤销。

---

## 16. 里程碑

| 版本 | 内容 | 验收标准 |
| --- | --- | --- |
| **M0 Spike** | S1、S2、S4–S10 | 每项有结论写回本文档 |
| **v0.1 MVP** | 模板重命名；魔杖/pos1/pos2；立方体选区；`set`/`replace`/`walls`/`outline`；undo/redo；copy/cut/paste（无旋转）；分 tick 执行与取消；限额；粒子可视化；中英文 | 100³ 填充不掉 TPS 到 15 以下，撤销完全复原 |
| **v0.2** | 完整 Pattern/Mask；形状生成（球、柱、金字塔）；rotate/flip（含状态旋转）；stack/move；`.mcstructure`/`.cxs` 原理图；表单主菜单；gmask | FEATURES.md 中 P0+P1 的选区/区域/剪贴板项全部完成 |
| **v0.3** | 笔刷与工具；导航命令；实用命令；多边形/椭球/圆柱选区；PrimitiveShapes 可视化；粘贴/形状假方块预览 | 手机端可只用表单完成"选区-填充-撤销-粘贴" |
| **v0.4** | 客户端增强：线框、快捷键、HUD、双端包同步 | 场景 B、C 可用 |
| **v1.0** | Sponge `.schem` 导入；对外 API 事件；性能调优；完整文档与测试计划 | 发布到 lip |
| **P3 以后** | 表达式生成、像素画、文字、树木/地形笔刷、平滑/侵蚀、凸包选区 | 按反馈排期 |

---

## 17. 编码约定（补充模板 SKILL.md）

- 命名空间：`creatorx`（模板初始化时由 `my_mod` 改名），核心层 `creatorx::core`，客户端 `creatorx::client`。
- 写任何 `ll::`/MC 调用前先查头文件；`MCNAPI` 函数不可用；`virtual` 函数可调用但 `$` thunk 若为 MCNAPI 则不能 Hook。
- 平台代码用 `#ifdef LL_PLAT_C` / `LL_PLAT_S` 或放进 `src-client/`。
- 世界对象只在服务端线程访问；跨线程只传值（`BlockId`、`Vec3i`、`CompoundTag` 拷贝）。
- 事件监听器在 `enable()` 注册、`disable()` 移除（保存 `ListenerPtr`）；命令在 `enable()` 注册；Hook 用 `HookRegistrar` 成员管理生命周期。
- 任务执行中模组被禁用：`disable()` 先取消全部任务并等待当前 slice 结束。

---

## 附录 A：关键 API 速查

| 用途 | API | 头文件 |
| --- | --- | --- |
| 注册命令 | `CommandRegistrar::getServerInstance().getOrCreateCommand(name, desc, perm, flag)` | `ll/api/command/CommandRegistrar.h` |
| 命令重载 | `CommandHandle::overload<P>().text().required().optional().execute(fn)` | `ll/api/command/Overload.h` |
| 动态补全 | `CommandRegistrar::setSoftEnumValues(name, values)` | 同上 |
| 执行原版命令 | `CommandRegistrar::executeCommand(str, origin, ...)` | 同上 |
| 右键方块 | `ll::event::PlayerInteractBlockEvent` | `ll/api/event/player/PlayerInteractBlockEvent.h` |
| 左键破坏 | `ll::event::PlayerDestroyBlockEvent` | `ll/api/event/player/PlayerDestroyBlockEvent.h` |
| 使用物品 | `ll::event::PlayerUseItemEvent` | `ll/api/event/player/PlayerUseItemEvent.h` |
| 挥手 | `ll::event::PlayerSwingEvent` | `ll/api/event/player/PlayerSwingEvent.h` |
| 进出服 | `PlayerJoinEvent` / `PlayerDisconnectEvent` | `ll/api/event/player/` |
| 服务端线程 | `ll::thread::ServerThreadExecutor::getDefault()` | `ll/api/thread/ServerThreadExecutor.h` |
| 线程池 | `ll::thread::ThreadPoolExecutor::getDefault()` | `ll/api/thread/ThreadPoolExecutor.h` |
| 协程 | `ll::coro::CoroTask`、`keepThis`、`yield`、`co_await 1_tick` | `ll/api/coro/` |
| 表单 | `ll::form::SimpleForm` / `CustomForm` / `ModalForm` | `ll/api/form/` |
| KV 存储 | `ll::data::KeyValueDB` | `ll/api/data/KeyValueDB.h` |
| 文件 | `ll::file_utils::readFile` / `writeFile` | `ll/api/io/FileUtils.h` |
| 读写方块 | `BlockSource::getBlock` / `setBlock` / `getExtraBlock` / `setExtraBlock` | `mc/world/level/BlockSource.h` |
| 取 BlockSource | `Actor::getDimensionBlockSource()`、`Dimension::getBlockSourceFromMainChunkSource()` | `mc/world/actor/Actor.h` |
| 方块查找 | `Block::tryGetFromRegistry(...)`（6 个重载） | `mc/world/level/block/Block.h` |
| 方块实体 | `BlockActor::save` / `create` / `refresh` | `mc/world/level/block/actor/BlockActor.h` |
| NBT | `CompoundTag::fromSnbt` / `toBinaryNbt` / `fromBinaryNbt`、`Tag::toSnbt` | `mc/deps/nbt/CompoundTag.h` |
| 结构 | `StructureTemplate::create(...)` / `placeInWorld(...)` | `mc/world/level/levelgen/structure/StructureTemplate.h` |
| 区块 | `ChunkSource::getAvailableChunk`、`LevelChunk::setBlockSimple` | `mc/world/level/chunk/` |
| 射线 | `Actor::traceRay(tMax, includeActor, includeBlock, filter)` | `mc/world/actor/Actor.h` |
| 包围盒迭代 | `BoundingBox::forEachPos()` | `mc/world/level/levelgen/structure/BoundingBox.h` |
| 发包 | `MinecraftPackets::createPacket(id)`、`Packet::sendTo(Player const&)` | `mc/network/` |
| 线框包 | `PrimitiveShapesPacket` / `PrimitiveShapeDataPayload` | `mc/network/packet/` |
| 客户端实例 | `ll::service::getClientInstance()` | `src-client/ll/api/service/TargetedBedrock.h` |
| 快捷键 | `ll::input::KeyRegistry` / `KeyHandle` | `src-client/ll/api/input/` |
| 键鼠事件 | `KeyInputEvent` / `MouseInputEvent` | `src-client/ll/api/event/input/` |
| UI 渲染事件 | `BeforeUIRenderEvent` / `AfterUIRenderEvent` | `src-client/ll/api/event/render/UIRenderEvent.h` |
| 世界渲染 Hook | `LevelRendererPlayer::$renderBlockEntities` | `src-client/mc/client/renderer/game/LevelRendererPlayer.h` |
| 画线 | `Tessellator::begin/color/vertex`、`MeshHelpers::renderMeshImmediately` | `src-client/mc/client/renderer/` |

## 附录 B：已确认不可用 / 不存在

- `BlockSource::fillBlocks`、`BlockSource::setBiome`、`isEmptyBlock`：不存在（改用 `Block::isAir()`；生物群系用 `LevelChunk::_setBiome`，同步待验证）。
- `TickingAreasManager`：全部 MCNAPI。
- `SubChunk` 所有方法：MCNAPI。
- `StructureManager::importStructure`、`tryPlaceStructureInWorld`：MCNAPI。
- `CommandBlockName::resolveBlock(states, output)`：MCNAPI（只有 `resolveBlock(int data)` 可用）。
- `ServerScriptDebugDrawerPacket`：本版本没有，替代品是 `PrimitiveShapesPacket`。
- 客户端 `ClientNetworkHandler::handle(PrimitiveShapesPacket)`：MCNAPI，不能 Hook。
- LL 没有世界 3D 渲染事件，没有 `ll/api/schedule/`。
- 没有"左键点击方块开始破坏"的专用事件。
