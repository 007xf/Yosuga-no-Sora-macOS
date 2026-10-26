# 攻略进度计算

`reference.py` 只读解析实际 `system.dat`，不保存、不修改原存档：

```sh
python3 tools/progress/reference.py --game yosuga /absolute/path/system.dat
python3 tools/progress/reference.py --game haruka /absolute/path/system.dat --compare /absolute/path/progress.json
python3 tools/progress/test_progress.py
python3 tools/progress/build_manifest.py --check
```

原生实现位于 `native/runtime/cotopha_port/game_progress.cpp`。每次成功载入或保存原作的 `system.dat` 后，在其可写存档目录用临时文件、`fsync` 和 `rename` 原子更新 `progress.json`。界面使用此记录和线程安全的 `LatestProgressJson(game)`；它是最近载入或保存时的快照，不是逐句实时数据。快照失败只记录日志，不改变原游戏存档结果。

统计单位：

- 已读：从原版标题入口可到达的剧情函数中，不重复的正数 `Hitret` 标志。缘之空 39343，悠之空 16088，不把预留的 48000 位当分母。缘之空未被调用的旧脚本 `00_B18B` 和其中的重复 `Hitret(0)` 不计入。
- CG：原作鉴赏页的缩略图条目，缘之空 128，悠之空 87；每个条目都已确认有可到达剧情能设定其对应标志。立绘的表情差分和 CG 图片差分不会重复增加分母。
- 回想：原作可选场景的专用解锁标志，18 / 8。
- 音乐可鉴赏：原作没有逐曲播放或解锁标志。缘之空任一路线通关后开放全部 21 曲；悠之空默认开放全部 22 曲。因此这项不表示已经听过。
- 路线完成：各线最后一句的已读标志，人物对应同时核对末章、前后剧情链和 `StaffRoll` 类型：缘之空 A 穹 / B 奈绪 / C 瑛 / D 一叶 / E 元香；悠之空 A 穹 / G 八寻 / H 梢。

`build_manifest.py` 从反编译源和现有鉴赏表生成 `manifest.json` 与 `game_progress_data.h`，输出只含标识符、位号和来源散列，不含剧情文本。SDK 原件不变。

测试在独立临时目录编译纯 C++ 统计模型，与独立 Python 参考计算逐字段比对：空存档、所有位为 -1 的带符号字、稀疏末句标志、32 位边界、分母之外的占位标志、音乐开放规则和可达图。实际运行时还需用隔离测试目录进行 load/save，然后以 `--compare` 核对该目录的原生快照。

## 三页攻略界面与真实恢复

原生「攻略进度」分为分支流程、路线总览、特别内容三页，参考用户仓库
`007xf/WHITE-ALBUM2-macOS` 的 `f0748fc04eec871b691e77e808ad67ef26708382`。

- 分支节点与选项只在原脚本实际显示选择后记录；选过的答案和下一次实际到达的选择形成连线。正常读档、重新开始及跨作品切换会重置当前连线的起点，历史节点仍保留。
- 选择现场和章节起点使用原生 `ScenarioObject` 序列化；最多保留同一选择的五份不同现场。后台写入和校验，不在第一次显示选择时同步写磁盘或计算剧本散列。
- 返回使用原作 `ShowLoadEffect → EnterLoad → ScenarioLoop` 读档流程，保留场景变量和选择历史。章节入口只在确实留有该起点快照时启用，不通过裸跳场景拼造路线条件。普通槽位和快速存档不用于存放攻略快照。
- 路线只显示实际已读到的章节；「已读至结局」使用原作各线末句已读标志，不表示已完整播放片尾。缘之空的 G000–G003 经原始调用图验证属于元香线。
- 特别内容使用原作标题入口和解锁规则。缘之空鉴赏要求实际 `sysObj.gameClear`，悠之空的原版特别入口默认开放。音乐数量仍表示「可鉴赏」，不表示已经听过。

原 `progress.json` 继续是载入/保存 `system.dat` 时的兼容快照；运行中的面板另由安全的原脚本输入等待点更新内存统计。分支记录保存在各作品的
`savedata/01 攻略进度/01 分支记录.json`，现场在其 `02 选择快照/`。它们属于个人游玩数据，不能打包进应用。

恢复前检查作品、原剧本 SHA-256、检查点大小/散列、无外部引用的原生结构与安全路径；异常记录保留并显示错误，不覆盖普通存档。恢复只在新会话的干净脚本栈进行，到达真实输入等待点后才显示成功。

两作原生对象版本分别为缘之空 200、悠之空 100。悠之空 `BustupInfo` 中的 `spr` / `sprImage` 仅在快照独立副本中清空，由原作恢复流程重建；不允许加载任意原生 Sprite 或外部引用。旧存档继续提供已读与鉴赏统计，选择现场在升级后实际再次到达时记录。

目录生成增加 `chapters`、`scenario_catalog` 和 `specials`，只包含标识符、统计范围、入口规则与界面标签，不包含游戏对白或选项全文。独立存储回归可运行引擎 CMake 目标 `branch_guide_store_test`。

1.4 起攻略进度改为游戏内页面（引擎 `apps/macos/guide_overlay.mm`），数据与恢复接口不变；打包统一使用 `tools/package_app.sh`。最终运行、清理和重建步骤见项目根 `HANDOFF.md`。
