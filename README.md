# EAGLS Engine Tool

> **项目正在重启中。核心归档层（IDX/PAK 读取与原样提取）与 CLI 已实现，并在首组真实样本上完成全量提取与独立 SHA-256 交叉校验；DAT 解密/原编码正文导出、GR 到 BMP 解码、文本清单提取/回填（含 GBK 译文）与 EAGLS.exe 中文显示补丁已实现，1,739 个内容输出通过独立交叉验证，补丁已通过游戏内中文显示验证；GR 编码与 PAK 回包尚未实现。**
>
> 旧 C++ 移植代码、旧可执行程序及分散的工程文件已经移除。旧版使用说明不再适用，请勿把历史版本或构建成功视为资源处理正确的证明；格式结论以 [归档格式证据](docs/eagls-archive.md) 为准。

## 目标与现状

从真实 EAGLS 游戏资源及游戏行为出发，实现原生 C++ 工具。面向资源提取与汉化工作流，独立核心由 CLI 与未来 GUI 共用。

| 项目 | 当前状态 |
| --- | --- |
| C++17 核心静态库、统一 CMake、CTest | MinGW 与 MSVC（Release）均构建并通过测试 |
| CLI：帮助、版本、`pak info`/`list`/`extract` | 已实现 |
| IDX/PAK 检查、列目录、原样提取 | 已实现；首组样本 5,022 个条目全量提取，独立 SHA-256 交叉校验通过 |
| DAT 解密 / 原编码正文导出、GR 到 BMP | 已实现；279 个 DAT、1,181 张 GR 完成独立验证 |
| 文本清单提取 / 回填（cp932 原文、gbk 译文） | 已实现；a001.dat 中文版通过游戏内显示验证 |
| EAGLS.exe 中文显示补丁（`exe check` / `exe patch`） | 已实现；6 字节补丁，MinGW/MSVC 产物 |
| GR 编码、PAK 回包 | 未实现 |
| GUI | 暂定 FLTK，尚未接入 |
| 真实游戏兼容性 | 仅炎孕人生（SQUEEZ，2006）三组归档与 EAGLS.exe 中文补丁完成验证；其他版本未验证 |

## 目录

```text
eagls_packer/
├── CMakeLists.txt          # 唯一构建入口
├── cmake/                 # 编译选项与生成文件模板
├── include/eagls/         # 核心公开接口
├── src/                   # eagls_core 的内部实现
├── apps/
│   ├── cli/               # eagls 命令行入口
│   └── gui/               # FLTK 接入边界说明，暂无 GUI 代码
├── tests/                 # 核心与 CLI 测试
│   └── fixtures/          # 样本约定；真实样本只保留在本地，不提交
├── docs/                  # 架构设计、格式证据与验证路线
└── reference/python/      # 原样保留的 15 个历史参考脚本
```

详细职责、依赖方向和下一步见 [架构设计](docs/architecture.md)。

## 构建与测试

需要支持 **C++17** 的编译器和 **CMake 3.20+**。当前无需 Python、FLTK、图像库或其他第三方依赖；构建不会下载依赖，也不会调用 Git。

以下命令在项目根目录的 PowerShell 中运行。不同工具链使用不同构建目录，不共享 CMake 缓存。

### MinGW

将 `g++` 和 `mingw32-make` 所在目录加入当前终端 PATH：

```powershell
cmake -S . -B build/mingw -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/mingw --parallel
ctest --test-dir build/mingw --output-on-failure

.\build\mingw\bin\eagls.exe --help
.\build\mingw\bin\eagls.exe --version
```

运行 MinGW 编译的程序时，其运行库 DLL 也需要可被系统找到；开发机上通常由同一工具链的 PATH 提供。当前未制作独立发布包。

### Visual Studio 2022

需要安装 C++ 桌面开发工具与 Windows SDK；此生成器需要 CMake 3.21+：

