#include <QtTest>
#include <windows.h>
#include <QAbstractItemModelTester>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>
#include "core.h"
#include "liveupdate.h"
#include "mainwindow.h"
#include "models.h"
#include "treemap.h"

namespace {
template<class T> void put(QByteArray& bytes, qsizetype offset, T value)
{
    memcpy(bytes.data() + offset, &value, sizeof(value));
}

uint32_t add(wt::Tree& tree, const QString& name, uint32_t parent, bool directory)
{
    wt::Node node;
    node.parent = parent;
    node.isDir = directory;
    node.size = directory ? 0 : 10;
    node.alloc = directory ? 0 : 4096;
    node.nameOff = uint32_t(tree.names.size());
    node.nameLen = uint32_t(name.size());
    const auto* chars = reinterpret_cast<const char16_t*>(name.utf16());
    tree.names.insert(tree.names.end(), chars, chars + name.size());
    tree.nodes.push_back(node);
    return uint32_t(tree.nodes.size() - 1);
}

wt::Tree emptyTree()
{
    wt::Tree tree;
    tree.root = add(tree, "C:\\resource-test", wt::kNone, true);
    tree.finalize();
    return tree;
}

wt::LiveUpdate entryUpdate(const QString& path, bool present)
{
    wt::LiveEntry entry;
    entry.state = present ? wt::LiveEntry::Present : wt::LiveEntry::Missing;
    entry.path = path;
    entry.name = path.mid(path.lastIndexOf('/') + 1);
    entry.metadata.size = 10;
    entry.metadata.alloc = 4096;
    wt::LiveUpdate update;
    update.entries.push_back(entry);
    return update;
}

size_t capacityBytes(const wt::Tree& tree)
{
    return tree.nodes.capacity() * sizeof(wt::Node) + tree.names.capacity() * sizeof(char16_t)
           + tree.children.capacity() * sizeof(uint32_t);
}
}

class ResourceTests : public QObject {
    Q_OBJECT
private slots:
    void malformedMftAttributes_data()
    {
        QTest::addColumn<QByteArray>("record");
        QByteArray record(1024, '\0');
        memcpy(record.data(), "FILE", 4);
        put<uint16_t>(record, 0x14, 0x38);
        put<uint16_t>(record, 0x16, 1);
        put<uint32_t>(record, 0x18, 1024);
        put<uint32_t>(record, 0x38, 0x20);
        put<uint32_t>(record, 0x3c, 24);
        put<uint32_t>(record, 0x50, 0x20);
        put<uint32_t>(record, 0x54, 0xFFFFFFE8);
        QTest::newRow("wrapping-cycle") << record;
        put<uint32_t>(record, 0x3c, 8);
        QTest::newRow("short-resident-header") << record;
        put<uint32_t>(record, 0x3c, 24);
        record[0x40] = 1;
        QTest::newRow("short-nonresident-header") << record;
        record[0x40] = 0;
        put<uint32_t>(record, 0x38, 0x30);
        put<uint32_t>(record, 0x48, 0xFFFFFFFF);
        put<uint16_t>(record, 0x4c, 24);
        QTest::newRow("overflowing-value-length") << record;
    }

    void malformedMftAttributes()
    {
        QFETCH(QByteArray, record);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QFile file(temp.filePath("bad.mft"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(record), record.size());
        file.close();
        QElapsedTimer timer; timer.start();
        const auto result = wt::scanMftDump(file.fileName(), {});
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains("Invalid MFT"));
        QVERIFY(timer.elapsed() < 2000);
    }

