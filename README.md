# DiskCleaner — Qt 磁盘空间分析器

本目录包含 C++20 + Qt 6 程序、测试和构建配置。

## 构建与运行

本机使用 Qt 6.11.2、MinGW 13.1、CMake 和 Ninja：

```powershell
Set-Location C:\Dev\DiskCleaner
$env:PATH = 'C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;C:\Qt\6.11.2\mingw_64\bin;' + $env:PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/mingw_64 -DDISK_CLEANER_BUILD_TESTS=ON
cmake --build build -j 8
.\build\DiskCleaner.exe
```

## 验证

在上述环境下运行：

```powershell
$env:QT_FORCE_STDERR_LOGGING = '1'
ctest --test-dir build --output-on-failure
pwsh -NoProfile -File tools\smoke.ps1
```

`tests/` 包含自动化回归测试，`work/sample` 和 `work/dups` 是冒烟测试所需的输入夹具；测试输出写入 `work/smoke`。`smoke/` 是独立的 Qt 环境检查程序。

`ResourceTests` 覆盖异常 MFT 长度/记录数、稀疏记录编号、扫描取消与句柄释放、实时增删/改名后的存储压缩和界面状态，以及后台 MFT 导出和异步关闭。导出界面测试使用可控的 I/O 替身，不需要管理员权限或访问真实磁盘；单个 CTest 测试目标超时为 90 秒。

使用 `build\DiskCleaner.exe --help` 查看命令行参数。

## 主界面与统计口径

主界面默认采用浅色主题；磁盘与文件夹扫描均可从顶部开始。文件夹支持直接输入路径，
也可先选择位置，再点击「开始扫描」。菜单中的多位置、MFT、CSV、导出等入口保留。

「文件大小 / 磁盘占用」同时控制目录树、文件列表、文件类型百分比及空间图。
列表的「占扫描范围」以当前显示的扫描结果为分母，应用全局筛选后以筛选结果为分母。
文件列表自身的搜索和显示数量限制不改变该分母。进入空间图的子目录只改变图中查看的范围。
CSV 导出仍分别保存 Size 与 Allocated 两列，重复文件识别仍使用原有的名称、文件大小、日期策略。

所属磁盘容量与本次扫描总量分开显示。导入的扫描及多个位置合并的扫描不套用本机磁盘容量。
可用、未知空间仅在整盘的磁盘占用口径下显示。色块按实际字节数分配，隐藏小色块不会放大其他色块。

「文件类型」按钮可展开统计面板，表头右键可选择显示的列；原有的导航、标记、筛选、
深色主题与文件操作保持可用。扫描失败或取消后保留上次结果，并标明其归属。

运行主界面截图验证（需要本机已有的 Pillow）：

```powershell
python tools\verify_mainwindow.py --dry-run
python tools\verify_mainwindow.py --apply
```

该脚本捕获浅色、深色、空状态、文件列表、紧凑窗口及 150% 缩放画面，将证据写入
`work/ui-redesign/`，完成后恢复用户设置。
