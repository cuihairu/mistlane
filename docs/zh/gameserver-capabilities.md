# gameServer 能力评估

本页逐项盘点旧游戏 Hegel 进程（gameServer 对应物）：进程形态、场景模板与实例、计时、同步与释放。逐条区分通用机制与原游戏玩法/配置，并说明各自对 Mistlane 的含义。证据引自 game-fogs 源码，不迁入任何代码或资源。本页与[模型页](./player-scene-model.md)（Session/Avatar、移交、地图数据分层）和[迁移账本](./migration-status.md)（计时器与工具文件已有结论）互补，不重复其结论。

## 进程形态

| 观察 | 旧实现证据 | 通用或玩法 | Mistlane 含义 |
|---|---|---|---|
| C++ 外壳是宿主，不是领域 | `d_hegel/ServerMain.cpp`：配置文件加 `--index` 进程序号、日志配置、RAII MySQL 初始化、单 `io_service` 上的 `FogsProcess`、信号驱动的优雅停止 | 通用 | Mistlane 原则上保持同一拆分：单进程、C++ 承载、Lua 领域 |
| 仅 gateway 接入 | `d_hegel/HegelServer.cpp::handleAccept/onRegister`：接入的连接经 `LuaCallHandler::registerConn` 注册，按 gateway id 存入 `gates_`，后续每帧经 `pushRpcMsg` 推入单个 Lua VM | 通用 | 当前 Mistlane gameServer 直接接受客户端并以 MLR1 上行；仅网关接入是部署形态，不是能力缺口 |
| 单个 C++ tick 驱动全部 Lua 时间 | `HegelServer::startTick/doTick` 调 `LuaCallHandler::onTick`，后者以墙钟毫秒调用 `L_doTick` 全局函数；间隔由 Lua `C_startTimer` 指定（`game/Util/Timer.lua` 默认 1000 ms） | 通用 | Mistlane 运行固定 100 ms、无参数的 `tick()` 钩子（见[账本](../migration-status.md#legacy-lua-callback-audit)）；两者都是单驱动设计 |
| 约 20 个 C API 绑定 | `d_hegel/CAPI.cpp`：listen、timer、死循环监控、connect server、UDP client、地图加载/信息/卸载、寻路与坐标换算（`generatePath`/`isStraightReachable` 已注册，`generateHighWay` 被注释）、MD5、时钟、运营日志 | 混合 | Mistlane 以模块表（`mistlane.*`）取代全局函数；地图/导航绑定不存在，也不从旧签名推导 |

Lua 侧承载全部领域：`game/Hegel/` 下 60+ 个玩法模块目录（战斗、帮会、宠物、任务、商店、竞技场等）加上框架目录（`Scene`、`GameScene`、`Server`、`Rpc`、`Async`、`Util`）。玩法目录整体排除在迁移之外。

## 场景模板与实例

| 条目 | 旧实现行为与证据 | 通用或玩法 | Mistlane 边界 |
|---|---|---|---|
| 模板到实例 | `SceneLogic.newScene` 对模板行浅拷贝并拆分身份：`o.tid = meta.id`、`o.id = tonumber(instId)`；`Scene:new` 在 tid 等于 id 时标记 `keep = true` | 通用机制，取值为玩法 | 目标模型已声明定义与实例分离；拷贝并独立编号是可迁移的部分 |
| 常驻场景 | `SceneManager.init` 对 `isInitScene`（`type == SCENE_TYPE_INIT`）的模板行各建一个实例并标记 `dontClear`；`Scene:clear` 在该标记下拒绝执行并打印调用栈 | 通用机制，取值为玩法 | 常驻与临时场景采用不同释放策略——与目标契约一致 |
| 临时副本 | `SceneLogic.createCloneScene` 从进程内计数器（起始 1,000,000）分配动态实例；源码留有 `TODO 确保每个hegel生成的mapid唯一`——跨进程唯一性未处理 | 通用机制，未完成 | 实例 id 需在分配作用域内唯一；旧方案只证明单进程 |
| 实例查找与按需创建 | `SceneManager.guaranteedGet(sceneId, tid)` 在实例 id 与模板 id 都不存在时退化为克隆 | 通用 | 进入既有场景不得克隆或重载；按需创建需要真正的存在性判断，回退路径把两者混在一起 |
| 容量与空闲 | `Scene:init` 从模板行或 `host_config.hegel.hegel_Limit` 取 `maxNum`；`Scene:isIdle` 按 `type > 2` 或 `#aids < maxNum` 判定 | 机制，阈值为玩法 | 当前分配查询（`MLR1 PLACE`）以整进程为粒度；每场景容量仍是 gameServer 本地决策 |
| 排除项 | `SceneType` 取值、`minLv` 门槛、`gametp`/活动/Boss 接线、`SCENEBATTLE` 概率表、`SceneMob` 波次/复活/驻留怪、组队与阵营语义、`changeToReborn` 中的成就/埋点副作用 | 玩法 | 不迁移，也不换个名字重实现 |

## 计时

旧 gameServer 只有一个进程级驱动：C++ tick 调 `L_doTick(ms)`，全部 Lua 调度在 `game/Util/Timer.lua` 的 `TimerQueue` 上——两层结构（min-heap 加 5 秒内桶数组、10 万任务上限、惰性删除、`xx1` 哨兵值移除出错的重复任务）已在[迁移账本](../migration-status.md)比对过。账本未记录的部分：

- **场景级 tick 并未运行。** `SceneManager.startTimer()` 内只剩被注释的 `--Timer.setInterval(SceneManager.updateState, 100)`；`SceneManager.updateState` 没有其他调用者。代码形态暗示的每场景 100 ms 节奏从未生效。
- **每场景唯一的定时器是位置同步。** `Scene:init` 以 `UPDATE_INTERVAL = 1000` 启动 `Timer.setInterval(self.syncChar, UPDATE_INTERVAL, self)`，且仅在模板设置 `isSync` 时；`Scene:clear` 会连同场景自身的定时器表一并移除。
- **错误隔离按任务进行**，`xpcall` 加逐函数 `Statistics` 计时；重复任务从当前 tick 重排（`t.ts = ms`），节奏重新起算而非累积漂移。

对 Mistlane 而言，场景级计时（每场景节奏、绑定场景生命的定时器、释放时失效）是设计项而非抽取项：旧证据给出了形态，也表明该通用形态从未被端到端使用。

## 同步

| 条目 | 旧实现行为与证据 | 通用或玩法 | Mistlane 边界 |
|---|---|---|---|
| 脏位置批处理 | `Scene:updatePos` 记录 `syncDirty[aid]`；`Scene:syncChar`（1000 ms）向每个客户端发送其他角色的脏数据并清空批次 | 通用形态 | 按场景分组的兴趣批处理是目标机制；当前运行时尚无任何实现 |
| 仅队长上报位置 | `updatePos` 忽略非队长成员的上报，并把队长位置复制给跟随者（组队变化时 `syncPosByLeaveTeam` 对称处理） | 通用闸门里的玩法规则 | 客户端上报不是服务端移动模拟；目标不得把队长权威当作物理模型继承 |
| 分块的入场载荷 | `Scene:enter` 用 `TableUtil.splitArr(chars, 20)` 切分可见角色列表，每块一帧 `SC_Sync`，随后是怪物/波次/个人怪载荷，最后 `SC_CharEnter` 广播 | 通用形态，载荷为玩法 | 有界的初始状态下发是目标机制；每帧 20 个是调优值，不是契约 |
| 模板门控广播 | `Scene` 内所有广播与同步都以 `TemplateReader.isNull(self.isSync)` 为闸——未设置标记的场景类型保持静默 | 玩法配置 | 按场景类型选择性同步是合理的；标记语义归属游戏侧 |
| 角色字段通知 | `SC_CharDataUpdate = 4005` 经 `Charactor.set`，已在[模型页](./player-scene-model.md)记录 | — | 属性同步由[实体契约](./entity-contract.md)约束，不在本页 |

## 释放

释放路径是旧证据最薄弱的部分：

- `Scene:clear` 释放成员、移除场景定时器并尊重 `dontClear`——这是唯一完整实现的部分。
- `Scene:destory` 是空的 `-- TODO`（注释称副本"人物都离开或时间到了自动销毁"），按空置或超时结束实例的能力并不存在。
- `SceneLogic.leaveScene(c, clearCurScene)` 在要求清理当前场景时调用 `SceneManager.delete(curScene)`——`curScene` 并非该函数的局部变量（实际解析的变量是 `ttid`），该分支什么也删不掉。承诺清理的调用路径没有清理。
- `GameSceneIndex.afterClose` 展示的是活动形态的模式：先发奖励，3 秒 `Timer.setTimeout` 后把剩余玩家全部传出、重置簿记；疏散是逐模板代码，不是框架服务。

Mistlane 当前没有可释放的场景对象（SIGTERM 排空的是会话，不是场景）。释放契约——空闲判定、玩家疏散、超时、静态数据引用计数——必须与场景模型一起设计（地图/场景生命周期任务项）；旧代码不能作为规范。

## 什么迁移、什么不迁移

**通用机制（候选，Mistlane 均未实现）：**单驱动 tick 加有界、错误隔离的任务调度器；模板拷贝建实例并独立编号、带常驻标记；每实例成员表与容量/空闲判定；固定节奏的脏数据批同步；有界分块的初始状态下发；释放时失效定时器并拒绝常驻实例。

**排除（原游戏玩法与配置）：**60+ 玩法模块树及其模板表、场景类型语义、等级与成就门槛、组队/阵营/复活/战斗状态规则、怪物波次与驻留怪、排行与奖励流程，以及上文引用的所有 XML/配置取值（它们是证据，不是默认值）。

**源码未能证明的部分：**任何通用机制作为成熟契约——场景 tick 被注释、析构是 TODO、清理分支是死代码、跨进程实例 id 是未决 TODO、活动导航在 `d_hegel/CAPI.cpp` 中被注释（见[模型页](./player-scene-model.md)）。

## 角色映射

- **gameServer** 以通用生命周期/空间/同步机制加 Lua 玩法规则承载场景实例——观察到的 C++/Lua 拆分延续；当前 Mistlane gameServer 只提供 Lua 执行外壳与准入，场景模型尚未开始。
- **crossServer** 复用同一套机制、只是模板集不同：旧代码复用同一 `Scene` 类，用 `ServerUtil.isCrossServer`/`P_SIDX` 检查和 `IS_CROSS` 表（`GameSceneIndex.setScene`）区分跨服行为，而不是独立运行时。
- **coordinator** 不从本页获得任何东西：场景的 tid/id 是进程内对象身份，不是归属记录；承载与迁移状态由[职责契约](./process-roles.md)约束，仍未实现。

旧代码位置：`d_hegel/ServerMain.cpp`、`d_hegel/HegelServer.cpp::startTick/doTick/onRegister`、`d_hegel/CAPI.cpp`（注册段）、`game/HegelMain.lua`、`game/Util/Timer.lua::doTick`、`game/Util/TimerQueue.lua`、`game/Hegel/Scene/SceneManager.lua::init/guaranteedGet/updateState/startTimer/delete`、`game/Hegel/Scene/SceneLogic.lua::newScene/createCloneScene/changeOne/leaveScene/doSwitchHegel`、`game/Hegel/Scene/Scene.lua::new/init/enter/leave/syncChar/updatePos/clear/destory`、`game/Hegel/GameScene/GameSceneIndex.lua::setScene/afterOpen/transferAllPlayer/afterClose`。
