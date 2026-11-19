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
| 所有配置共用的唯一打包目录 | `dist/` |
| 验证截图、导出和日志 | `work/` |

- 从仓库根目录、Windows x64 Developer PowerShell 执行 README 的 CMake 命令。日常开发验证和交付默认使用 Release。只有需要调试时才构建 Debug，复用 `build/`，结束后切回 Release。
- CMake 统一调用 Cargo 并使用 `Cargo.lock`。直接运行 Cargo 也使用仓库的 `.cargo/config.toml`，例如 `cargo test --locked --release` 和 `cargo clippy --locked --release --all-targets`。避免通过环境变量、`--target-dir` 或 IDE 配置把 target 或 build-dir 指向其他位置。
- 仅由根 `CMakeLists.txt` 的目标属性指定部署位置。所有配置共用 `dist/DiskCleaner.exe` 和 `dist/vcruntime140.dll`。禁止另建配置、版本或日期打包目录。正式交付前最后构建 Release，使用 `ctest --test-dir build -C Release --output-on-failure` 核对分发清单及 EXE 与当前配置的哈希。编译输出和验证产物不提交，修改路径约定时同步更新相关配置、README 和验证脚本。
- 切换配置和重复打包均在唯一发布路径原位覆盖。先比较 EXE 和运行库的内容，相同文件跳过。需要替换时，按目标 EXE 的绝对路径识别并终止该路径启动的所有实例，等待退出后再覆盖；这是打包流程的既定授权，无需另行询问。其他路径的同名进程继续运行。
- 打包失败时修复原因并重试同一目标。禁止通过另存带版本号、时间戳、序号或 `.bak` 的副本来绕过占用。部署临时文件在替换后清理。

## 控制缓存占用

- 复用同一工具链、配置和目标目录。项目默认关闭 Cargo 增量编译，保留当前配置的依赖缓存；避免为每次任务、生成器或分支创建独立构建目录。
- 切换工具链、依赖集或发现磁盘占用异常时，先按配置统计 `build/cargo/`。只清理已确认废弃的配置。以 Debug 为例，先运行 `cargo clean --profile dev --target-dir build/cargo --dry-run` 查看范围，确认后再执行同一命令去掉 `--dry-run`。
- 清理前核对仓库绝对路径并等待编译结束。常规清理只针对本项目中间产物，保留分发程序、测试样本和用户级 Cargo 缓存。
- 冒烟测试成功后，`tools/smoke.ps1` 在 `work/smoke/` 保留最近 5 次已完成的结果。失败时保留现场，等下次成功再轮换。可运行 `python tools/prune_smoke.py --dry-run` 预览清单。

## 应用图标

- `resources/app.png` 保存原始图标，`resources/app.ico` 为 Windows 多尺寸图标。
- 图标必须同时通过 `resources/app.rc` 嵌入 EXE，并从 Rust 为 GPUI 的 HWND 设置大小窗口图标，覆盖资源管理器、任务栏与窗口切换器。
- 更新图标后重新构建并检查 ICO 尺寸、EXE 图标资源和窗口图标。
