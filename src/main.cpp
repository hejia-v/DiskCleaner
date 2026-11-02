#include <QApplication>
#include <QIcon>
#include <cstdio>
#include <QSettings>
#include <QTranslator>
#include <QDebug>
#include <QTimer>

#include "core.h"
#include "i18n.h"
#include "shellintegration.h"
#include "theme.h"
#include "updatecheck.h"

#ifndef APP_VERSION
#define APP_VERSION "0.1"
#endif
#include "mainwindow.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("DiskCleaner"));
    QApplication::setApplicationName(QStringLiteral("DiskCleaner"));
    QApplication::setApplicationVersion(QStringLiteral("0.1"));
    QApplication::setApplicationDisplayName(QStringLiteral("DiskCleaner"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/ui/app.ico")));


    if (app.arguments().contains(QStringLiteral("--help")) ||
        app.arguments().contains(QStringLiteral("-h"))) {
        const char* text =
            "DiskCleaner " APP_VERSION "\n"
            "\n"
            "Usage: DiskCleaner [--scan <path>] [options]\n"
            "\n"
            "Scanning\n"
            "  --scan <path>            scan a folder, MFT dump (.mft/.bin) or CSV (.csv/.tsv)\n"
            "  --scan a;b               merge several roots into one tree\n"
            "  --scan-mft <file>        load an MFT dump and print a summary\n"
            "  --scan-csv <file>        import a CSV/TSV and print a summary\n"
            "  --scan-multi \"a;b\"       merge several roots (headless)\n"
            "  --make-mft <file> <n>    write a synthetic MFT image with n files\n"
            "\n"
            "Export\n"
            "  --export <file.csv>      export the tree view (default columns)\n"
            "  --export-full <file.csv> export with every optional column\n"
            "  --treemap-png <file.png> render the treemap to a 1920x1080 PNG\n"
            "  --recycle <path>         move a file/folder to the Recycle Bin\n"
            "  --rename <path> <name>   rename a file/folder\n"
            "\n"
            "View\n"
            "  --tab <0..2>             select the initial tab (tree/files/about)\n"
            "  --dup <0|1|2>            duplicate detection mode\n"
            "  --dup-only               show duplicates only\n"
            "  --filter <text>          apply a substring filter after scanning\n"
            "  --filter-path            match the full path instead of the name\n"
            "  --sort <column>          sort the file view by column\n"
            "  --sort-desc              descending sort\n"
            "  --theme auto|light|dark  UI theme\n"
            "  --lang en|zh_CN          UI language\n"
            "\n"
            "Other\n"
            "  --check-update <url>     run one update check against url\n"
            "  --shell-install          add Explorer context-menu entries\n"
            "  --shell-uninstall        remove them\n"
            "  --selftest               run the NTFS record parser self-test\n"
            "  --shot <file.png> --delay <ms>  save a window screenshot and exit\n"
            "  --help                   show this help\n";
        fputs(text, stdout);
        return 0;
    }

    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("DiskCleaner"),
                       QStringLiteral("DiskCleaner"));
    QString language = settings.value(QStringLiteral("language"), QStringLiteral("en")).toString();
    QString theme = settings.value(QStringLiteral("theme"), QStringLiteral("light")).toString();
    for (int i = 1; i < app.arguments().size(); ++i) {
        if (app.arguments()[i] == QLatin1String("--lang") && i + 1 < app.arguments().size())
            language = app.arguments()[i + 1];
        else if (app.arguments()[i] == QLatin1String("--theme") &&
                 i + 1 < app.arguments().size())
            theme = app.arguments()[i + 1];
    }
    app.setProperty("language", language);
    if (QTranslator* t = createAppTranslator(language)) {
        t->setParent(&app);
        app.installTranslator(t);
    }

    applyAppTheme(theme);

    if (app.arguments().contains(QStringLiteral("--shell-install"))) {
        QString err;
        const bool ok = wt::installShellIntegration(QCoreApplication::applicationFilePath(), err);
        qInfo() << "shell-install" << (ok ? "OK" : "FAILED") << err;
        return ok ? 0 : 1;
    }
    if (app.arguments().contains(QStringLiteral("--shell-uninstall"))) {
        QString err;
        const bool ok = wt::removeShellIntegration(&err);
        qInfo() << "shell-uninstall" << (ok ? "OK" : "FAILED") << err;
        return ok ? 0 : 1;
    }

    QString scanPath;
    QString shotPath;
    QString dumpPath;
    QString exportPath;
    QString exportFull;
    QString treemapPng;
    QString filterText;
    QString recyclePath;
    QString makeMft;
    QString scanMft;
    QString scanCsv;
    QString scanMulti;
    QString checkUrl;
    QString scanDriveLetter;
    QString checkSort;
    QString renamePath;
    QString renameName;
    bool copyInfo = false;
    int sortCol = -1;
    int mftCount = 1000;
    bool sortDesc = false;
    bool filterPath = false;
    int shotDelay = 2500;
    int tabIndex = -1;
    int dupMode = -1;
    bool dupOnly = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--scan") && i + 1 < args.size()) scanPath = args[++i];
        else if (args[i] == QLatin1String("--shot") && i + 1 < args.size()) shotPath = args[++i];
        else if (args[i] == QLatin1String("--delay") && i + 1 < args.size()) shotDelay = args[++i].toInt();
        else if (args[i] == QLatin1String("--dump") && i + 1 < args.size()) dumpPath = args[++i];
        else if (args[i] == QLatin1String("--tab") && i + 1 < args.size()) tabIndex = args[++i].toInt();
        else if (args[i] == QLatin1String("--dup") && i + 1 < args.size()) dupMode = args[++i].toInt();
        else if (args[i] == QLatin1String("--dup-only")) dupOnly = true;
        else if (args[i] == QLatin1String("--filter") && i + 1 < args.size()) filterText = args[++i];
        else if (args[i] == QLatin1String("--filter-path")) filterPath = true;
        else if (args[i] == QLatin1String("--make-mft") && i + 2 < args.size()) {
            makeMft = args[++i];
            mftCount = args[++i].toInt();
        }
        else if (args[i] == QLatin1String("--scan-mft") && i + 1 < args.size()) scanMft = args[++i];
        else if (args[i] == QLatin1String("--scan-csv") && i + 1 < args.size()) scanCsv = args[++i];
        else if (args[i] == QLatin1String("--scan-multi") && i + 1 < args.size()) scanMulti = args[++i];
        else if (args[i] == QLatin1String("--check-update") && i + 1 < args.size()) checkUrl = args[++i];
        else if (args[i] == QLatin1String("--scan-drive") && i + 1 < args.size()) scanDriveLetter = args[++i];
        else if (args[i] == QLatin1String("--check-sort") && i + 1 < args.size()) checkSort = args[++i];
        else if (args[i] == QLatin1String("--recycle") && i + 1 < args.size()) recyclePath = args[++i];
        else if (args[i] == QLatin1String("--rename") && i + 2 < args.size()) {
            renamePath = args[++i];
            renameName = args[++i];
        }
        else if (args[i] == QLatin1String("--copy-info")) copyInfo = true;
        else if (args[i] == QLatin1String("--sort") && i + 1 < args.size()) sortCol = args[++i].toInt();
        else if (args[i] == QLatin1String("--sort-desc")) sortDesc = true;
        else if (args[i] == QLatin1String("--export") && i + 1 < args.size()) exportPath = args[++i];
        else if (args[i] == QLatin1String("--export-full") && i + 1 < args.size()) exportFull = args[++i];
        else if (args[i] == QLatin1String("--treemap-png") && i + 1 < args.size()) treemapPng = args[++i];
    }

    if (args.contains(QStringLiteral("--selftest"))) {
        QString out;
        const bool ok = wt::selfTest(out);
        qInfo().noquote() << out;
        qInfo() << "selftest" << (ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

    if (!makeMft.isEmpty()) {
        QString err;
        const bool ok = wt::writeSyntheticMft(makeMft, mftCount, err);
        qInfo() << "make-mft" << (ok ? "OK" : "FAILED") << err;
        return ok ? 0 : 1;
    }
    if (!checkSort.isEmpty()) {
        wt::ScanOptions opt;
        opt.preferMft = false;
        wt::ScanResult r = wt::scanFolder(checkSort, opt);
        if (!r.ok) {
            qInfo() << "scan failed" << r.error;
            return 1;
        }
        uint64_t violations = 0;
        int shown = 0;
        for (uint32_t n = 0; n < r.tree.nodes.size(); ++n) {
            const wt::Node& nd = r.tree.nodes[n];
            for (uint32_t i = 1; i < nd.childCount; ++i) {
                const wt::Node& a = r.tree.nodes[r.tree.children[nd.childBegin + i - 1]];
                const wt::Node& b = r.tree.nodes[r.tree.children[nd.childBegin + i]];
                if (a.size < b.size) {
                    ++violations;
                    if (shown++ < 5)
                        qInfo() << "violation in" << r.tree.name(n) << ":" << a.size << "then"
                                << b.size;
                }
            }
        }
        qInfo() << "check-sort nodes=" << int(r.tree.nodes.size()) << "violations=" << violations;
        // print the root's children to eyeball ordering
        {
            const wt::Node& rootNode = r.tree.nodes[r.tree.root];
            for (uint32_t i = 0; i < rootNode.childCount && i < 12; ++i) {
                const uint32_t c = r.tree.children[rootNode.childBegin + i];
                qInfo() << "  " << r.tree.name(c) << qulonglong(r.tree.nodes[c].size);
            }
        }
        // print the largest folder's children to eyeball ordering
        for (uint32_t n = 0; n < r.tree.nodes.size(); ++n) {
            if (r.tree.name(n) == QLatin1String("Program Files")) {
                const wt::Node& nd = r.tree.nodes[n];
                for (uint32_t i = 0; i < nd.childCount && i < 12; ++i) {
                    const uint32_t c = r.tree.children[nd.childBegin + i];
                    qInfo() << "  " << r.tree.name(c) << qulonglong(r.tree.nodes[c].size);
                }
                break;
            }
        }
        return violations == 0 ? 0 : 1;
    }
    if (!scanDriveLetter.isEmpty()) {
        const QChar letter = scanDriveLetter.at(0).toUpper();
        wt::ScanOptions opt;
        opt.preferMft = true;
        wt::ScanResult r = wt::scanDrive(letter.unicode(), opt);
        const wt::Node& root = r.tree.nodes.empty() ? wt::Node{} : r.tree.nodes[r.tree.root];
        qInfo() << "scan-drive" << letter << "ok=" << r.ok << "mft=" << r.usesMft
                << "records=" << int(r.records) << "parsed=" << int(r.parsed)
                << "nodes=" << int(r.tree.nodes.size()) << "secs=" << r.seconds
                << "rootSize=" << qulonglong(root.size) << "rootAlloc=" << qulonglong(root.alloc)
                << "files=" << root.files << "folders=" << root.folders << "err=" << r.error
                << "noData=" << int(r.noDataFiles) << "zeroSize=" << int(r.zeroSizeFiles)
                << "sumData=" << qulonglong(r.sumDataSizes)
                << "sumFn=" << qulonglong(r.sumFileNameSizes);
        if (!r.tree.nodes.empty()) {
            const wt::Node& rn = r.tree.nodes[r.tree.root];
            for (uint32_t i = 0; i < rn.childCount && i < 12; ++i) {
                const uint32_t c = r.tree.children[rn.childBegin + i];
                qInfo() << "  " << r.tree.name(c) << r.tree.nodes[c].size << r.tree.nodes[c].files
                        << r.tree.nodes[c].folders;
            }
        }
        return r.ok ? 0 : 1;
    }
    if (!checkUrl.isEmpty()) {
        const wt::UpdateInfo info = wt::checkForUpdates(checkUrl);
        qInfo() << "update-check ok=" << info.ok << "version=" << info.version
                << "infourl=" << info.infoUrl << "err=" << info.error;
        if (info.ok)
            qInfo() << "newer=" << wt::isNewerVersion(info.version,
                                                     QCoreApplication::applicationVersion())
                    << "current=" << QCoreApplication::applicationVersion();
        return info.ok ? 0 : 1;
    }
    if (!scanMulti.isEmpty()) {
        wt::ScanOptions opt;
        std::vector<wt::Tree> parts;
        for (const QString& t : scanMulti.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const bool isDrive = t.size() == 3 && t.at(1) == QLatin1Char(':');
            wt::ScanResult r = isDrive ? wt::scanDrive(t.at(0).unicode(), opt) : wt::scanFolder(t, opt);
            if (r.ok) parts.push_back(std::move(r.tree));
        }
        wt::Tree merged = wt::mergeTrees(parts, QStringLiteral("Multiple selections"));
        const wt::Node& root = merged.nodes[merged.root];
        qInfo() << "scan-multi parts=" << int(parts.size()) << "nodes=" << int(merged.nodes.size())
                << "size" << qulonglong(root.size) << "files" << root.files << "folders"
                << root.folders;
        for (uint32_t i = 0; i < merged.nodes.size() && i < 8; ++i)
            qInfo() << i << merged.name(i) << "size" << qulonglong(merged.nodes[i].size) << "dir"
                    << int(merged.nodes[i].isDir);
        return 0;
    }
    if (!scanCsv.isEmpty()) {
        wt::ScanOptions opt;
        wt::ScanResult r = wt::importCsvTree(scanCsv, opt);
        qInfo() << "scan-csv ok=" << r.ok << "nodes=" << int(r.tree.nodes.size())
                << "rows=" << int(r.records) << "err=" << r.error;
        if (r.ok) {
            const wt::Node& root = r.tree.nodes[r.tree.root];
            qInfo() << "root" << r.tree.name(r.tree.root) << "size" << qulonglong(root.size)
                    << "files" << root.files << "folders" << root.folders;
            for (uint32_t i = 0; i < r.tree.nodes.size() && i < 12; ++i) {
                const wt::Node& nd = r.tree.nodes[i];
                qInfo() << i << r.tree.name(i) << "size" << qulonglong(nd.size) << "dir"
                        << int(nd.isDir) << "parent" << int(nd.parent) << "children"
                        << nd.childCount;
            }
        }
        return r.ok ? 0 : 1;
    }
    if (!scanMft.isEmpty()) {
        wt::ScanOptions opt;
        wt::ScanResult r = wt::scanMftDump(scanMft, opt);
        qInfo() << "scan-mft ok=" << r.ok << "nodes=" << int(r.tree.nodes.size())
                << "records=" << int(r.records) << "err=" << r.error
                << "noData=" << int(r.noDataFiles) << "zeroSize=" << int(r.zeroSizeFiles)
                << "sumData=" << qulonglong(r.sumDataSizes)
                << "sumFn=" << qulonglong(r.sumFileNameSizes);
        const int n = int(qMin<size_t>(r.tree.nodes.size(), 10));
        for (int i = 0; i < n; ++i) {
            const wt::Node& nd = r.tree.nodes[uint32_t(i)];
            qInfo() << i << r.tree.name(uint32_t(i)) << "size" << qulonglong(nd.size)
                    << "dir" << int(nd.isDir) << "children" << nd.childCount;
        }
        return r.ok ? 0 : 1;
    }

    if (!renamePath.isEmpty()) {
        QString outPath;
        QString err;
        const bool ok = wt::renameFileOrFolder(renamePath, renameName, outPath, err);
        qInfo() << "rename" << (ok ? "OK" : "FAILED") << outPath << err;
        return ok ? 0 : 1;
    }

    if (!recyclePath.isEmpty()) {
        const bool ok = wt::moveToRecycleBin({recyclePath});
        qInfo() << "recycle" << recyclePath << (ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }

    if (!dumpPath.isEmpty()) {
        wt::ScanOptions opt;
        opt.preferMft = false;
        wt::ScanResult r = wt::scanFolder(dumpPath, opt);
        qInfo() << "ok=" << r.ok << "nodes=" << int(r.tree.nodes.size()) << "secs=" << r.seconds;
        const int n = int(qMin<size_t>(r.tree.nodes.size(), 12));
        for (int i = 0; i < n; ++i) {
            const wt::Node& nd = r.tree.nodes[uint32_t(i)];
            qInfo() << i << r.tree.name(uint32_t(i)) << "nameOff" << nd.nameOff << "nameLen"
                    << nd.nameLen << "size" << qulonglong(nd.size) << "children" << nd.childCount;
        }
        return 0;
    }

    MainWindow w;
    if (!shotPath.isEmpty() || !exportPath.isEmpty() || !exportFull.isEmpty() ||
        !treemapPng.isEmpty() || !filterText.isEmpty() || copyInfo || sortCol >= 0)
        w.setBatchMode(true);
    w.show();

    if (copyInfo || sortCol >= 0) {
        w.setScanDoneCallback([&w, copyInfo, sortCol, sortDesc] {
            if (copyInfo) w.copyTreeInfoToClipboard();
            if (sortCol >= 0) w.sortFiles(sortCol, sortDesc);
        });
    }
    if (!exportPath.isEmpty() || !exportFull.isEmpty() || !treemapPng.isEmpty() || !filterText.isEmpty()) {
        w.setScanDoneCallback([&w, exportPath, exportFull, treemapPng, filterText, filterPath] {
            if (!exportPath.isEmpty()) w.exportCsvTo(exportPath);
            if (!exportFull.isEmpty()) w.exportCsvFullTo(exportFull);
            if (!treemapPng.isEmpty())
                w.exportTreemapTo(treemapPng, QSize(1920, 1080), false, false);
            if (!filterText.isEmpty()) w.applyFilterFromCli(filterText, filterPath);
        });
    }
    if (!scanPath.isEmpty())
        QTimer::singleShot(300, &w, [&w, scanPath] { w.scanPathFromCli(scanPath); });
    if (dupMode >= 0)
        QTimer::singleShot(400, &w, [&w, dupMode, dupOnly] { w.configureDuplicates(dupMode, dupOnly); });
    if (tabIndex >= 0)
        QTimer::singleShot(shotDelay > 1200 ? shotDelay - 800 : 400, &w,
                           [&w, tabIndex] { w.setTabIndex(tabIndex); });

    if (!shotPath.isEmpty()) {
        QTimer::singleShot(shotDelay, &w, [&w, shotPath] {
            w.grab().save(shotPath);
            w.close();
        });
    }
    return app.exec();
}














