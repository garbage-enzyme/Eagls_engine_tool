# EAGLS Engine Tool

> **项目正在重启，目前只有可构建、可测试的工程骨架。尚未实现可用的资源处理功能，也未验证任何真实游戏的兼容性。**
>
> 旧 C++ 移植代码、旧可执行程序及分散的工程文件已经移除。旧版使用说明不再适用，请勿把历史版本或构建成功视为资源处理正确的证明。

## 目标与现状

从真实 EAGLS 游戏资源及游戏行为出发，实现原生 C++ 工具。面向资源提取与汉化工作流，独立核心由 CLI 与未来 GUI 共用。

| 项目 | 当前状态 |
| --- | --- |
| C++17 核心静态库、统一 CMake、CTest | 已建立工程骨架 |
| CLI 帮助、版本、非法参数处理 | 已实现 |
| IDX/PAK 检查、列目录、原样提取 | 未实现，下一阶段目标 |
| 内容解密、GR 图像编解码、文本回填、回包 | 未实现 |
| GUI | 暂定 FLTK，尚未接入 |
| 真实游戏兼容性 | 未验证 |

不以旧 C++ 或参考 Python 的输出作为正确性标准，也不把程序内部往返自洽等同于游戏兼容。

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
│   └── fixtures/          # 测试样本约定，暂无游戏样本
├── docs/                  # 架构设计与验证路线
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
- 未知参数、额外参数、尚未实现的命令：写入标准错误，退出码 `2`。

当前测试只验证核心链接、版本一致性和 CLI 行为，**不代表已通过格式或游戏兼容性测试**。

## 历史参考与样本

- [参考脚本说明](reference/README.md)：仅供阅读，不属于构建、测试或运行时依赖。
- [样本管理约定](tests/fixtures/README.md)：真实游戏资源在本地验证，不提交到仓库。

## 许可证

项目许可证见 [LICENSE](LICENSE)。游戏资源权利归原权利人；历史参考脚本的来源与许可需单独核实，不能仅凭放在本仓库内就推定其许可。
