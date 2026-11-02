# 应用图标

使用内置 imagegen 生成透明背景原图 `app.png`；`app.ico` 为同一图像的多尺寸
Windows 编码。EXE 通过 `app.rc` 使用该图标，Qt 通过 `ui.qrc` 使用该图标。

生成提示词：

> Create a single Windows desktop application icon for DiskCleaner, a disk space analyzer and cleaner. Centered standalone app mark, transparent outside, square composition. Modern restrained professional utility aesthetic matching a warm white and charcoal desktop UI with muted periwinkle accent. A simple rounded charcoal disk enclosure, a warm ivory circular disk inset and a bold small four-point periwinkle cleaning sparkle at upper right. Strong silhouette and thick simple shapes readable at 16 and 32 pixels. Flat vector-like precision, subtle depth only, no tiny details, no letters, no text, no watermark, no mockup, no background scene. Icon fills 88 percent of canvas. Deliver one icon.

在仓库根目录使用已有 Pillow 转换 ICO（替换原图后运行）：

```powershell
python -c "from PIL import Image; Image.open('resources/app.png').save('resources/app.ico', sizes=[(s,s) for s in (16,20,24,32,40,48,64,128,256)])"
```

转换后重新构建。如果 Windows 对以前固定到任务栏的快捷方式仍显示缓存图标，
取消固定旧快捷方式，再从 `dist/Release/DiskCleaner.exe` 固定即可。
