# 项目执行规则

## Windows 环境

- 使用原生 Windows 和 PowerShell；不使用 WSL、Git Bash 或 `cmd /c`。
- 优先用 `rg` 搜索。普通修改使用可审查的小补丁，复杂批量重写使用支持 `--dry-run` / `--apply` 的 Python 脚本。
- 不安装全局依赖，不删除用户文件，不执行破坏性 Git 命令。清理本项目构建缓存须有明确授权，并先预览目标和占用。

## 编译、打包与验证

| 用途 | 仓库内唯一位置 |
| --- | --- |
| CMake 配置、生成器文件和 CTest 日志 | `build/` |
| Cargo 编译、测试、Clippy 及构建脚本产物 | `build/cargo/` |
| 可分发程序和 x64 VC 运行库 | `dist/<配置>/` |
| 验证截图、导出和日志 | `work/` |

- 从仓库根目录、Windows x64 Developer PowerShell 执行 README 的 CMake 命令。日常开发验证和交付默认使用 Release。只有需要调试时才构建 Debug，复用 `build/`，结束后切回 Release。
- CMake 统一调用 Cargo 并使用 `Cargo.lock`。直接运行 Cargo 也使用仓库的 `.cargo/config.toml`，例如 `cargo test --locked --release` 和 `cargo clippy --locked --release --all-targets`。避免通过环境变量、`--target-dir` 或 IDE 配置把 target 或 build-dir 指向其他位置。
- 仅由根 `CMakeLists.txt` 的目标属性指定部署位置。正式验证运行 `dist/Release/DiskCleaner.exe`，同目录包含 `vcruntime140.dll`。使用 `ctest --test-dir build -C Release --output-on-failure` 检查回归和分发清单。编译输出和验证产物不提交，修改路径约定时同步更新 CMake、Cargo 配置、README 和验证脚本。

## 控制缓存占用

- 复用同一工具链、配置和目标目录。项目默认关闭 Cargo 增量编译，保留当前配置的依赖缓存；避免为每次任务、生成器或分支创建独立构建目录。
- 切换工具链、依赖集或发现磁盘占用异常时，先按配置统计 `build/cargo/`。只清理已确认废弃的配置。以 Debug 为例，先运行 `cargo clean --profile dev --target-dir build/cargo --dry-run` 查看范围，确认后再执行同一命令去掉 `--dry-run`。
- 清理前核对仓库绝对路径并等待编译结束。常规清理只针对本项目中间产物，保留分发程序、测试样本和用户级 Cargo 缓存。

## 应用图标

- `resources/app.png` 保存原始图标，`resources/app.ico` 为 Windows 多尺寸图标。
- 图标必须同时通过 `resources/app.rc` 嵌入 EXE，并从 Rust 为 GPUI 的 HWND 设置大小窗口图标，覆盖资源管理器、任务栏与窗口切换器。
- 更新图标后重新构建并检查 ICO 尺寸、EXE 图标资源和窗口图标。
