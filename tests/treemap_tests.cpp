#include <QtTest>
#include <windows.h>
#include <QFile>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QTreeView>
#include <QTableView>
#include <QMenu>
#include <QClipboard>
#include <QPushButton>
#include <QToolButton>
#include <QSpinBox>
#include <QLabel>
#include <QComboBox>
#include <QHeaderView>
#include <QMimeData>
#include <QScopeGuard>
#include <QScreen>
#include <QAbstractItemModelTester>
#include "deletionwatcher.h"
#include "core.h"
#include "mainwindow.h"
#include "models.h"
#include "treemap.h"

namespace {
uint32_t add(wt::Tree& tree, const QString& name, uint32_t parent, bool directory,
             uint64_t size = 0, uint64_t allocated = 0)
{
    wt::Node node;
    node.parent = parent;
    node.isDir = directory;
    node.size = size;
    node.alloc = allocated;
    node.nameOff = uint32_t(tree.names.size());
    node.nameLen = uint32_t(name.size());
    const auto* begin = reinterpret_cast<const char16_t*>(name.utf16());
    tree.names.insert(tree.names.end(), begin, begin + name.size());
    tree.nodes.push_back(node);
    return uint32_t(tree.nodes.size() - 1);
}

wt::Tree fixture()
{
    wt::Tree tree;
    tree.root = add(tree, QStringLiteral("C:\\fixture"), wt::kNone, true);
    add(tree, QStringLiteral("media"), 0, true);                  // 1
    add(tree, QStringLiteral("archives"), 0, true);               // 2
    add(tree, QStringLiteral("movie.mp4"), 1, false, 600, 1200);  // 3
    add(tree, QStringLiteral("nested"), 1, true);                 // 4
    add(tree, QStringLiteral("song.mp3"), 4, false, 300, 600);    // 5
    add(tree, QStringLiteral("pack.zip"), 2, false, 100, 200);    // 6
    add(tree, QStringLiteral("empty.txt"), 0, false);             // 7
    tree.finalize();
    return tree;
}
}

class TreemapTests : public QObject {
    Q_OBJECT
    static QRectF rectFor(const TreemapWidget& widget, uint32_t node, bool label = false)
    {
        for (const auto& cell : widget.cells_)
            if (cell.node == node) return label ? cell.labelRect : cell.rect;
        return {};
    }
    static double area(const QRectF& rect) { return rect.width() * rect.height(); }

private slots:
    void incrementalInsertRenameAndResize()
    {
        auto tree = fixture();
        TreeModel model;
        model.setTree(&tree);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        ExtensionModel types;
        types.rebuild(&tree, tree.root, wt::UnitMode::Bytes);
        FileListModel files;
        files.setTree(&tree);
        files.rebuild("txt", false, false, 100, false);
        wt::LiveUpdate update;
        wt::LiveEntry entry;
        entry.state = wt::LiveEntry::Present;
        entry.path = "C:/fixture/new.txt"; entry.name = "new.txt";
        entry.metadata.size = 200; entry.metadata.alloc = 4096;
        update.entries.push_back(entry);
        entry.path = "C:/fixture/media/movie.mp4"; entry.name = "movie.mp4";
        entry.metadata.size = 800; entry.metadata.alloc = 4096;
        update.entries.push_back(entry);
        update.renames.append(qMakePair(QString("C:/fixture/media/nested"), QString("C:/fixture/archives/renamed")));
        update.renames.append(qMakePair(QString("C:/fixture/archives/pack.zip"), QString("C:/fixture/archives/pack.txt")));
        auto delta = model.updateLive(update, 2, Qt::DescendingOrder);
        types.applyUpdate(&tree, tree.root, delta, wt::UnitMode::Bytes);
        files.refreshAfterUpdate(delta);
        QCOMPARE(tree.nodes[0].size, uint64_t(1400));
        QCOMPARE(tree.nodes[0].files, uint32_t(5));
        QCOMPARE(tree.nodes[0].folders, uint32_t(3));
        QCOMPARE(tree.nodes[1].size, uint64_t(800));
        QCOMPARE(tree.nodes[2].size, uint64_t(400));
        QCOMPARE(tree.findPaths({"C:/fixture/archives/renamed/song.mp3"}).front(), uint32_t(5));
        QCOMPARE(files.rowCount(), 3); // an old nonmatching row becomes eligible after rename
        ExtensionModel reference;
        reference.rebuild(&tree, tree.root, wt::UnitMode::Bytes);
        QCOMPARE(types.rowCount(), reference.rowCount());
        for (int row = 0; row < types.rowCount(); ++row) {
            int referenceRow = -1;
            for (int other = 0; other < reference.rowCount(); ++other)
                if (reference.extensionAt(other) == types.extensionAt(row)) referenceRow = other;
            QVERIFY(referenceRow >= 0);
            for (int col = 0; col < 6; ++col)
                QCOMPARE(types.data(types.index(row, col), Qt::DisplayRole), reference.data(reference.index(referenceRow, col), Qt::DisplayRole));
        }
        update = {};
        update.renames = {{"C:/fixture/archives/renamed", "C:/fixture/media/nested"},
                          {"C:/fixture/media/nested", "C:/fixture/archives/renamed"},
                          {"C:/fixture/archives/renamed", "C:/fixture/media/nested"}};
        delta = model.updateLive(update, 2, Qt::DescendingOrder);
        QCOMPARE(tree.nodes[1].childCount, uint32_t(2)); // no duplicate child references after A->B->A
        QCOMPARE(tree.nodes[2].childCount, uint32_t(1));
        QCOMPARE(tree.nodes[0].size, uint64_t(1400));
    }

    void overlappingRootsRename()
    {
        auto first = fixture(), second = fixture();
        const QString name = "C:\\fixture\\media";
        second.nodes[0].nameOff = uint32_t(second.names.size());
        second.nodes[0].nameLen = uint32_t(name.size());
        const auto* chars = reinterpret_cast<const char16_t*>(name.utf16());
        second.names.insert(second.names.end(), chars, chars + name.size());
        std::vector<wt::Tree> parts;
        parts.push_back(std::move(first)); parts.push_back(std::move(second));
        auto tree = wt::mergeTrees(parts, "Multiple selections");
        wt::LiveUpdate update;
        update.renames.append(qMakePair(QString("C:/fixture"), QString("C:/renamed")));
        wt::applyLiveUpdate(tree, update);
        QCOMPARE(tree.nodes[tree.root].size, uint64_t(2000));
        QVERIFY(!tree.findPaths({"C:/renamed/media/media/movie.mp4"}).empty());
        QVERIFY(tree.findPaths({"C:/fixture/media/media/movie.mp4"}).empty());
    }