    void untrustedMftCountsAndSparseIds()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath("count.mft");
        QByteArray header("WTMFT\0\0\1", 8);
        header.resize(20);
        put<uint32_t>(header, 8, 1024);
        for (const uint64_t count : {uint64_t(1), uint64_t(1) << 32, std::numeric_limits<uint64_t>::max()}) {
            put(header, 12, count);
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(header), qint64(20));
            file.close();
            const auto result = wt::scanMftDump(path, {});
            QVERIFY(!result.ok);
            QVERIFY(result.error.contains("record count"));
        }
        QString error;
        QVERIFY(wt::writeSyntheticMft(path, 1, error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.seek(20 + 11 * 1024 + 0x2c));
        const uint32_t sparseId = 0xFFFFFFFE;
        QCOMPARE(file.write(reinterpret_cast<const char*>(&sparseId), sizeof(sparseId)), qint64(4));
        file.close();
        const auto result = wt::scanMftDump(path, {});
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.tree.nodes.size(), size_t(7));
        QCOMPARE(result.tree.nodes.back().mftRec, sparseId);
        QCOMPARE(result.tree.nodes[result.tree.root].files, uint32_t(1));
    }

    void unusedMftRecordsIgnoreStaleAttributes()
    {
        QTemporaryDir temp;
        QString error;
        const auto path = temp.filePath("unused.mft");
        QVERIFY(wt::writeSyntheticMft(path, 1, error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        // The first record is unused in this fixture; its stale length must not fail the scan.
        QVERIFY(file.seek(20 + 0x16));
        const uint16_t flags = 0;
        QCOMPARE(file.write(reinterpret_cast<const char*>(&flags), sizeof(flags)), qint64(2));
        QVERIFY(file.seek(20 + 0x3c));
        const uint32_t staleLength = 0xFFFFFFFF;
        QCOMPARE(file.write(reinterpret_cast<const char*>(&staleLength), sizeof(staleLength)), qint64(4));
        file.close();
        const auto result = wt::scanMftDump(path, {});
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.tree.nodes[result.tree.root].files, uint32_t(1));
    }

    void taskExceptionsBecomeFailures()
    {
        const auto allocation = wt::runScanTask([]() -> wt::ScanResult { throw std::bad_alloc(); });
        QVERIFY(!allocation.ok);
        QVERIFY(allocation.error.contains("memory"));
        const auto parsing = wt::runScanTask([]() -> wt::ScanResult { throw std::runtime_error("test parse error"); });
        QVERIFY(!parsing.ok);
        QCOMPARE(parsing.error, QString("test parse error"));
    }

    void cancelWideDirectoryAndFinalization()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        for (int i = 0; i < 20001; ++i) {
            QFile file(temp.filePath(QString::number(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        std::atomic_bool cancel = false;
        wt::ScanOptions options;
        options.cancel = &cancel;
        options.status = [&](const QString& status) {
            if (status == "Scanning: 20000 items") cancel = true;
        };
        DWORD before = 0, after = 0;
        QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &before));
        for (int i = 0; i < 3; ++i) {
            cancel = false;
            const auto result = wt::scanFolder(temp.path(), options);
            QVERIFY(cancel.load());
            QVERIFY(!result.ok);
            QCOMPARE(result.error, QString("canceled"));
        }
        QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &after));
        QVERIFY2(after <= before + 1, "Canceled enumeration leaked a search handle");
        cancel = false;
        options.status = [&](const QString& status) { if (status == "Building tree...") cancel = true; };
        const auto finalized = wt::scanFolder(temp.path(), options);
        QVERIFY(cancel.load());
        QVERIFY(!finalized.ok);
        QCOMPARE(finalized.error, QString("canceled"));
        const auto preCanceled = wt::scanFolder(temp.filePath("missing"), options);
        QCOMPARE(preCanceled.error, QString("canceled"));
        const auto drive = wt::scanDrive(L'?', options);
        QCOMPARE(drive.error, QString("canceled"));
    }

    void cancelTreeProcessingAndImports()
    {
        auto tree = emptyTree();
        for (int i = 0; i < 10000; ++i) add(tree, QString::number(i), tree.root, false);
        std::atomic_bool cancel = true;
        auto run = [&](const std::function<void()>& operation) {
            return wt::runScanTask([&] { operation(); wt::ScanResult result; result.ok = true; return result; });
        };
        QCOMPARE(run([&] { tree.finalize(&cancel); }).error, QString("canceled"));
        tree.finalize();
        QCOMPARE(run([&] { tree.sortRecursive(0, true, &cancel); }).error, QString("canceled"));
        std::vector<wt::Tree> parts;
        parts.push_back(std::move(tree));
        QCOMPARE(run([&] { wt::mergeTrees(parts, "merged", &cancel); }).error, QString("canceled"));
        QTemporaryDir temp;
        QString error;
        const auto mft = temp.filePath("valid.mft");
        QVERIFY(wt::writeSyntheticMft(mft, 20, error));
        wt::ScanOptions options;
        options.cancel = &cancel;
        options.status = [&](const QString& status) { if (status == "Building tree...") cancel = true; };
        cancel = false;
        QCOMPARE(wt::scanMftDump(mft, options).error, QString("canceled"));
        QFile csv(temp.filePath("valid.csv"));
        QVERIFY(csv.open(QIODevice::WriteOnly));
        csv.write("File Name,Path,Size\na.txt,C:\\fixture,10\n");
        csv.close();
        cancel = false;
        QCOMPARE(wt::importCsvTree(csv.fileName(), options).error, QString("canceled"));
    }

    void liveChurnHasBoundedStorage()
    {
        auto tree = emptyTree();
        const auto create = entryUpdate("C:/resource-test/churn.txt", true);
        const auto remove = entryUpdate("C:/resource-test/churn.txt", false);
        size_t maximum = 0;
        int compactions = 0;
        for (int i = 0; i < 100000; ++i) {
            wt::applyLiveUpdate(tree, create);
            wt::applyLiveUpdate(tree, remove);
            if (tree.needsCompaction()) { tree.compact(); ++compactions; }
            maximum = std::max(maximum, capacityBytes(tree));
        }
        QVERIFY(compactions > 50);
        QCOMPARE(tree.nodes[tree.root].files, uint32_t(0));
        QVERIFY(tree.nodes.size() <= 1026);
        QVERIFY2(maximum < 512 * 1024, qPrintable(QString::number(maximum)));
        qInfo() << "100k create/delete cycles; peak array capacity bytes:" << maximum;
        // A removed root must survive compaction and be recreatable without counter underflow.
        wt::applyLiveUpdate(tree, entryUpdate("C:/resource-test", false));
        tree.compact();
        auto root = entryUpdate("C:/resource-test", true);
        root.entries[0].metadata.isDir = 1;
        wt::applyLiveUpdate(tree, root);
        QVERIFY(!tree.nodes[tree.root].removed);
        QVERIFY(!tree.needsCompaction());
    }

    void repeatedRenamesAndMovesHaveBoundedStorage()
    {
        auto tree = emptyTree();
        add(tree, "a", tree.root, true);
        add(tree, "b", tree.root, true);
        add(tree, "file.txt", 1, false);
        tree.finalize();
        QString path = "C:/resource-test/a/file.txt";
        for (int i = 0; i < 10000; ++i) {
            const QString next = QString("C:/resource-test/%1/file-%2.txt").arg(i % 2 ? "a" : "b").arg(i);
            wt::LiveUpdate update;
            update.renames.append({path, next});
            wt::applyLiveUpdate(tree, update);
            if (tree.needsCompaction()) tree.compact();
            path = next;
        }
        QCOMPARE(tree.nodes.size(), size_t(4));
        QCOMPARE(tree.nodes[tree.root].files, uint32_t(1));
        QVERIFY(!tree.findPaths({path}).empty());
        QVERIFY(capacityBytes(tree) < 128 * 1024);
    }

    void directoryMovesReleaseUnusedChildCapacity()
    {
        auto tree = emptyTree();
        for (int i = 0; i < 12; ++i) add(tree, "dir" + QString::number(i), tree.root, true);
        for (int i = 0; i < 1100; ++i) add(tree, "file" + QString::number(i), 1, false);
        tree.finalize();
        int compactions = 0;
        for (int directory = 1; directory < 12; ++directory) {
            wt::LiveUpdate move;
            for (int i = 0; i < 1100; ++i) {
                const auto from = QString("C:/resource-test/dir%1/file%2").arg(directory - 1).arg(i);
                const auto to = QString("C:/resource-test/dir%1/file%2").arg(directory).arg(i);
                move.renames.append({from, to});
            }
            wt::applyLiveUpdate(tree, move);
            if (tree.needsCompaction()) { tree.compact(); ++compactions; }
            QVERIFY(tree.children.size() <= tree.nodes.size() * 4);
        }
        QVERIFY(compactions > 0);
        QCOMPARE(tree.nodes[tree.root].files, uint32_t(1100));
        QCOMPARE(tree.nodes.size(), size_t(1113));
        QVERIFY(!tree.findPaths({"C:/resource-test/dir11/file1099"}).empty());
    }

    void compactionPreservesViews_data()
    {
        QTest::addColumn<bool>("filtered");
        QTest::newRow("unfiltered-navigation-selection-tags") << false;
        QTest::newRow("filtered-projection") << true;
    }

    void compactionPreservesViews()
    {
        QFETCH(bool, filtered);
        QTemporaryDir settings;
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        MainWindow window;
        window.setBatchMode(true);
        window.maxFilesBox_->setCurrentIndex(window.maxFilesBox_->findData(0));
        auto result = std::make_shared<wt::ScanResult>();
        auto& tree = result->tree;
        tree = emptyTree();
        for (int i = 0; i < 1100; ++i) add(tree, "old" + QString::number(i), tree.root, false);
        const auto folder = add(tree, "keep", tree.root, true);
        const auto file = add(tree, "keep.txt", folder, false);
        tree.finalize();
        result->ok = true;
        window.onScanFinished(result);
        if (filtered) window.applyFilterFromCli("keep", false);
        auto* shown = window.displayTree();
        const auto shownFolder = shown->findPaths({"C:/resource-test/keep"}).front();
        const auto shownFile = shown->findPaths({"C:/resource-test/keep/keep.txt"}).front();
        window.treeView_->expand(window.treeModel_->indexForNode(shownFolder));
        window.treeView_->setCurrentIndex(window.treeModel_->indexForNode(shownFolder));
        window.treemap_->toggleTag(shownFile, 2);
        for (int row = 0; row < window.fileModel_->rowCount(); ++row)
            if (window.fileModel_->nodeAt(row) == shownFile) window.fileView_->selectRow(row);
        QCOMPARE(window.selectedNodes(window.fileView_), QList<uint32_t>{shownFile});
        QAbstractItemModelTester tester(window.treeModel_, QAbstractItemModelTester::FailureReportingMode::QtTest);
        wt::LiveUpdate update;
        for (int i = 0; i < 1100; ++i)
            update.entries.push_back(entryUpdate("C:/resource-test/old" + QString::number(i), false).entries.front());
        window.applyLiveResult(update);
        QCOMPARE(window.result_->tree.nodes.size(), size_t(3));
        QCOMPARE(window.result_->tree.nodes[0].files, uint32_t(1));
        shown = window.displayTree();
        QCOMPARE(shown->path(window.treeModel_->nodeOf(window.treeView_->currentIndex())), QString("C:\\resource-test\\keep"));
        QCOMPARE(shown->path(window.treemap_->root()), QString("C:\\resource-test\\keep"));
        QCOMPARE(window.treemap_->tags_.value("c:\\resource-test\\keep\\keep.txt"), 2);
        if (!filtered) {
            QVERIFY(window.treeView_->isExpanded(window.treeModel_->indexForNode(1)));
            QCOMPARE(window.selectedNodes(window.fileView_), QList<uint32_t>{2});
            window.treemap_->goBack();
            QCOMPARE(window.treemap_->root(), shown->root);
            window.treemap_->goForward();
            QCOMPARE(window.treemap_->root(), uint32_t(1));
        }
        QCOMPARE(window.extModel_->rowCount(), 1);
        Q_UNUSED(file);
    }

    void mftExportRunsOffUiAndCancels()
    {
        QTemporaryDir settings;
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        std::atomic_bool entered = false, offUi = false;
        MainWindow window;
        window.setBatchMode(true);
        window.show();
        int beats = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] { ++beats; });
        timer.start(5);
        window.startMftDump(L'C', settings.filePath("dump.mft"),
            [&](wchar_t, const QString&, std::atomic_bool& cancel, const wt::StatusFn& status) {
                offUi = QThread::currentThread() != qApp->thread();
                entered = true;
                status("export running");
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (!cancel.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                wt::ScanResult result;
                result.error = cancel ? "canceled" : "test backend timeout";
                return result;
            });
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(beats >= 3, 1000);
        QVERIFY(offUi.load());
        QVERIFY(window.dumping_);
        QTRY_COMPARE(window.dumpProgress_->labelText(), QString("export running"));
        auto* cancelButton = window.dumpProgress_->findChild<QPushButton*>();
        QVERIFY(cancelButton);
        QTest::mouseClick(cancelButton, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!window.dumping_, 2000);
        QVERIFY(window.dumpProgress_ == nullptr);
        QVERIFY(window.dumpCancel_.load());
        QVERIFY(window.statusLabel_->text().contains("cancel", Qt::CaseInsensitive));
        // Exceptions at the actual async export boundary must also complete the task.
        window.startMftDump(L'C', settings.filePath("bad.mft"),
            [](wchar_t, const QString&, std::atomic_bool&, const wt::StatusFn&) -> wt::ScanResult { throw std::bad_alloc(); });
        QTRY_VERIFY_WITH_TIMEOUT(!window.dumping_, 2000);
        QVERIFY(window.statusLabel_->text().contains("memory"));
    }

    void closeWaitsAsynchronouslyForExport()
    {
        QTemporaryDir settings;
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        std::atomic_bool entered = false, release = false;
        MainWindow window;
        window.setBatchMode(true);
        window.show();
        window.startMftDump(L'C', settings.filePath("dump.mft"),
            [&](wchar_t, const QString&, std::atomic_bool&, const wt::StatusFn&) {
                entered = true;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (!release.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                wt::ScanResult result; result.error = "canceled"; return result;
            });
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 1000);
        QElapsedTimer timer; timer.start();
        window.close();
        QVERIFY(timer.elapsed() < 500);
        QVERIFY(window.isVisible());
        QVERIFY(window.dumpCancel_.load());
        bool eventProcessed = false;
        QTimer::singleShot(0, &window, [&] { eventProcessed = true; release = true; });
        QTRY_VERIFY_WITH_TIMEOUT(eventProcessed && !window.isVisible(), 2000);
        QVERIFY(!window.dumping_);
    }

    void canceledExportLeavesExistingFile()
    {
        QTemporaryDir temp;
        const auto path = temp.filePath("existing.mft");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("previous export");
        file.close();
        std::atomic_bool cancel = true;
        QCOMPARE(wt::dumpMftToFile(L'?', path, cancel, {}).error, QString("canceled"));
        cancel = false;
        QVERIFY(!wt::dumpMftToFile(L'?', path, cancel, {}).ok);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("previous export"));
    }

    void canceledScanKeepsPreviousResultAndCloses()
    {
        QTemporaryDir temp;
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.filePath("settings"));
        MainWindow window;
        window.setBatchMode(true);
        auto previous = std::make_shared<wt::ScanResult>();
        previous->tree = emptyTree();
        previous->ok = true;
        window.onScanFinished(previous);
        auto* original = window.result_.get();
        window.startScanPath(temp.path(), 0, 0);
        window.onCancelClicked();
        QTRY_VERIFY_WITH_TIMEOUT(!window.scanning_, 3000);
        QCOMPARE(window.result_.get(), original);
        QCOMPARE(window.scanStateLabel_->text(), QString("Scan canceled"));
        // Even if success was queued just before the click, cancellation wins at publication.
        auto completed = std::make_shared<wt::ScanResult>();
        completed->tree = emptyTree(); completed->ok = true;
        window.onScanFinished(completed);
        QCOMPARE(window.result_.get(), original);
        window.startMultiScan({temp.path(), temp.path()});
        window.onCancelClicked();
        QTRY_VERIFY_WITH_TIMEOUT(!window.scanning_, 3000);
        QCOMPARE(window.result_.get(), original);
        QCOMPARE(window.scanStateLabel_->text(), QString("Scan canceled"));
        window.show();
        window.startScanPath(temp.path(), 0, 0);
        QElapsedTimer timer; timer.start();
        window.close();
        QVERIFY(timer.elapsed() < 500);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 3000);
        QVERIFY(!window.scanning_);
    }

    void newScanDefersUntilLiveWorkerCompletes()
    {
        QTemporaryDir temp;
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.filePath("settings"));
        MainWindow window;
        window.setBatchMode(true);
        auto previous = std::make_shared<wt::ScanResult>();
        previous->tree = emptyTree(); previous->ok = true;
        window.onScanFinished(previous);
        window.pendingDeletions_.insert(temp.path());
        window.flushDeletions();
        QVERIFY(window.liveBusy_);
        QElapsedTimer timer; timer.start();
        window.startScanPath(temp.path(), 0, 0);
        QVERIFY(timer.elapsed() < 500);
        QVERIFY(bool(window.pendingScan_));
        QVERIFY(window.liveCancel_.load());
        bool scanned = false;
        window.setScanDoneCallback([&] { scanned = true; });
        QTRY_VERIFY_WITH_TIMEOUT(scanned, 5000);
        QVERIFY(!window.liveBusy_);
        QVERIFY(!window.pendingScan_);
        QCOMPARE(window.result_->tree.path(window.result_->tree.root), QDir::toNativeSeparators(temp.path()));
    }
};

QTEST_MAIN(ResourceTests)
#include "resource_tests.moc"