```powershell
cmake -S . -B build/msvc -G "Visual Studio 17 2022" -A x64
cmake --build build/msvc --config Release --parallel
ctest --test-dir build/msvc -C Release --output-on-failure

.\build\msvc\bin\Release\eagls.exe --help
```

### 仅构建核心

```powershell
cmake -S . -B build/core-only -DEAGLS_BUILD_CLI=OFF -DBUILD_TESTING=OFF
cmake --build build/core-only --config Release --parallel
```

| CMake 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `EAGLS_BUILD_CLI` | `ON` | 构建 `eagls_cli` 目标，程序名为 `eagls` |
| `BUILD_TESTING` | `ON` | 构建并注册测试；关闭 CLI 时仍可测试核心 |

核心目标为 `eagls_core`，供其他目标通过 `eagls::core` 链接。目前没有 GUI 构建开关，因为 GUI 尚未实现。

## 当前 CLI 行为

- 无参数、`--help`、`-h`：显示帮助，退出码 `0`。
- `--version`：输出 `eagls` 和核心版本，退出码 `0`。
- `eagls pak info <idx> <pak>`：显示归档信息（种子、条目数、数据大小、布局）。
- `eagls pak list <idx> <pak>`：列出全部条目（槽位、名称、物理偏移、大小）。
- `eagls pak extract <idx> <pak> [--out DIR] [--entry NAME]... [--overwrite]`：按索引原样提取条目；
  默认输出到 `<pak 所在目录>/<pak 主文件名>_out`，`--entry` 可重复并可带目录前缀。
- `eagls dat decrypt <文件或目录> --out DIR [--overwrite]`：导出保留标签表/终止符/种子的解密 DAT，不能直接回填游戏。
- `eagls dat text <文件或目录> --out DIR [--overwrite]`：导出 `.txt` 正文，保留原编码与换行，不自动转码。
- `eagls gr decode <文件或目录> --out DIR [--overwrite]`：导出 24/32 位 BMP；修正大小字段，丢弃像素后的填充。
- `eagls text extract <文件或目录> --out FILE.json [--overwrite]`：提取含日文文本与说话者的 JSON 清单。
- `eagls text apply <文件或目录> --table FILE.json --out DIR [--overwrite] [--encoding cp932|gbk]`：按清单重建加密 DAT；译文可用 GBK（配合 exe 补丁）。
- `eagls exe check <EAGLS.exe>`：定位 GBK 中文补丁位置并报告当前状态，不写文件。
- `eagls exe patch <EAGLS.exe> [--out FILE] [--overwrite]`：写出补丁副本（默认 `<主文件名>-cn.exe`），拒绝覆盖输入本身。
- 内容命令的 `--out` 始终表示目录，目录输入只处理当前层的相应扩展名；不允许就地覆盖输入。
- 未知参数、额外参数、尚未实现的命令、参数用法错误：写入标准错误，退出码 `2`；归档、解码或写入失败：退出码 `1`。

默认 CTest 包含 27 项（版本、解码与 exe 补丁边界、CLI 参数），不需要真实游戏资源。
将 `EAGLS_LOCAL_CONTENT_DIR` 显式设为已提取样本的绝对路径后，额外启用两项真实样本回归
（内容解码与损坏输入、EXE 补丁端到端并校验产物 SHA-256），共 29 项。
归档提取和内容解码另有全量独立校验，见 [归档证据](docs/eagls-archive.md) 与 [DAT/GR 证据及复验命令](docs/eagls-content.md)。
Python 只用于手动交叉验证，不是核心/CLI/默认 CTest 的依赖。**这不代表全部游戏版本兼容或回填游戏验收。**

## 历史参考与样本

- [参考脚本说明](reference/README.md)：仅供阅读，不属于构建、测试或运行时依赖。
- [样本管理约定](tests/fixtures/README.md)：真实游戏资源在本地验证，不提交到仓库。

## 许可证

项目许可证见 [LICENSE](LICENSE)。游戏资源权利归原权利人；历史参考脚本的来源与许可需单独核实，不能仅凭放在本仓库内就推定其许可。