    void largeIncrementalBatchCost()
    {
        wt::Tree tree;
        tree.root = add(tree, "C:\\wide", wt::kNone, true);
        for (int i = 0; i < 100000; ++i) add(tree, QString::number(i), 0, false, 100, 4096);
        tree.finalize();
        wt::LiveUpdate update;
        for (int i = 0; i < 10000; ++i) {
            wt::LiveEntry entry;
            entry.state = wt::LiveEntry::Present;
            entry.name = QString::number(i);
            entry.path = "C:/wide/" + entry.name;
            entry.metadata.size = 200; entry.metadata.alloc = 4096;
            update.entries.push_back(entry);
            entry.name = "new" + QString::number(i);
            entry.path = "C:/wide/" + entry.name;
            update.entries.push_back(entry);
        }
        QElapsedTimer timer; timer.start();
        const auto delta = wt::applyLiveUpdate(tree, update);
        qInfo() << "100k siblings / 10k inserts + 10k resizes:" << timer.elapsed() << "ms";
        QCOMPARE(tree.nodes[0].size, uint64_t(13000000));
        QCOMPARE(tree.nodes[0].files, uint32_t(110000));
        QCOMPARE(tree.nodes[0].childCount, uint32_t(110000));
        QCOMPARE(delta.files.size(), qsizetype(20000));
        QVERIFY(tree.children.size() < 400000); // capacity growth remains bounded
        QVERIFY(wt::applyLiveUpdate(tree, update).changed.isEmpty()); // repeated notifications are idempotent
        update.entries.resize(1);
        timer.restart();
        for (int i = 0; i < 100; ++i) {
            update.entries[0].metadata.size = 300 + i;
            wt::applyLiveUpdate(tree, update);
        }
        qInfo() << "100 repeated single-file updates with cached sibling lookup:" << timer.elapsed() << "ms";
    }

