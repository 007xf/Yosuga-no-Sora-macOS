# 缘之空 + 悠之空 macOS 原生移植 · 交接

更新：2026-10-26（Asia/Singapore）。本文件替代 1.1—1.3.0 的交接，历史以 Git 记录为准。

## 当前正式版

- 应用：`/Applications/缘之空.app`，版本 1.4.0 / 20261026.1（安装与核验结果见文末「本次验收」）。
- 源码：私有仓库 `007xf/Yosuga-no-Sora-macOS`（全新提交历史，只含源码与文档，不含游戏数据）。
- 引擎：EntisGLS4 SDK 的 SDL3 移植（`source/vendor/EntisGLS-SDL3`，本作分支 yosuga-port）。

## 1.4.0 做了什么

1. **「空澄」界面（明亮）**：三类覆盖包（`title.noa`、`frame.noa`、`option.noa`，两作各一套）。
   - 对白框与回看保留原作的深青→天蓝渐变，白字配深青阴影；其余系统页（存读档、设定、确认、选项、鉴赏、语音个别设定）为白色磨砂玻璃、深青墨字、天蓝强调色。
   - 所有界面纹理由 `mods/src/sky_ui.py` 以 4 倍原稿绘制，缩小为逻辑纹理；原稿同时就是该纹理的高清伴随。文字使用圆体-简（`tools/fonts.swift` 自动取得）与 SF Rounded，鉴赏曲名用ヒラギノ丸ゴ。
   - 标题新增「攻略进度」（`ID_GUIDE`）；悠之空的「结束」标为「返回缘之空」（合并版中它回到缘之空）。
   - 皮肤 XML 的颜色一律写十进制：SDK 的 rich-integer 解析会把含字母 E 的十六进制（如 `00C4EAF8H`）读成指数，得到黑色。
   - 原归档的非 XML 条目逐字节保留；XML 只允许声明过的编辑与纹理重定向（`make_frame_skin.build_archive` 断言）。
2. **高清文字**（引擎 `native/platform/sdl/hd_text.cpp`）：对白（`legacy_message.cpp` 逐字）与静态文字（SDK `SGLSpriteText`，经 `tools/sdk/prepare_sdl_hd.py` 补丁）以 4 倍字号用同一字体、描边、阴影重新排字，每个字放在其逻辑排版位置（含行对齐偏移）的 4 倍处，作为逻辑图的高清伴随发布。逻辑像素、字距、换行、点击区域不变。`ENTISGLS_HD_DEBUG=1` 时记录装饰参数。
3. **游戏内攻略进度**（引擎 `apps/macos/guide_overlay.mm`）：在游戏窗口的 4:3 画面上绘制的页面，取代原生窗口。分支流程（按剧情顺序排列，只连实际走过的分支，拖动平移、滚轮/双指缩放、定位最近）、路线总览（章节进度、已读至结局）、特别内容；详情面板可切换记录、「回到这个选择」「从章节开头进入」「进入特别内容」，游戏内确认框，确认前后重读记录（沿用 1.3.0 的数据与恢复接口，`game_progress.h`）。入口：标题按钮（`Bridge::FilterCommand` 发出 `GuideRequested`）、应用菜单「攻略进度…」、设置的数据页。页面打开时独占鼠标与键盘（A/H 与菜单游戏命令不转发）。Esc 或右键返回。
4. **体积**：高清素材改为无损 WebP 存储（`tools/hd/pack_store.py`），逐像素与审查通过的 PNG 相同，按画面去重；不再打包旧「晴空」皮肤的 90 张 PNG。引擎静态链接 libwebp 解码器（`image_codec.cpp`），解码结果与 PNG 路径相同。

## 数据边界

- 原游戏目录只读；个人存档 `~/Library/Application Support/Sphere/YosugaNoSora` 不参与测试或打包。
- 测试一律设置 `ENTISGLS_TEST_SAVEDIR`（隔离存档）与 `ENTISGLS_DRIVER`（驱动文件）。截图只用实例自身的 `shot`/`uishot`，不截整个桌面；不抢前台。
- GitHub 仓库只放源码、文档与 UI 截图；原作归档、解码或放大的素材、反编译脚本、对白、存档、高清集均不上传。
- 1.4.0 交付后按用户要求，本机只保留 `/Applications/缘之空.app`；项目目录、测试副本、高清生产集与原游戏目录已移入废纸篓（由用户自行清空）。高清素材此后的唯一副本在 app 的 `Contents/Resources/hd`（无损 WebP，可逐像素还原为 PNG）。

