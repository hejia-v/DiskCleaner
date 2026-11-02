#include "i18n.h"

#include <QHash>
#include <QTranslator>
#include <QString>

namespace {

class DictTranslator : public QTranslator {
public:
    explicit DictTranslator(QHash<QString, QString> dict, QObject* parent = nullptr)
        : QTranslator(parent), dict_(std::move(dict))
    {
    }

    QString translate(const char* context, const char* sourceText, const char* disambiguation,
                      int n) const override
    {
        Q_UNUSED(context);
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (!sourceText) return {};
        const auto it = dict_.constFind(QString::fromUtf8(sourceText));
        return it == dict_.constEnd() ? QString() : it.value();
    }

    bool isEmpty() const override { return dict_.isEmpty(); }

private:
    QHash<QString, QString> dict_;
};

QHash<QString, QString> makeChinese()
{
    static const struct {
        const char* en;
        const char* zh;
    } table[] = {
        {"Scan location", "扫描位置"},
        {"Drive", "磁盘"},
        {"Choose folder...", "选择文件夹…"},
        {"Choose or enter a folder path", "选择或输入文件夹路径"},
        {"Start scan", "开始扫描"},
        {"Standard scan mode", "普通扫描模式"},
        {"Fast scan available", "可使用快速扫描"},
        {"Administrator rights allow faster NTFS drive scans. Folder scans work without them.", "管理员权限可加速 NTFS 整盘扫描。扫描文件夹无需管理员权限。"},
        {"Ready to scan", "准备扫描"},
        {"Choose a location to begin", "选择位置后开始扫描"},
        {"Space on disk", "磁盘占用"},
        {"File size", "文件大小"},
        {"Size metric:", "统计口径："},
        {"About size metrics", "了解统计口径"},
        {"Lists and treemap use the same size metric. Percentages in lists are relative to the displayed scan; zooming the treemap only changes the area being viewed.", "列表与空间图使用同一大小口径。列表百分比相对于当前扫描范围；缩放空间图只改变查看的目录。"},
        {"File types", "文件类型"},
        {"File types for the selected item; percentages use the scan range.", "统计选中项目的文件类型；百分比以当前扫描范围为分母。"},
        {"Directory usage", "目录占用"},
        {"Filter files or folders", "筛选文件或文件夹"},
        {"Choose a drive or folder, then start a scan.", "选择磁盘或文件夹，点击「开始扫描」查看占用。"},
        {"Space distribution", "空间分布"},
        {"Space analysis", "空间分析"},
        {"Right-click to choose columns", "右键选择要显示的列"},
        {"Choose an existing folder to scan.", "请选择一个已存在的文件夹。"},
        {"Scan complete", "扫描完成"},
        {"Scan canceled", "扫描已取消"},
        {"This scan · %1", "本次扫描 · %1"},
        {"Previous scan · %1", "上次扫描 · %1"},
        {"Previous filtered scan · %1", "上次筛选结果 · %1"},
        {"Filtered scan · %1", "筛选结果 · %1"},
        {"Drive capacity", "所属磁盘容量"},
        {"Drive capacity · %1", "所属磁盘 · %1"},
        {"Available after scanning", "扫描后显示磁盘容量"},
        {"Total %1", "总容量 %1"},
        {"Used %1 (%2%)", "已用 %1（%2%）"},
        {"Free %1", "可用 %1"},
        {"Multiple locations", "多个扫描位置"},
        {"Imported scan", "导入的扫描数据"},
        {"Drive capacity unavailable", "无可用的所属磁盘容量信息"},
        {"Select an item to view its details", "选择项目查看详情"},
        {"Selected: %1 · %2 %3", "已选：%1 · %2 %3"},
        {"%1 items selected", "已选择 %1 个项目"},
        {"No scan results yet", "尚无扫描结果"},
        {"Area is proportional to bytes in this directory. Small hidden cells keep their area.", "面积按当前目录的实际字节数分配。隐藏的小色块仍保留其面积。"},
        {"Use space on disk in all views", "所有视图使用磁盘占用口径"},
        {"Capacity overlays are available for full drives in space-on-disk mode.", "可用与未知空间仅在整盘的「磁盘占用」口径下显示。"},
        {"Name", "名称"},
        {"File count", "文件数"},
        {"% of Scan", "占扫描范围"},
        {"Duplicate file size", "重复文件大小"},
        {"No non-zero files in this view", "当前范围没有大小非零的文件"},
        {"Scan a drive or folder to see its space distribution", "扫描磁盘或文件夹后，查看空间分布"},
        {"Scan complete in %1 s (%2 records, %3 items)", "扫描完成，用时 %1 秒（%2 条记录，%3 个项目）"},
        {"Select Folder", "选择文件夹"},
        {"Copy", "复制"},
        {"Cut", "剪切"},
        {"Back (Backspace)", "后退（退格键）"},
        {"Forward (Shift+Backspace)", "前进（Shift+退格键）"},
        {"Scan root (Ctrl+Home)", "扫描根目录（Ctrl+Home）"},
        {"Detail: ", "层级："},
        {"Visible folder levels (Ctrl +/-)", "显示目录层数（Ctrl +/-）"},
        {"File classes", "文件分类配色"},
        {"Chart options", "图表选项"},
        {"Zoom into folder", "进入目录"},
        {"Tag", "标签"},
        {"Red (1)", "红色（1）"},
        {"Yellow (2)", "黄色（2）"},
        {"Green (3)", "绿色（3）"},
        {"Blue (4)", "蓝色（4）"},
        {"Clear tags in this view (0)", "清除当前子树标签（0）"},
        {"Show unknown space", "显示未知空间"},
        {"Unknown space", "未知空间"},
        {"Minimum cell size...", "最小色块尺寸…"},
        {"Minimum cell size", "最小色块尺寸"},
        {"Pixels:", "像素："},
        {"%1\nLogical: %2\nAllocated: %3\n%4% of scanned size", "%1\n逻辑大小：%2\n分配大小：%3\n占扫描总量 %4%"},
        {"Modified: %1", "修改时间：%1"},
        // menus / actions
        {"&File", "文件(&F)"},
        {"&Options", "选项(&O)"},
        {"&Help", "帮助(&H)"},
        {"&Scan", "扫描(&S)"},
        {"Select &Folder...", "选择文件夹(&F)..."},
        {"&Export to CSV file...", "导出为 CSV 文件(&E)..."},
        {"Save Treemap Image as .png file...", "保存树图为 .png 图片..."},
        {"Export File &Types to CSV...", "导出文件类型到 CSV(&T)..."},
        {"E&xit", "退出(&X)"},
        {"Display Dynamic values", "动态显示数值"},
        {"Display Byte values", "按字节显示"},
        {"Display KB values", "按 KB 显示"},
        {"Display MB values", "按 MB 显示"},
        {"Display GB values", "按 GB 显示"},
        {"Display TB values", "按 TB 显示"},
        {"Show Folder and File Names on Treemap", "在树图上显示文件夹和文件名"},
        {"Show Allocated Size on Treemap", "在树图上显示分配大小"},
        {"Show Free Space on Treemap", "在树图上显示空闲空间"},
        {"&About...", "关于(&A)..."},
        {"Language", "语言"},
        {"Restart DiskCleaner to apply the new language?", "重新启动 DiskCleaner 以应用新语言？"},
        {"English", "English"},
        {"Chinese (Simplified)", "简体中文"},
        // top bar / labels
        {"Select:", "选择:"},
        {"Selection:", "选择:"},
        {"Total:", "总空间:"},
        {"Used:", "已用:"},
        {"Free:", "可用:"},
        {"Scan", "扫描"},
        {"Cancel", "取消"},
        {"Ready", "就绪"},
        {"Scanning...", "正在扫描..."},
        {"Canceling...", "正在取消..."},
        {"Scan failed", "扫描失败"},
        {"Scan complete in %1 s (%2 records)", "扫描完成，用时 %1 秒（%2 条记录）"},
        {"%1 files, %2 folders", "%1 个文件，%2 个文件夹"},
        {"%1 files, %2 folders, %3 duplicate groups", "%1 个文件，%2 个文件夹，%3 个重复组"},
        // tabs
        {"Tree View", "树视图"},
        {"File View", "文件列表"},
        {"About", "关于"},
        // file view controls
        {"Filter:", "过滤:"},
        {"Include:", "包含:"},
        {"Exclude:", "排除:"},
        {"Apply", "应用"},
        {"Clear", "清除"},
        {"substring", "子串"},
        {"Match file name only", "仅匹配文件名"},
        {"Match entire path", "匹配完整路径"},
        {"Filter applied: %1 items", "已应用过滤：%1 项"},
        {"Filter cleared", "已清除过滤"},
        {"Filter Scan Results", "过滤扫描结果"},
        {"UI Theme", "界面主题"},
        {"Check for updates", "检查更新"},
        {"Check for updates now", "立即检查更新"},
        {"Update check is disabled: no endpoint configured.", "更新检查已禁用：未配置端点。"},
        {"Error checking for updates: %1", "检查更新出错：%1"},
        {"No update found.", "未发现新版本。"},
        {"An update is available (%1). Current version: %2.", "发现新版本（%1），当前版本：%2。"},
        {"Up one level", "上一级"},
        {"Auto", "自动"},
        {"Light", "浅色"},
        {"Dark", "深色"},
        {"Windows Explorer context menu", "Windows 资源管理器右键菜单"},
        {"Failed to update Windows Explorer context menu", "更新 Windows 资源管理器右键菜单失败"},
        {"Open", "打开"},
        {"Explore Folder", "在资源管理器中打开"},
        {"Command Prompt Here", "在此处打开命令行"},
        {"Copy Path", "复制路径"},
        {"Copied %1 item(s)", "已复制 %1 个项目"},
        {"Cut %1 item(s)", "已剪切 %1 个项目"},
        {"Properties", "属性"},
        {"Delete (to Recycle Bin)", "删除（到回收站）"},
        {"Delete", "删除"},
        {"Move %1 item(s) to the Recycle Bin?", "将 %1 个项目移到回收站？"},
        {"Moved %1 item(s) to the Recycle Bin", "已将 %1 个项目移到回收站"},
        {"Live update: %1 files, %2 folders", "实时更新：%1 个文件，%2 个文件夹"},
        {"Notifications overflowed; rescanning to recover changes...", "文件变更通知溢出，正在补偿扫描…"},
        {"Cannot monitor %1 (Windows error %2)", "无法监听 %1（Windows 错误 %2）"},
        {"Cannot create file notification event (%1)", "无法创建文件监听事件（%1）"},
        {"Live monitoring stopped for %1 (Windows error %2)", "%1 的实时监听已停止（Windows 错误 %2）"},
        {"Cannot refresh %1; waiting for the next file change", "暂时无法更新 %1，等待下一次文件变更"},
        {"Delete failed", "删除失败"},
        {"Copied %1 path(s) to clipboard", "已复制 %1 个路径到剪贴板"},
        {"Copy file and size info to clipboard", "复制文件与大小信息到剪贴板"},
        {"Copied tree info to clipboard", "已复制树信息到剪贴板"},
        {"Rename failed: %1", "重命名失败：%1"},
        {"Dropped %1 files are not supported yet", "暂不支持拖入的 %1 文件"},
        {"Match file name", "匹配文件名"},
        {"Folders", "文件夹"},
        {"Max files:", "最大文件数:"},
        {"ALL", "全部"},
        {"Don't locate duplicates", "不检测重复文件"},
        {"Locate by Name, Size", "按名称、大小定位重复"},
        {"Locate by Name, Size, Date", "按名称、大小、日期定位重复"},
        {"Duplicates only", "仅显示重复项"},
        // warning bar
        {"Administrator rights are required to read the NTFS $MFT directly. Without them the scan "
         "falls back to a much slower directory walk.",
         "直接读取 NTFS $MFT 需要管理员权限。没有管理员权限时将退回到慢得多的目录遍历扫描。"},
        {"Restart as Administrator", "以管理员身份重新启动"},
        {"Don't show again", "不再显示"},
        // tree headers
        {"Folder", "文件夹"},
        {"% of Parent", "父级百分比"},
        {"Size", "大小"},
        {"Allocated", "分配"},
        {"Items", "项目"},
        {"Files", "文件"},
        {"Modified", "修改时间"},
        {"Attributes", "属性"},
        // file view headers
        {"File Name", "文件名"},
        {"Path", "路径"},
        {"% of Drive", "占驱动器百分比"},
        {"Dup Count", "重复数"},
        {"Dup Size", "重复大小"},
        // extension panel
        {"Extension", "扩展名"},
        {"File Type", "文件类型"},
        {"Percent", "百分比"},
        {"(No Extension)", "(无扩展名)"},
        // treemap
        {"Free space", "空闲空间"},
        // export dialogs
        {"Export", "导出"},
        {"Open MFT Dump...", "打开 MFT 转储..."},
        {"Select CSV file...", "选择 CSV 文件..."},
        {"Select Multiple...", "多选..."},
        {"Select Multiple", "多选"},
        {"Add Folder...", "添加文件夹..."},
        {"Add Folder", "添加文件夹"},
        {"Remove", "移除"},
        {"Select All", "全选"},
        {"Select None", "全不选"},
        {"Scanning %1 of %2: %3", "正在扫描第 %1/%2 项：%3"},
        {"Multiple selections", "多选合并"},
        {"No targets scanned", "没有可扫描的目标"},
        {"Select CSV file", "选择 CSV 文件"},
        {"Dump MFT File...", "转储 MFT 文件..."},
        {"Dump MFT File", "转储 MFT 文件"},
        {"Dump MFT", "转储 MFT"},
        {"Open MFT Dump", "打开 MFT 转储"},
        {"Please select a drive first.", "请先选择一个驱动器。"},
        {"Dumping MFT file, please wait...", "正在转储 MFT 文件，请稍候..."},
        {"MFT data dumped to file %1", "MFT 数据已转储到 %1"},
        {"MFT Dump cancelled...", "MFT 转储已取消..."},
        {"Export complete.", "导出完成。"},
        {"Export Files", "导出文件"},
        {"Export Folders", "导出文件夹"},
        {"Export drive size information", "导出驱动器容量信息"},
        {"Export \"Last Accessed\" and \"Created\" dates", "导出“最后访问”和“创建”日期"},
        {"Export total size and allocated size of folders (excluding sub folders)", "导出文件夹自身总大小与分配大小（不含子文件夹）"},
        {"Split file names into root, folder, filename and extension", "拆分文件名（根/目录/文件名/扩展名）"},
        {"Export percentage of parent value", "导出占父级百分比"},
        {"Export MFT record number", "导出 MFT 记录号"},
        {"Selected only", "仅选中项"},
        {"Max folder depth (0 = no limit):", "最大文件夹深度（0 = 不限）："},
        {"no limit", "不限"},
        {"Cannot write %1", "无法写入 %1"},
        {"Scan something first.", "请先扫描。"},
        {"Save Treemap Image as .png file", "保存树图为 .png 图片"},
        {"Save Treemap Image", "保存树图"},
        {"Width:", "宽度:"},
        {"Height:", "高度:"},
        {"Shades of Gray (single color)", "灰阶（单色）"},
        {"Show Free Space", "显示空闲空间"},
        {"Save", "保存"},
        {"Cancel", "取消"},
        {"PNG files (*.png)", "PNG 图片 (*.png)"},
        {"CSV files (*.csv);;All files (*.*)", "CSV 文件 (*.csv);;所有文件 (*.*)"},
        {"CSV files (*.csv)", "CSV 文件 (*.csv)"},
        {"Export to CSV", "导出 CSV"},
        {"Export File Types", "导出文件类型"},
        {"Treemap image saved: %1 x %2", "树图已保存：%1 x %2"},
        {"Error", "错误"},
        {"Path not found: %1", "路径不存在：%1"},
        // about dialog
        {"About DiskCleaner", "关于 DiskCleaner"},
        // file types
        {"Text Document", "文本文档"},
        {"Configuration", "配置文件"},
        {"Application", "应用程序"},
        {"Application Extension", "应用程序扩展"},
        {"System File", "系统文件"},
        {"Image", "图像"},
        {"Icon", "图标"},
        {"Audio", "音频"},
        {"Video", "视频"},
        {"Archive", "压缩文件"},
        {"PDF Document", "PDF 文档"},
        {"Word Document", "Word 文档"},
        {"Excel Worksheet", "Excel 工作表"},
        {"PowerPoint", "演示文稿"},
        {"HTML Document", "HTML 文档"},
        {"Style Sheet", "样式表"},
        {"Script", "脚本"},
        {"JSON File", "JSON 文件"},
        {"XML Document", "XML 文档"},
        {"CSV File", "CSV 文件"},
        {"TSV File", "TSV 文件"},
        {"Batch File", "批处理文件"},
        {"Python Script", "Python 脚本"},
        {"C++ Source", "C++ 源文件"},
        {"C/C++ Header", "C/C++ 头文件"},
        {"C Source", "C 源文件"},
        {"Qt Translation", "Qt 翻译文件"},
        {"File", "文件"},
    };
    QHash<QString, QString> dict;
    dict.reserve(int(std::size(table)));
    for (const auto& e : table) dict.insert(QString::fromUtf8(e.en), QString::fromUtf8(e.zh));
    return dict;
}

}  // namespace

QTranslator* createAppTranslator(const QString& language)
{
    if (language.compare(QLatin1String("zh"), Qt::CaseInsensitive) == 0 ||
        language.startsWith(QLatin1String("zh_"), Qt::CaseInsensitive) ||
        language.startsWith(QLatin1String("zh-"), Qt::CaseInsensitive)) {
        return new DictTranslator(makeChinese());
    }
    return nullptr;
}







