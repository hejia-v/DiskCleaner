#pragma once

#include <QMainWindow>
#include <QSettings>
#include <QSet>
#include <atomic>
#include <memory>
#include <thread>
#include <functional>

#include "core.h"
#include "liveupdate.h"

class QComboBox;
class QPushButton;
class QProgressBar;
class QLabel;
class QTabWidget;
class QTreeView;
class QTableView;
class QLineEdit;
class QRadioButton;
class QFrame;
class QCheckBox;
class QSplitter;
class QTimer;
class QAction;
class QDockWidget;
class TreeModel;
class FileListModel;
class ExtensionModel;
class TreemapWidget;
class DeletionWatcher;

struct ExportOptions {
    bool files = true;
    bool folders = true;
    bool driveSize = false;
    bool dates = false;
    bool folderSize = false;
    bool splitNames = false;
    bool percentOfParent = false;
    bool mftNo = false;
    bool selectedOnly = false;
    int maxDepth = 0;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
    void scanPathFromCli(const QString& path);
    void setTabIndex(int index);
    void configureDuplicates(int mode, bool only);
    void sortFiles(int column, bool descending);
    void openMftDump();
    void openCsvFile();
    void selectMultiple();
    void startMultiScan(const QStringList& targets);
    void dumpMftFile();
    void checkForUpdatesNow();
    void copyFilesToClipboard(bool cut);
    void applyFilterFromCli(const QString& text, bool matchPath);
    void openSelection();
    void exploreSelection();
    void commandPromptHere();
    void copyPathToClipboard();
    void showProperties();
    void deleteSelectionToRecycleBin();
    void copyTreeInfoToClipboard();
    void exportCsvTo(const QString& path);
    void exportCsvFullTo(const QString& path);
    void exportTreemapTo(const QString& path, const QSize& size, bool grayscale, bool freeSpace);
    void setScanDoneCallback(std::function<void()> cb) { onScanDone_ = std::move(cb); }
    void setBatchMode(bool on) { batchMode_ = on; }

protected:
    void closeEvent(QCloseEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private slots:
    void onDriveChanged(int index);
    void onScanClicked();
    void onCancelClicked();
    void onTreeSelectionChanged();
    void onTreeHeaderClicked(int column);
    void onSearchChanged();
    void exportCsv();
    void exportFileTypesCsv();
    void exportTreemapPng();
    void chooseFolder();
    void showAbout();
    void showTreeContextMenu(const QPoint& pos);
    void showFileContextMenu(const QPoint& pos);
    void showTreemapMenu(const QPoint& pos);
    void treemapUp();
    void updateTreemapBreadcrumb();
    void selectTreemapNode(uint32_t node);
    void setUnit(int mode);

private:
    void buildUi();
    void buildMenus();
    void refreshDrives();
    void startScan(const QString& target, wchar_t letter, bool isDrive);
    void startScanPath(const QString& target, wchar_t letter, int mode);
    void onScanFinished(const std::shared_ptr<wt::ScanResult>& result);
    void beginDeletionWatch(const QStringList& targets);
    void flushDeletions();
    void stopLiveUpdates();
    void applyLiveResult(const wt::LiveUpdate& update);
    void setBusy(bool busy);
    void setScanSource(bool folder);
    void setSizeMetric(bool allocated);
    void updateSelectionInfo();
    void updateStats();
    void updateTreemapRoot();
    void refreshExtensions();
    void applySettings();
    void saveSettings();
    void restartAsAdmin();
    void setLanguage(const QString& code);
    void toggleFilterPanel(bool on);
    void applyFilter();
    void clearFilter();
    void refreshFilteredViews();
    void showItemMenu(QWidget* source, const QPoint& globalPos);
    QList<uint32_t> selectedNodes(QWidget* source) const;
    QList<uint32_t> actionNodes() const;
    QWidget* menuSource_ = nullptr;
    wt::Tree* displayTree() const;
    void writeCsv(const QString& path, const ExportOptions& opt);
    std::function<void()> onScanDone_;

    QComboBox* driveBox_ = nullptr;
    QLineEdit* scanPathEdit_ = nullptr;
    QPushButton* diskSourceBtn_ = nullptr;
    QPushButton* folderSourceBtn_ = nullptr;
    QPushButton* browseBtn_ = nullptr;
    QPushButton* logicalMetricBtn_ = nullptr;
    QPushButton* allocatedMetricBtn_ = nullptr;
    QPushButton* exploreBtn_ = nullptr;
    QPushButton* copyPathBtn_ = nullptr;
    QLabel* scanTitle_ = nullptr;
    QLabel* metricLabel_ = nullptr;
    QLabel* alternateSizeLabel_ = nullptr;
    QLabel* scanStateLabel_ = nullptr;
    QLabel* volumeLabel_ = nullptr;
    QLabel* treemapSummary_ = nullptr;
    QLabel* emptyHint_ = nullptr;
    QProgressBar* volumeBar_ = nullptr;
    QAction* allocatedAction_ = nullptr;
    QAction* filterAction_ = nullptr;
    QDockWidget* typesDock_ = nullptr;
    bool folderSource_ = false;
    bool showAllocated_ = true;
    bool displayingPreviousScan_ = false;
    int resultScanMode_ = 0;
    QPushButton* scanBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* selLabel_ = nullptr;
    QLabel* totalLabel_ = nullptr;
    QLabel* usedLabel_ = nullptr;
    QLabel* freeLabel_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTreeView* treeView_ = nullptr;
    QTableView* fileView_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QComboBox* maxFilesBox_ = nullptr;
    QCheckBox* showFoldersBox_ = nullptr;
    QFrame* filterBar_ = nullptr;
    QLineEdit* filterInclude_ = nullptr;
    QLineEdit* filterExclude_ = nullptr;
    QRadioButton* filterPathRadio_ = nullptr;
    std::unique_ptr<wt::Tree> filteredTree_;
    QString activeInclude_, activeExclude_;
    bool activeMatchPath_ = false;
    bool syncingTreemapSelection_ = false;
    QComboBox* dupsBox_ = nullptr;
    QCheckBox* dupsOnlyBox_ = nullptr;
    QSplitter* treeSplit_ = nullptr;
    QTableView* extView_ = nullptr;
    QLabel* pathLabel_ = nullptr;
    QLabel* treemapPath_ = nullptr;
    QTimer* searchTimer_ = nullptr;
    QTimer* extTimer_ = nullptr;
    DeletionWatcher* deletionWatcher_ = nullptr;
    QTimer* deletionTimer_ = nullptr;
    QStringList watchTargets_;
    int watchScanMode_ = 0;
    wchar_t watchDriveLetter_ = 0;
    QSet<QString> pendingDeletions_;
    wt::FileChanges pendingChanges_;
    std::unique_ptr<wt::LiveUpdate> readyUpdate_;
    std::thread liveWorker_;
    std::atomic_bool liveCancel_{false};
    bool liveBusy_ = false;
    uint64_t liveGeneration_ = 0;
    QHash<QString, int> liveRetries_;
    QSet<uint32_t> expandedNodes_;
    bool watchNeedsRescan_ = false;
    QString watchError_;

    TreeModel* treeModel_ = nullptr;
    FileListModel* fileModel_ = nullptr;
    ExtensionModel* extModel_ = nullptr;
    TreemapWidget* treemap_ = nullptr;

    std::unique_ptr<wt::ScanResult> result_;
    std::thread worker_;
    std::atomic_bool cancel_{false};
    bool scanning_ = false;
    bool closing_ = false;
    bool batchMode_ = false;
    bool showFreeSpace_ = false;
    int sortCol_ = 2;
    Qt::SortOrder sortOrder_ = Qt::DescendingOrder;
    QString currentTarget_;

    QSettings settings_;
    wt::UnitMode unit_ = wt::UnitMode::Dynamic;
    int decimals_ = 1;
};















