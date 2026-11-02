# 项目执行规则

## Windows 环境

- 使用原生 Windows 和 PowerShell；不使用 WSL、Git Bash 或 `cmd /c`。
- 优先用 `rg` 搜索。普通修改使用可审查的小补丁，复杂批量重写使用支持 `--dry-run` / `--apply` 的 Python 脚本。
- 不安装全局依赖，不删除用户文件或构建目录，不执行破坏性 Git 命令。

## 固定构建与程序输出目录

- CMake 配置和中间文件默认放在仓库根目录 `build/`。
- 唯一正式 Release 程序入口为 `dist/Release/DiskCleaner.exe`。
- Debug 程序入口为 `dist/Debug/DiskCleaner.exe`；其他配置同样使用 `dist/<配置>/`。
- 输出目录必须由根目录 `CMakeLists.txt` 的目标属性控制；不能因为使用不同生成器或构建缓存目录而改变程序输出位置。
- 构建完成后，`windeployqt` 自动将 Qt DLL、平台插件和编译器运行库部署到程序所在目录；分发时复制整个配置目录。
- 不向项目根目录、临时目录或其他构建目录复制第二份正式程序；旧的 `build/DiskCleaner.exe` 不是有效交付入口。
- 验证脚本默认运行 `dist/Release/DiskCleaner.exe`。测试程序和界面预览工具保留在 `build/`，不混入分发目录。
- `build/`、`dist/` 和验证产物不提交到 Git。修改路径约定时，必须同时更新 CMake、README 和所有相关脚本。

## 应用图标

- `resources/app.png` 保存原始图标，`resources/app.ico` 为 Windows 多尺寸图标。
- 图标必须同时通过 `resources/app.rc` 嵌入 EXE，并通过 Qt 资源与 `QApplication::setWindowIcon` 设置窗口图标，覆盖资源管理器、任务栏与窗口切换器。
- 更新图标后重新构建并检查 ICO 尺寸、EXE 图标资源和窗口图标。