## 构建

从源码与自己的两作目录完整构建（首次高清处理需数小时）：

```sh
cd source
./build.sh "/path/to/缘之空" "/path/to/悠之空"
```

依赖：Xcode 命令行工具；`brew install cmake bison webp`；`python3 -m pip install pillow numpy fonttools`。产物 `build/stage/缘之空.app`。分步：

```sh
# 引擎
cd vendor/EntisGLS-SDL3
cmake -S . -B build/macos-sdl3-arm64 -DENTISGLS_LAUNCHER=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DTJS_BISON=$(brew --prefix bison)/bin/bison
cmake --build build/macos-sdl3-arm64 --target studysteady_sdl --parallel
cd ../..
# 皮肤（三类覆盖包 + 4 倍原稿与 validation.json）
python3 -I mods/src/build_mods.py "$YOSUGA_DIR" "$HARUKA_DIR" mods/build
# 高清存储（增量）与打包、核验
python3 -I tools/hd/pack_store.py build
YOSUGA_DIR=... HARUKA_DIR=... tools/package_app.sh
python3 -I tools/hd/verify_app.py build/stage/缘之空.app
```

`package_app.sh` 克隆（APFS）原作运行文件，拷入覆盖包，安装高清存储，写 Info.plist（版本在脚本内），临时签名，然后运行 `verify_app.py`：高清清单逐行与文件散列、全部解码像素、覆盖包与当前构建一致且每张贴图都有高清伴随、原作运行文件散列、无个人存档、版本、严格签名。

开发时快速预览皮肤：`python3 tools/hd/dev_root.py` 生成 `work/hd-dev`（PNG），用 `ENTISGLS_HD_ROOT=work/hd-dev` 启动；`tools/ui_mockup.py` 在背景上离线预览对白框。

## 测试驱动

`ENTISGLS_DRIVER=<文件>` 后，引擎逐行执行启动之后追加的命令（启动前已有的行忽略）：`wait N`、`shot <png>`（只含游戏 GL 画面）、`uishot <png>`（覆盖层）、`click/rclick/move X Y`（800×600 逻辑坐标，SDL 事件）、`key`、`wheel`、`hub <game> <ID>`、`shell <命令>`、`quit`。攻略页面是 AppKit 视图，SDL 点击到不了，测试用 `shell progress-click X Y`（仅隔离存档时可用）、`progress-select`、`progress-entry`、`progress-restore`、`progress-confirm`、`close-progress` 等。`ENTISGLS_TEST_GAME=haruka` 直接以悠之空启动。

## 已知边界

- 没有从头到尾游玩全部路线；声音只做过客观抽检。
- 缘之空鉴赏需通关才在标题出现，本次以标题命令打开验证界面，未用通关存档走完整流程。
- 墙纸鉴赏的软木板标题、悠之空倒计时等脚本图片属于美术内容，保持原样（高清插值）。
- 攻略页面在剧情中通过菜单打开时，自动播放不会暂停。
- 自动播放开启后，再点「自动」不会关闭它；点击画面任意处（含其他按钮）才停止，而这次点击只用于停止。属原版脚本逻辑，界面只负责显示开启状态。
- libwebp 静态库来自 Homebrew，标注的最低系统版本高于应用的 11.0（链接时有警告）；本机运行正常。

## 本次验收（2026-10-26，已安装的 1.4.0）

- 体积：app 9.9 GB（原 1.3.0 约 15 GB），其中高清素材 5.9 GB（原 PNG 11.0 GB），游戏数据 3.9 GB。
- `verify_app.py`（只读，含全部解码）：高清 4280 行 / 3762 文件 PASS；覆盖包 6 个、贴图 161 张 PASS；原作运行文件 14 个归档 + 6 个散文件散列 PASS；版本、严格签名、无个人存档 PASS。
- 运行（隔离存档，安装版）：两作标题、对白、选项、存读档、设定（四页）、确认框、回看、鉴赏（两作）、悠之空标题与返回、游戏内攻略进度（分支流程、路线总览、特别内容、「回到这个选择」实际恢复到选项）全部走过；高清目录命中 4280 帧，无拒绝；正常退出。
- 个人存档：测试前后 5 个文件散列一致。
- 截图：`docs/images/`（由 `shots/release` 缩为 1200×900 JPG）。