    void fullLiveFilesystemIntegration()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path() + "/settings");
        {
            QSettings settings(QSettings::IniFormat, QSettings::UserScope, "DiskCleaner", "DiskCleaner");
            settings.setValue("edFilterInclude_Text", "txt");
            settings.setValue("edFilterExclude_Text", "hidden");
        }
        QString root = temp.path() + "/data";
        QVERIFY(QDir().mkpath(root + "/original"));
        const auto write = [](const QString& path, int size) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(QByteArray(size, 'x')) == size && file.flush();
        };
        QVERIFY(write(root + "/seed.bin", 100));
        MainWindow window;
        window.setBatchMode(true); window.show();
        QVERIFY(window.findChild<QLineEdit*>("filterInclude")->text().isEmpty());
        QVERIFY(window.findChild<QLineEdit*>("filterExclude")->text().isEmpty());
        bool done = false, unexpectedRescan = false;
        window.setScanDoneCallback([&] { done = true; });
        window.scanPathFromCli(root);
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        window.setScanDoneCallback([&] { unexpectedRescan = true; });
        auto* view = window.findChild<QTreeView*>("treeView");
        auto* model = qobject_cast<TreeModel*>(view->model());
        auto* tree = model->tree();
        auto* fileModel = qobject_cast<FileListModel*>(window.findChild<QTableView*>("fileView")->model());
        auto* widget = window.findChild<TreemapWidget*>("treemap");
        const auto total = [&] { return tree->nodes[tree->root].size; };
        const auto idFor = [&](const QString& path) {
            const auto ids = tree->findPaths({path}); return ids.empty() ? wt::kNone : ids.front();
        };
        const auto seed = idFor(root + "/seed.bin");
        QVERIFY(write(root + "/added.txt", 200));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(300), 10000);
        QCOMPARE(fileModel->rowCount(), 2);
        QVERIFY(QDir().mkpath(root + "/fresh/deep"));
        QVERIFY(write(root + "/fresh/deep/child.txt", 300));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(600), 10000);
        const auto fresh = idFor(root + "/fresh");
        const auto child = idFor(root + "/fresh/deep/child.txt");
        QVERIFY(fresh != wt::kNone && child != wt::kNone);
        widget->setRoot(fresh);
        widget->toggleTag(child, 2);
        QVERIFY(QDir().rename(root + "/fresh", root + "/renamed"));
        QTRY_COMPARE_WITH_TIMEOUT(idFor(root + "/renamed/deep/child.txt"), child, 10000);
        QCOMPARE(widget->root(), fresh);
        QCOMPARE(widget->tags_.value(QDir::toNativeSeparators(root + "/renamed/deep/child.txt").toCaseFolded()), 2);
        QVERIFY(QFile::rename(root + "/seed.bin", root + "/seed.txt"));
        QTRY_COMPARE_WITH_TIMEOUT(idFor(root + "/seed.txt"), seed, 10000);
        QVERIFY(QFile::rename(root + "/seed.txt", root + "/SEED.txt"));
        QTRY_COMPARE_WITH_TIMEOUT(tree->name(seed), QStringLiteral("SEED.txt"), 10000);
        for (int i = 1; i <= 10; ++i) QVERIFY(write(root + "/SEED.txt", 100 + 50 * i));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(1100), 10000);
        QVERIFY(write(root + "/SEED.txt", 0));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(500), 10000);
        QVERIFY(QDir().rename(root + "/renamed", root + "/original/moved"));
        QTRY_VERIFY_WITH_TIMEOUT(idFor(root + "/original/moved/deep/child.txt") != wt::kNone, 10000);
        QCOMPARE(total(), uint64_t(500));
        QVERIFY(write(root + "/original/moved/deep/child.txt", 400));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(600), 10000);
        QVERIFY(QDir().mkpath(temp.path() + "/outside/sub"));
        QVERIFY(write(temp.path() + "/outside/sub/import.bin", 1000));
        QVERIFY(QDir().rename(temp.path() + "/outside", root + "/imported"));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(1600), 10000);
        QVERIFY(QDir().rename(root + "/imported", temp.path() + "/exported"));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(600), 10000);
        window.applyFilterFromCli("txt", false);
        QVERIFY(write(root + "/new.txt", 80));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(680), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(model->tree()->nodes[model->tree()->root].size, uint64_t(680), 10000);
        QVERIFY(QFile::rename(root + "/new.txt", root + "/new.bin"));
        QTRY_COMPARE_WITH_TIMEOUT(model->tree()->nodes[model->tree()->root].size, uint64_t(600), 10000);
        QVERIFY(write(root + "/added.txt", 250));
        QTRY_COMPARE_WITH_TIMEOUT(model->tree()->nodes[model->tree()->root].size, uint64_t(650), 10000);
        window.applyFilterFromCli({}, false);
        QCOMPARE(model->tree(), tree);
        QCOMPARE(total(), uint64_t(730));
        const auto newRoot = temp.path() + "/renamed-root";
        QVERIFY(QDir().rename(root, newRoot));
        root = newRoot;
        QTRY_VERIFY_WITH_TIMEOUT(idFor(root + "/SEED.txt") != wt::kNone, 10000);
        QVERIFY(write(root + "/after-root-rename.txt", 70));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(800), 10000);
        const auto lockedPath = QDir::toNativeSeparators(root + "/SEED.txt");
        HANDLE locked = CreateFileW(reinterpret_cast<LPCWSTR>(lockedPath.utf16()), GENERIC_WRITE,
                                    0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(locked != INVALID_HANDLE_VALUE);
        const auto closeLocked = qScopeGuard([&] { if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked); });
        DWORD written = 0;
        const QByteArray lockedData(100, 'x');
        QVERIFY(WriteFile(locked, lockedData.constData(), DWORD(lockedData.size()), &written, nullptr));
        QVERIFY(FlushFileBuffers(locked));
        QTest::qWait(650); // metadata may remain readable despite an exclusive data handle
        QVERIFY(total() == 800 || total() == 900);
        QVERIFY(idFor(root + "/SEED.txt") != wt::kNone);
        CloseHandle(locked); locked = INVALID_HANDLE_VALUE;
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(900), 10000);
        wt::ScanOptions options;
        const auto reference = wt::scanFolder(root, options);
        QVERIFY(reference.ok);
        QCOMPARE(total(), reference.tree.nodes[reference.tree.root].size);
        QCOMPARE(tree->nodes[tree->root].files, reference.tree.nodes[reference.tree.root].files);
        QCOMPARE(tree->nodes[tree->root].folders, reference.tree.nodes[reference.tree.root].folders);
        for (uint32_t id = 0; id < tree->nodes.size(); ++id) {
            if (tree->nodes[id].removed) continue;
            const auto matches = reference.tree.findPaths({tree->path(id)});
            QVERIFY2(!matches.empty(), qPrintable(tree->path(id)));
            QCOMPARE(tree->nodes[id].size, reference.tree.nodes[matches.front()].size);
        }
        QVERIFY(!unexpectedRescan);
        QVERIFY(QDir(root).removeRecursively());
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(0), 10000);
        QVERIFY(QDir().mkpath(root));
        QVERIFY(write(root + "/reborn.txt", 40));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(40), 10000);
        QVERIFY(write(root + "/later.txt", 60));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(100), 10000);
        // Delete and recreate without waiting for a notification batch in between.
        QVERIFY(QDir(root).removeRecursively());
        QVERIFY(QDir().mkpath(root));
        QVERIFY(write(root + "/replacement.txt", 90));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(90), 10000);
        QCOMPARE(tree->nodes[tree->root].files, uint32_t(1));
        QVERIFY(write(root + "/after-replacement.txt", 10));
        QTRY_COMPARE_WITH_TIMEOUT(total(), uint64_t(100), 10000);
        QVERIFY(!unexpectedRescan);
        window.close();
    }

    void liveMultipleRootsDuplicatesAndExclude()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path() + "/settings");
        const auto a = temp.path() + "/a", b = temp.path() + "/b";
        QVERIFY(QDir().mkpath(a)); QVERIFY(QDir().mkpath(b));
        const auto write = [](const QString& path, int size) {
            QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(QByteArray(size, 'x')) == size;
        };
        QVERIFY(write(a + "/same.txt", 100));
        MainWindow window;
        window.setBatchMode(true); window.show();
        bool done = false;
        window.setScanDoneCallback([&] { done = true; });
        window.startMultiScan({a, b});
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        auto* model = qobject_cast<TreeModel*>(window.findChild<QTreeView*>("treeView")->model());
        auto* source = model->tree();
        auto* files = qobject_cast<FileListModel*>(window.findChild<QTableView*>("fileView")->model());
        window.configureDuplicates(1, true);
        QTRY_COMPARE(files->rowCount(), 0);
        QVERIFY(write(b + "/same.txt", 100));
        QTRY_COMPARE_WITH_TIMEOUT(files->rowCount(), 2, 10000);
        QVERIFY(write(b + "/same.txt", 200));
        QTRY_COMPARE_WITH_TIMEOUT(files->rowCount(), 0, 10000);
        QCOMPARE(source->nodes[source->root].size, uint64_t(300));
        window.configureDuplicates(0, false);
        window.findChild<QLineEdit*>("filterExclude")->setText("skip");
        window.applyFilterFromCli("txt", false);
        QVERIFY(write(a + "/skip.txt", 50));
        QTRY_COMPARE_WITH_TIMEOUT(source->nodes[source->root].size, uint64_t(350), 10000);
        QCOMPARE(model->tree()->nodes[model->tree()->root].size, uint64_t(300));
        // Editing an input without Apply must not change the active filter on a live event.
        window.findChild<QLineEdit*>("filterExclude")->clear();
        QVERIFY(write(a + "/skip2.txt", 50));
        QTRY_COMPARE_WITH_TIMEOUT(source->nodes[source->root].size, uint64_t(400), 10000);
        QCOMPARE(model->tree()->nodes[model->tree()->root].size, uint64_t(300));
        QVERIFY(QFile::rename(a + "/skip.txt", a + "/visible.txt"));
        QTRY_COMPARE_WITH_TIMEOUT(model->tree()->nodes[model->tree()->root].size, uint64_t(350), 10000);
        window.applyFilterFromCli({}, false);
        QVERIFY(window.findChild<QLineEdit*>("filterInclude")->text().isEmpty());
        QVERIFY(window.findChild<QLineEdit*>("filterExclude")->text().isEmpty());
        QCOMPARE(model->tree()->nodes[model->tree()->root].size, uint64_t(400));
        window.close();
    }

    void batchRemovalAndStableModels()
    {
        auto tree = fixture();
        TreeModel model;
        model.setTree(&tree);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        FileListModel files;
        files.setTree(&tree);
        files.rebuild({}, false, false, 2, false);
        ExtensionModel extensions;
        extensions.rebuild(&tree, tree.root, wt::UnitMode::Bytes);
        TreemapWidget widget;
        widget.setTree(&tree, tree.root);
        widget.setRoot(1);
        widget.setRoot(4);
        widget.selectNode(5);
        auto ids = tree.findPaths({"c:/FIXTURE/media", "C:/fixture/media/nested/song.mp3", "C:/fixture/media"});
        QCOMPARE(ids.size(), size_t(3));
        const auto removal = model.removeNodes(ids, 2, Qt::DescendingOrder);
        QCOMPARE(removal.nodes.size(), size_t(4));
        QCOMPARE(tree.nodes[0].size, uint64_t(100));
        QCOMPARE(tree.nodes[0].alloc, uint64_t(200));
        QCOMPARE(tree.nodes[0].files, uint32_t(2));
        QCOMPARE(tree.nodes[0].folders, uint32_t(1));
        QCOMPARE(tree.nodes[0].childCount, uint32_t(2));
        QVERIFY(tree.removeNodes(ids).nodes.empty());
        files.refreshAfterRemoval();
        QCOMPARE(files.rowCount(), 2); // refills the capped table with surviving rows
        QCOMPARE(files.totalFiles(), 2);
        for (int row = 0; row < files.rowCount(); ++row) QVERIFY(!tree.nodes[files.nodeAt(row)].removed);
        extensions.applyRemoval(&tree, tree.root, removal, wt::UnitMode::Bytes);
        QCOMPARE(extensions.rowCount(), 2);
        widget.refreshAfterRemoval(removal, 200, 0, 0);
        QCOMPARE(widget.root(), tree.root);
        QCOMPARE(widget.selectedNode(), tree.root);
        QVERIFY(!widget.canGoBack());
        auto filtered = wt::filterTree(tree, "mp3", {}, false);
        QCOMPARE(filtered.nodes[filtered.root].files, uint32_t(0));
        QVERIFY(tree.findPaths({"C:/fixture/media/nested/song.mp3"}).empty());
        const auto rootRemoval = model.removeNodes({tree.root}, 2, Qt::DescendingOrder);
        files.refreshAfterRemoval();
        extensions.applyRemoval(&tree, tree.root, rootRemoval, wt::UnitMode::Bytes);
        QCOMPARE(files.rowCount(), 0);
        QCOMPARE(extensions.rowCount(), 0);
        QCOMPARE(tree.nodes[0].size, uint64_t(0));
    }

    void wideDirectoryDeletionCost()
    {
        wt::Tree tree;
        tree.root = add(tree, "C:\\wide", wt::kNone, true);
        for (int i = 0; i < 100000; ++i) add(tree, QString::number(i), 0, false, 100, 4096);
        tree.finalize();
        QStringList paths;
        for (int i = 0; i < 10000; ++i) paths.append("C:/wide/" + QString::number(i));
        QElapsedTimer timer;
        timer.start();
        const auto removed = tree.removeNodes(tree.findPaths(paths));
        qInfo() << "100k siblings / 10k deletions, lookup + update:" << timer.elapsed() << "ms";
        QCOMPARE(removed.nodes.size(), size_t(10000));
        QCOMPARE(tree.nodes[0].files, uint32_t(90000));
        QCOMPARE(tree.nodes[0].size, uint64_t(9000000));
        QCOMPARE(tree.nodes[0].childCount, uint32_t(90000));
        for (uint32_t row = 0; row < tree.nodes[0].childCount; ++row)
            QCOMPARE(tree.nodes[tree.children[row]].rowInParent, row);
    }

    void mergedRootsAndDuplicateRemoval()
    {
        auto first = fixture();
        auto second = fixture();
        const auto rootName = QStringLiteral("D:\\other");
        second.nodes[0].nameOff = uint32_t(second.names.size());
        second.nodes[0].nameLen = uint32_t(rootName.size());
        const auto* chars = reinterpret_cast<const char16_t*>(rootName.utf16());
        second.names.insert(second.names.end(), chars, chars + rootName.size());
        std::vector<wt::Tree> parts;
        parts.push_back(std::move(first));
        parts.push_back(std::move(second));
        auto tree = wt::mergeTrees(parts, "Multiple selections");
        const auto ids = tree.findPaths({"C:/fixture/media/movie.mp4", "d:/OTHER/media/movie.mp4"});
        QCOMPARE(ids.size(), size_t(2));
        QCOMPARE(tree.path(ids[0]), QStringLiteral("C:\\fixture\\media\\movie.mp4"));
        QCOMPARE(tree.path(ids[1]), QStringLiteral("D:\\other\\media\\movie.mp4"));
        FileListModel files;
        files.setTree(&tree);
        files.setDupMode(1);
        files.rebuild({}, false, false, 0, true);
        QCOMPARE(files.rowCount(), 8);
        tree.removeNodes({ids[0]});
        files.refreshAfterRemoval();
        QCOMPARE(files.rowCount(), 6); // remaining movie is no longer a duplicate
        QCOMPARE(tree.nodes[tree.root].size, uint64_t(1400));
    }

    void nativeDeletionNotifications()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto root = temp.path() + "/watched";
        QVERIFY(QDir().mkpath(root + "/nested"));
        for (int i = 0; i < 100; ++i) {
            QFile file(root + "/nested/" + QString::number(i));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("test");
        }
        DeletionWatcher watcher;
        QSignalSpy removed(&watcher, &DeletionWatcher::pathsRemoved);
        QSignalSpy errors(&watcher, &DeletionWatcher::watchError);
        watcher.start({root});
        QCOMPARE(watcher.subscriptionCount(), 2);
        QCOMPARE(errors.size(), 0);
        for (int i = 0; i < 100; ++i) QVERIFY(QFile::remove(root + "/nested/" + QString::number(i)));
        QTRY_VERIFY_WITH_TIMEOUT(!removed.isEmpty(), 5000);
        QSet<QString> paths;
        auto collect = [&] {
            for (const auto& args : removed)
                for (const auto& path : args[0].toStringList()) paths.insert(path);
            return paths.size();
        };
        QTRY_COMPARE_WITH_TIMEOUT(collect(), qsizetype(100), 5000);
        QVERIFY(removed.size() < 10); // notifications coalesce, rather than 100 UI refreshes
        removed.clear();
        QVERIFY(QDir(root + "/nested").removeRecursively());
        QVERIFY(QDir().rmdir(root));
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const auto& args : removed)
                if (args[0].toStringList().contains(QDir::toNativeSeparators(root).toCaseFolded())) return true;
            return false;
        }(), 5000);
        watcher.stop();
        QCOMPARE(watcher.subscriptionCount(), 0);
    }

    void liveDeletionAndMenuPosition()
    {
        auto savedClipboard = std::make_unique<QMimeData>();
        if (const auto* mime = QApplication::clipboard()->mimeData())
            for (const auto& format : mime->formats()) savedClipboard->setData(format, mime->data(format));
        const auto restoreClipboard = qScopeGuard([&] {
            QApplication::clipboard()->setMimeData(savedClipboard.release());
        });
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path() + "/settings");
        const auto root = temp.path() + "/data";
        QVERIFY(QDir().mkpath(root + "/media/nested"));
        auto write = [](const QString& path, int size) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(QByteArray(size, 'x')) == size;
        };
        QVERIFY(write(root + "/keep.bin", 100));
        QVERIFY(write(root + "/media/movie.mp4", 600));
        QVERIFY(write(root + "/media/nested/song.mp3", 300));
        MainWindow window;
        window.setBatchMode(true);
        window.resize(850, 600);
        window.move(45, 35);
        window.show();
        bool done = false;
        window.setScanDoneCallback([&] { done = true; });
        window.scanPathFromCli(root);
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        auto* view = window.findChild<QTreeView*>("treeView");
        auto* model = qobject_cast<TreeModel*>(view->model());
        auto* files = window.findChild<QTableView*>("fileView");
        auto* widget = window.findChild<TreemapWidget*>("treemap");
        auto* tree = model->tree();
        QCOMPARE(tree->nodes[tree->root].size, uint64_t(1000));
        const auto media = tree->findPaths({root + "/media"}).front();
        view->expand(model->indexForNode(media));

        // Run real modal menus on both views. A moved window exposes double global mapping.
        for (int tab = 0; tab < 2; ++tab) {
            window.setTabIndex(tab);
            QCoreApplication::processEvents();
            QAbstractItemView* itemView = tab == 0 ? static_cast<QAbstractItemView*>(view) : files;
            const auto index = itemView->model()->index(0, 0);
            const QPoint local = itemView->visualRect(index).topLeft() + QPoint(16, 9);
            const QPoint expected = itemView->viewport()->mapToGlobal(local);
            bool inspected = false, positioned = false;
            QTimer::singleShot(80, &window, [&] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!menu) return;
                inspected = true;
                const QRect screen = menu->screen()->availableGeometry();
                const QPoint clamped(std::clamp(expected.x(), screen.left(), std::max(screen.left(), screen.right() - menu->width() + 1)),
                                     std::clamp(expected.y(), screen.top(), std::max(screen.top(), screen.bottom() - menu->height() + 1)));
                positioned = (menu->pos() - clamped).manhattanLength() <= 6;
                for (auto* action : menu->actions()) {
                    if (action->text() == QLatin1String("Copy Path")) {
                        menu->setActiveAction(action);
                        QTest::keyClick(menu, Qt::Key_Return);
                        return;
                    }
                }
                menu->close();
            });
            QVERIFY(QMetaObject::invokeMethod(&window, tab == 0 ? "showTreeContextMenu" : "showFileContextMenu",
                                              Qt::DirectConnection, Q_ARG(QPoint, local)));
            QVERIFY(inspected);
            QVERIFY(positioned);
            const auto expectedNode = tab == 0 ? tree->root
                : qobject_cast<FileListModel*>(files->model())->nodeAt(0);
            QCOMPARE(QApplication::clipboard()->text(), tree->path(expectedNode));
        }
        window.setTabIndex(0);
        view->setCurrentIndex(model->indexForNode(media));
        QVERIFY(QFile::remove(root + "/media/movie.mp4"));
        QTRY_COMPARE_WITH_TIMEOUT(tree->nodes[tree->root].size, uint64_t(400), 5000);
        QCOMPARE(widget->root(), media);
        QVERIFY(view->isExpanded(model->indexForNode(media)));
        QCOMPARE(files->model()->rowCount(), 2);
        window.applyFilterFromCli("mp3", false);
        auto* filtered = model->tree();
        QCOMPARE(filtered->nodes[filtered->root].size, uint64_t(300));
        QVERIFY(QDir(root + "/media").removeRecursively());
        QTRY_COMPARE_WITH_TIMEOUT(tree->nodes[tree->root].size, uint64_t(100), 5000);
        QCOMPARE(model->tree()->nodes[model->tree()->root].size, uint64_t(0));
        QCOMPARE(files->model()->rowCount(), 0);
        window.exportCsvTo(temp.path() + "/after.csv");
        QFile csv(temp.path() + "/after.csv");
        QVERIFY(csv.open(QIODevice::ReadOnly));
        const auto text = csv.readAll();
        QVERIFY(text.contains("keep.bin"));
        QVERIFY(!text.contains("movie.mp4"));
        QVERIFY(!text.contains("song.mp3"));

        // Exercise the overflow recovery path: a rescan replaces the snapshot in background.
        QVERIFY(write(root + "/new.bin", 50));
        auto* watcher = window.findChild<DeletionWatcher*>();
        QVERIFY(watcher);
        done = false;
        window.setScanDoneCallback([&] { done = true; });
        QVERIFY(QMetaObject::invokeMethod(watcher, "rescanRequired", Qt::DirectConnection));
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        window.applyFilterFromCli({}, false);
        QCOMPARE(model->tree()->nodes[model->tree()->root].size, uint64_t(150));
        window.close();
        QCOMPARE(watcher->subscriptionCount(), 0);
    }

    void unifiedMetricsAndProportionalAreas()
    {
        wt::Tree tree;
        tree.root = add(tree, "C:\\metrics", wt::kNone, true);
        const auto folder = add(tree, "folder", 0, true);
        const auto tiny = add(tree, "tiny.txt", folder, false, 1, 100);
        const auto big = add(tree, "big.bin", 0, false, 99, 100);
        add(tree, "zero.txt", 0, false);
        tree.finalize();
        TreeModel directories;
        directories.setTree(&tree);
        FileListModel files;
        files.setTree(&tree);
        files.rebuild({}, false, false, 0, false);
        ExtensionModel types;
        types.rebuild(&tree, 0, wt::UnitMode::Bytes);
        const auto tinyPercent = [&] {
            return directories.data(directories.indexForNode(tiny).siblingAtColumn(1), Qt::DisplayRole).toString();
        };
        const auto filePercent = [&](uint32_t node) {
            for (int row = 0; row < files.rowCount(); ++row)
                if (files.nodeAt(row) == node) return files.data(files.index(row, 4), Qt::DisplayRole).toString();
            return QString();
        };
        const auto typePercent = [&] {
            for (int row = 0; row < types.rowCount(); ++row)
                if (types.extensionAt(row) == "txt") return types.data(types.index(row, 2), Qt::DisplayRole).toString();
            return QString();
        };
        QCOMPARE(tinyPercent(), QString("1.0 %")); // scan root, not tiny.txt's parent
        QCOMPARE(filePercent(tiny), tinyPercent());
        QCOMPARE(typePercent(), tinyPercent());
        types.rebuild(&tree, folder, wt::UnitMode::Bytes);
        QCOMPARE(typePercent(), QString("1.0 %")); // selected directory does not change the scan denominator
        for (bool allocated : {true, false}) {
            directories.setShowAllocated(allocated);
            files.setShowAllocated(allocated);
            types.setShowAllocated(allocated);
            const QString expected = allocated ? "50.0 %" : "1.0 %";
            QCOMPARE(tinyPercent(), expected);
            QCOMPARE(filePercent(tiny), expected);
            QCOMPARE(typePercent(), expected);
            TreemapWidget widget;
            widget.resize(900, 600);
            widget.setMinimumCellSize(1);
            widget.setShowAllocated(allocated);
            widget.setTree(&tree, 0);
            const double clientArea = 898.0 * 572.0;
            const double expectedFolder = allocated ? 0.5 : 0.01;
            QVERIFY(std::abs(area(rectFor(widget, folder)) / clientArea - expectedFolder) < 1e-8);
            QVERIFY(std::abs(area(rectFor(widget, big)) / clientArea - (1.0 - expectedFolder)) < 1e-8);
            const auto before = rectFor(widget, big);
            widget.setMinimumCellSize(40);
            QCOMPARE(rectFor(widget, big), before); // hiding small cells must not enlarge the others
        }
    }

    void mainWindowScanEntryAndMetricSwitch()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path() + "/settings");
        QVERIFY(QDir().mkpath(temp.path() + "/data/nested"));
        for (const auto& item : {qMakePair(QString("nested/tiny.txt"), 1), qMakePair(QString("big.bin"), 9000)}) {
            QFile file(temp.path() + "/data/" + item.first);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray(item.second, 'a')), qint64(item.second));
        }
        MainWindow window;
        window.setBatchMode(true);
        window.show();
        QTest::mouseClick(window.findChild<QPushButton*>("folderSource"), Qt::LeftButton);
        auto* input = window.findChild<QLineEdit*>("scanPath");
        QVERIFY(input->isVisible());
        QVERIFY(!window.findChild<QComboBox*>("scanDrive")->isVisible());
        input->setText(temp.path() + "/data");
        bool done = false;
        window.setScanDoneCallback([&] { done = true; });
        QTest::mouseClick(window.findChild<QPushButton*>("startScan"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        auto* view = window.findChild<QTreeView*>("treeView");
        auto* model = qobject_cast<TreeModel*>(view->model());
        auto* widget = window.findChild<TreemapWidget*>("treemap");
        auto* files = window.findChild<QTableView*>("fileView");
        auto* fileModel = qobject_cast<FileListModel*>(files->model());
        auto* tree = model->tree();
        const auto root = tree->root;
        const auto tiny = tree->findPaths({temp.path() + "/data/nested/tiny.txt"}).front();
        window.setTabIndex(1);
        files->selectRow(0);
        const auto selectedFile = fileModel->nodeAt(0);
        for (bool allocated : {false, true}) {
            QTest::mouseClick(window.findChild<QPushButton*>(allocated ? "allocatedMetric" : "logicalMetric"), Qt::LeftButton);
            QCOMPARE(view->isColumnHidden(2), allocated);
            QCOMPARE(view->isColumnHidden(3), !allocated);
            QCOMPARE(widget->showAllocated_, allocated);
            QCOMPARE(window.findChild<QLabel*>("scanSize")->text(),
                     wt::formatSize(allocated ? tree->nodes[root].alloc : tree->nodes[root].size, wt::UnitMode::Dynamic, 1));
            QCOMPARE(files->selectionModel()->selectedRows().size(), 1);
            QCOMPARE(fileModel->nodeAt(files->selectionModel()->selectedRows().first().row()), selectedFile);
            const auto whole = allocated ? tree->nodes[root].alloc : tree->nodes[root].size;
            const auto bytes = allocated ? tree->nodes[tiny].alloc : tree->nodes[tiny].size;
            const QString expected = QStringLiteral("%1 %").arg(100.0 * double(bytes) / double(whole), 0, 'f', 1);
            QCOMPARE(model->data(model->indexForNode(tiny).siblingAtColumn(1), Qt::DisplayRole).toString(), expected);
        }
        // Modern navigation must still drive the existing tree and selection models.
        window.setTabIndex(0);
        widget->setRoot(tree->nodes[tiny].parent);
        auto* breadcrumb = window.findChild<QToolButton*>("treemapPath");
        QVERIFY(breadcrumb && breadcrumb->menu());
        QVERIFY(QMetaObject::invokeMethod(breadcrumb->menu(), "aboutToShow"));
        QVERIFY(breadcrumb->menu()->actions().size() >= 2);
        breadcrumb->menu()->actions().first()->trigger();
        QCOMPARE(widget->root(), root);
        QCOMPARE(model->nodeOf(view->currentIndex()), root);
        auto* options = window.findChild<QToolButton*>("chartOptions");
        QVERIFY(options && options->menu());
        auto* detail = window.findChild<QSpinBox*>("treemapDetail");
        detail->setValue(4);
        QCOMPARE(widget->detailLevel(), 4);
        widget->setDetailLevel(2);
        QCOMPARE(detail->value(), 2);
        window.applyFilterFromCli("tiny", false);
        auto* search = window.findChild<QLineEdit*>("directorySearch");
        QVERIFY(!search->text().isEmpty());
        search->actions().first()->trigger(); // trailing clear action, before the search icon
        QTRY_COMPARE(model->tree()->nodes[model->tree()->root].size, uint64_t(9001));
        QVERIFY(window.findChild<QLineEdit*>("filterInclude")->text().isEmpty());
        window.applyFilterFromCli("tiny", false);
        tree = model->tree();
        QCOMPARE(window.findChild<QLabel*>("scanSize")->text(), wt::formatSize(tree->nodes[tree->root].alloc, wt::UnitMode::Dynamic, 1));
        QVERIFY(!window.findChild<QLabel*>("volumeTitle")->text().contains("Imported"));
        const auto previousSize = window.findChild<QLabel*>("scanSize")->text();
        window.scanPathFromCli(temp.path() + "/missing-folder");
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel*>("scanState")->text(), QString("Scan failed"), 10000);
        QCOMPARE(window.findChild<QLabel*>("scanSize")->text(), previousSize);
        QVERIFY(window.findChild<QLabel*>("scanTitle")->text().startsWith("Previous"));
        window.exportCsvTo(temp.path() + "/scan.csv");
        done = false;
        window.setScanDoneCallback([&] { done = true; });
        window.scanPathFromCli(temp.path() + "/scan.csv");
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QCOMPARE(window.findChild<QLabel*>("volumeTitle")->text(), QString("Imported scan"));
        done = false;
        window.setScanDoneCallback([&] { done = true; });
        window.scanPathFromCli(temp.path() + "/data; " + temp.path() + "/data/nested");
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QCOMPARE(model->tree()->nodes[model->tree()->root].size, uint64_t(2)); // active tiny filter still applies
        window.applyFilterFromCli({}, false);
        QCOMPARE(model->tree()->nodes[model->tree()->root].size, uint64_t(9002));
        QCOMPARE(window.findChild<QLabel*>("volumeTitle")->text(), QString("Multiple locations"));
        window.close();
    }

    void nestedGeometryAndHitTesting()
    {
        auto tree = fixture();
        TreemapWidget widget;
        widget.resize(1200, 800);
        widget.setTree(&tree, 0);
        QVERIFY(!rectFor(widget, 5).isEmpty());
        QVERIFY(rectFor(widget, 1).contains(rectFor(widget, 4)));
        QVERIFY(rectFor(widget, 4).contains(rectFor(widget, 5)));
        QCOMPARE(widget.nodeAt(rectFor(widget, 5).center()), uint32_t(5));
        QCOMPARE(widget.nodeAt(rectFor(widget, 1, true).center()), uint32_t(1));
        QCOMPARE(widget.nodeAt(rectFor(widget, 0, true).center()), uint32_t(0));
        QVERIFY(rectFor(widget, 7).isEmpty()); // zero files are counted, not drawn
        QVERIFY(std::abs(area(rectFor(widget, 1)) / area(rectFor(widget, 2)) - 9.0) < 0.001);
        widget.setDetailLevel(1);
        QVERIFY(rectFor(widget, 5).isEmpty());
        QVERIFY(!rectFor(widget, 1, true).isEmpty()); // collapsed directory still has a label
        QCOMPARE(tree.nodes[0].size, uint64_t(1000));
    }

    void largeDirectoryHasNoFourThousandCutoff()
    {
        wt::Tree tree;
        tree.root = add(tree, QStringLiteral("C:\\wide"), wt::kNone, true);
        for (int i = 0; i < 5000; ++i)
            add(tree, QString::number(i), 0, false, 100, 100);
        tree.finalize();
        TreemapWidget widget;
        widget.resize(2000, 1500);
        widget.setMinimumCellSize(1);
        widget.setTree(&tree, 0);
        QCOMPARE(widget.cells_.size(), qsizetype(5001));
        QVERIFY(!rectFor(widget, 5000).isEmpty());
        const double expected = (2000.0 - 2) * (1500.0 - 28) / 5000;
        QVERIFY(std::abs(area(rectFor(widget, 5000)) - expected) < 0.1);
        widget.setMinimumCellSize(40);
        QVERIFY(widget.cells_.size() < 5001);
        QCOMPARE(tree.nodes[0].files, uint32_t(5000));
    }

    void sizeModeAndPseudoSpace()
    {
        wt::Tree tree;
        tree.root = add(tree, QStringLiteral("C:\\"), wt::kNone, true);
        add(tree, QStringLiteral("a.bin"), 0, false, 10, 100);
        add(tree, QStringLiteral("b.bin"), 0, false, 30, 100);
        tree.finalize();
        TreemapWidget widget;
        widget.resize(900, 600);
        widget.setTree(&tree, 0);
        QVERIFY(std::abs(area(rectFor(widget, 1)) / area(rectFor(widget, 2)) - 1) < 0.001);
        widget.setShowAllocated(false);
        QVERIFY(std::abs(area(rectFor(widget, 1)) / area(rectFor(widget, 2)) - 1.0 / 3) < 0.001);
        widget.setShowFreeSpace(true);
        widget.setShowUnknownSpace(true);
        widget.setFreeSpace(100, 500);
        widget.setScannedAllocated(300); // unfiltered baseline is authoritative
        for (const auto& cell : widget.cells_)
            QCOMPARE(cell.space, TreemapWidget::Space::None); // no mixing logical size with disk capacity
        widget.setShowAllocated(true);
        int pseudo = 0;
        for (const auto& cell : widget.cells_) {
            if (cell.space == TreemapWidget::Space::None) continue;
            ++pseudo;
            QCOMPARE(cell.bytes, quint64(100));
            QCOMPARE(widget.nodeAt(cell.rect.center()), wt::kNone);
        }
        QCOMPARE(pseudo, 2);
    }

    void clicksAndNavigation()
    {
        auto tree = fixture();
        TreemapWidget widget;
        widget.resize(1200, 800);
        widget.setTree(&tree, 0);
        widget.show();
        QTest::qWait(20);
        QSignalSpy clicked(&widget, &TreemapWidget::nodeClicked);
        const QPoint folderHeader = rectFor(widget, 1, true).center().toPoint();
        QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, folderHeader);
        QCOMPARE(widget.root(), uint32_t(0));
        QCOMPARE(widget.selectedNode(), uint32_t(1));
        QCOMPARE(clicked.count(), 1);
        QTest::mouseDClick(&widget, Qt::LeftButton, Qt::NoModifier, folderHeader);
        QCOMPARE(widget.root(), uint32_t(1));
        widget.setRoot(4);
        widget.goUp();
        QCOMPARE(widget.root(), uint32_t(1));
        widget.goBack(); // history differs from parent navigation
        QCOMPARE(widget.root(), uint32_t(4));
        widget.goForward();
        QCOMPARE(widget.root(), uint32_t(1));
        QTest::keyClick(&widget, Qt::Key_Home, Qt::ControlModifier);
        QCOMPARE(widget.root(), uint32_t(0));
        widget.goBack();
        widget.setRoot(2);
        QVERIFY(!widget.canGoForward());
        QTest::mouseDClick(&widget, Qt::LeftButton, Qt::NoModifier, rectFor(widget, 6).center().toPoint());
        QCOMPARE(widget.root(), uint32_t(2)); // double-click a file does not blank the tree
        auto replacement = fixture();
        widget.setTree(&replacement, 0);
        QVERIFY(!widget.canGoBack());
        QVERIFY(!widget.canGoForward());
        QCOMPARE(widget.selectedNode(), wt::kNone);
    }

    void tagsAndExportDoNotChangeNavigation()
    {
        auto tree = fixture();
        TreemapWidget widget;
        widget.resize(1200, 800);
        widget.setTree(&tree, 0);
        widget.toggleTag(5, 1);
        widget.toggleTag(6, 4);
        widget.setRoot(1);
        const auto before = widget.renderImage(QSize(1000, 700), false, false);
        QVERIFY(!before.isNull());
        widget.selectNode(5);
        QCOMPARE(widget.renderImage(QSize(1000, 700), false, false), before); // no selection in export
        const auto gray = widget.renderImage(QSize(400, 300), true, false);
        for (int y = 0; y < gray.height(); y += 17)
            for (int x = 0; x < gray.width(); x += 13) {
                const QColor color = gray.pixelColor(x, y);
                QCOMPARE(color.red(), color.green());
                QCOMPARE(color.red(), color.blue());
            }
        QCOMPARE(widget.root(), uint32_t(1));
        QCOMPARE(widget.selectedNode(), uint32_t(5));
        QVERIFY(widget.canGoBack());
        widget.clearTagsInRoot();
        QCOMPARE(widget.tags_.size(), qsizetype(1));
        QCOMPARE(widget.tags_.value(tree.path(6).toCaseFolded()), 4);
        auto filtered = wt::filterTree(tree, QStringLiteral("zip"), {}, false);
        widget.setTree(&filtered, filtered.root);
        bool retained = false;
        for (const auto& cell : widget.cells_) if (cell.tag == 4) retained = true;
        QVERIFY(retained);
        widget.clearTags();
        QVERIFY(widget.tags_.isEmpty());
    }

    void emptyAndTinyLayouts()
    {
        TreemapWidget widget;
        widget.setTree(nullptr, wt::kNone);
        QVERIFY(!widget.renderImage(QSize(50, 50), false, false).isNull());
        QCOMPARE(widget.nodeAt(QPointF(20, 20)), wt::kNone);
        wt::Tree tree;
        tree.root = add(tree, QStringLiteral("empty"), wt::kNone, true);
        tree.finalize();
        widget.setTree(&tree, 0);
        QVERIFY(!widget.renderImage(QSize(5, 5), false, false).isNull());
        widget.setDetailLevel(100);
        QCOMPARE(widget.detailLevel(), 15);
        widget.setDetailLevel(0);
        QCOMPARE(widget.detailLevel(), 1);
    }

    void mainWindowTableAndFilterIntegration()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path() + "/settings");
        QVERIFY(QDir().mkpath(temp.path() + "/data/media/nested"));
        QFile movie(temp.path() + "/data/media/movie.mp4");
        QVERIFY(movie.open(QIODevice::WriteOnly));
        QCOMPARE(movie.write(QByteArray(60000, 'a')), qint64(60000));
        movie.close();
        QFile song(temp.path() + "/data/media/nested/song.mp3");
        QVERIFY(song.open(QIODevice::WriteOnly));
        QCOMPARE(song.write(QByteArray(30000, 'b')), qint64(30000));
        song.close();
        MainWindow window;
        window.setBatchMode(true);
        window.resize(1400, 1000);
        window.show();
        bool done = false;
        window.setScanDoneCallback([&done] { done = true; });
        window.scanPathFromCli(temp.path() + "/data");
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        auto* widget = window.findChild<TreemapWidget*>(QStringLiteral("treemap"));
        auto* view = window.findChild<QTreeView*>(QStringLiteral("treeView"));
        auto* files = window.findChild<QTableView*>(QStringLiteral("fileView"));
        QVERIFY(widget && view && files);
        auto* model = qobject_cast<TreeModel*>(view->model());
        QVERIFY(model);
        auto* tree = model->tree();
        QCOMPARE(tree->nodes[tree->root].size, uint64_t(90000));
        QVERIFY(files->model()->rowCount() >= 2);
        uint32_t songId = wt::kNone, mediaId = wt::kNone;
        for (uint32_t i = 0; i < tree->nodes.size(); ++i) {
            if (tree->name(i) == QLatin1String("song.mp3")) songId = i;
            if (tree->name(i) == QLatin1String("media")) mediaId = i;
        }
        QVERIFY(songId != wt::kNone && mediaId != wt::kNone);
        widget->refresh();
        QVERIFY(!rectFor(*widget, songId).isEmpty());
        QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, rectFor(*widget, songId).center().toPoint());
        QCOMPARE(model->nodeOf(view->currentIndex()), songId);
        QCOMPARE(widget->root(), tree->root);
        widget->setRoot(mediaId);
        QCOMPARE(model->nodeOf(view->currentIndex()), mediaId);
        widget->goBack();
        QCOMPARE(model->nodeOf(view->currentIndex()), tree->root);
        // Selecting a file in the table keeps that exact row selected while showing its parent.
        const QModelIndex root = model->index(0, 0, {});
        const QModelIndex media = model->index(int(tree->nodes[mediaId].rowInParent), 0, root);
        const uint32_t nestedId = tree->nodes[songId].parent;
        const QModelIndex nested = model->index(int(tree->nodes[nestedId].rowInParent), 0, media);
        const QModelIndex songIndex = model->index(int(tree->nodes[songId].rowInParent), 0, nested);
        view->setCurrentIndex(songIndex);
        QCOMPARE(model->nodeOf(view->currentIndex()), songId);
        QCOMPARE(widget->root(), nestedId);
        widget->toggleTag(songId, 2);
        window.applyFilterFromCli(QStringLiteral("mp3"), false);
        QCOMPARE(model->tree()->nodes[model->tree()->root].size, uint64_t(30000));
        QCOMPARE(widget->root(), model->tree()->root);
        QVERIFY(!widget->canGoBack());
        QCOMPARE(widget->tags_.size(), qsizetype(1));
        window.exportCsvTo(temp.path() + "/filtered.csv");
        QFile csv(temp.path() + "/filtered.csv");
        QVERIFY(csv.open(QIODevice::ReadOnly));
        const auto text = csv.readAll();
        QVERIFY(text.contains("song.mp3"));
        // Existing CSV export intentionally reads the original scan, not the display filter.
        QVERIFY(text.contains("movie.mp4"));
        window.close();
    }
};

QTEST_MAIN(TreemapTests)
#include "treemap_tests.moc"
