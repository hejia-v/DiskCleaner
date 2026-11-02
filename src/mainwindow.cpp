#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "mainwindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QButtonGroup>
#include <QDockWidget>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QMenu>
#include <QSpinBox>
#include <QToolButton>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QInputDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFontMetrics>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QProcess>
#include <QMessageBox>
#include <QProgressBar>
#include <QProgressDialog>
#include <QClipboard>
#include <QDate>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDrag>
#include <QUrl>
#include <QRadioButton>
#include <QShortcut>
#include <QPushButton>
#include <QSettings>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QTabWidget>
#include <QTableView>
#include <QTextBrowser>
#include <QTextStream>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include "models.h"
#include "deletionwatcher.h"
#include "shellintegration.h"
#include "theme.h"
#include "updatecheck.h"
#include "treemap.h"

namespace {

QString driveLabel(wchar_t letter)
{
    wchar_t root[8];
    swprintf_s(root, L"%c:\\", letter);
    wchar_t label[MAX_PATH + 1] = {};
    wchar_t fs[MAX_PATH + 1] = {};
    DWORD serial = 0, maxLen = 0, flags = 0;
    if (GetVolumeInformationW(root, label, MAX_PATH, &serial, &maxLen, &flags, fs, MAX_PATH))
        return QString::fromWCharArray(label);
    return {};
}

bool volumeUsage(const QString& path, quint64& total, quint64& freeBytes)
{
    std::wstring p = QDir::toNativeSeparators(path).toStdWString();
    wchar_t volRoot[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(p.c_str(), volRoot, MAX_PATH)) return false;
    ULARGE_INTEGER avail{}, tot{}, totFree{};
    if (!GetDiskFreeSpaceExW(volRoot, &avail, &tot, &totFree)) return false;
    total = tot.QuadPart;
    freeBytes = avail.QuadPart;
    return true;
}

QString ftToText(int64_t ft)
{
    if (ft <= 0) return {};
    const qint64 ms = ft / 10000 - 11644473600000LL;
    if (ms <= 0) return {};
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

class PercentageDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem background(option);
        initStyleOption(&background, index);
        const QString text = background.text;
        background.text.clear();
        const auto* style = option.widget ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, option.widget);
        const double ratio = std::clamp(index.data(Qt::UserRole + 1).toDouble(), 0.0, 1.0);
        QRect track = option.rect.adjusted(10, 0, -66, 0);
        track.setTop(option.rect.center().y() - 5);
        track.setHeight(10);
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(option.palette.color(QPalette::AlternateBase));
        painter->drawRoundedRect(track, 3, 3);
        painter->setBrush(option.palette.color(QPalette::Link));
        painter->drawRoundedRect(QRectF(track.x(), track.y(), track.width() * ratio, track.height()), 3, 3);
        painter->setPen(option.palette.color(option.state & QStyle::State_Selected
                                                ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(option.rect.adjusted(0, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter, text);
        painter->restore();
    }
};

}  // namespace

MainWindow::MainWindow()
    : settings_(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("DiskCleaner"),
                QStringLiteral("DiskCleaner"))
{
    setWindowTitle(QStringLiteral("DiskCleaner"));
    resize(1440, 960);
    setMinimumSize(1080, 680);
    buildUi();
    buildMenus();
    refreshDrives();
    applySettings();
    setBusy(false);
    deletionWatcher_ = new DeletionWatcher(this);
    deletionTimer_ = new QTimer(this);
    deletionTimer_->setSingleShot(true);
    deletionTimer_->setInterval(200);
    connect(deletionTimer_, &QTimer::timeout, this, &MainWindow::flushDeletions);
    connect(deletionWatcher_, &DeletionWatcher::changesReady, this, [this](const wt::FileChanges& changes) {
        for (const auto& path : changes.paths) {
            pendingDeletions_.insert(path);
            liveRetries_.remove(wt::livePathKey(path));
        }
        pendingChanges_.renames.append(changes.renames);
        pendingChanges_.createdPaths.append(changes.createdPaths);
        for (const auto& rename : changes.renames) {
            for (auto& target : watchTargets_) {
                const auto key = wt::livePathKey(target), oldKey = wt::livePathKey(rename.first);
                if (key == oldKey || key.startsWith(oldKey + '/'))
                    target = QDir::toNativeSeparators(rename.second) + QDir::toNativeSeparators(target).mid(QDir::toNativeSeparators(rename.first).size());
            }
            if (wt::livePathKey(currentTarget_) == wt::livePathKey(rename.first)) currentTarget_ = rename.second;
        }
        if (watchScanMode_ < 0) currentTarget_ = watchTargets_.join(QStringLiteral("; "));
        if (pendingDeletions_.size() + pendingChanges_.renames.size() + pendingChanges_.createdPaths.size() > 32768) {
            pendingDeletions_.clear(); pendingChanges_ = {}; watchNeedsRescan_ = true;
        }
        if (!scanning_ && !liveBusy_ && !deletionTimer_->isActive()) deletionTimer_->start(0);
    });
    connect(deletionWatcher_, &DeletionWatcher::rescanRequired, this, [this] {
        watchNeedsRescan_ = true;
        if (!scanning_) deletionTimer_->start(0);
    });
    connect(deletionWatcher_, &DeletionWatcher::watchError, this, [this](const QString& error) {
        watchError_ = error;
        statusLabel_->setText(error);
    });
    connect(treeView_, &QTreeView::expanded, this, [this](const QModelIndex& index) {
        expandedNodes_.insert(treeModel_->nodeOf(index));
    });
    connect(treeView_, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
        expandedNodes_.remove(treeModel_->nodeOf(index));
    });
}

MainWindow::~MainWindow()
{
    closing_ = true;
    cancel_ = true;
    dumpCancel_ = true;
    deletionWatcher_->stop();
    stopLiveUpdates();
    if (worker_.joinable()) worker_.join();
    if (dumpWorker_.joinable()) dumpWorker_.join();
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("mainSurface"));
    auto* vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(8, 6, 8, 0);
    vbox->setSpacing(6);
    const auto heading = [](const QString& text, QWidget* parent) {
        auto* label = new QLabel(text, parent);
        label->setProperty("heading", true);
        return label;
    };
    auto* modeHint = new QToolButton(central);
    modeHint->setText(wt::isProcessElevated() ? tr("Fast scan available") : tr("Standard scan mode"));
    modeHint->setIcon(style()->standardIcon(QStyle::SP_MessageBoxInformation));
    modeHint->setToolButtonStyle(Qt::ToolButtonIconOnly);
    modeHint->setAccessibleName(modeHint->text());
    modeHint->setToolTip(tr("Administrator rights allow faster NTFS drive scans. Folder scans work without them."));
    auto* modeMenu = new QMenu(modeHint);
    auto* restart = modeMenu->addAction(tr("Restart as Administrator"));
    restart->setEnabled(!wt::isProcessElevated());
    connect(restart, &QAction::triggered, this, &MainWindow::restartAsAdmin);
    modeHint->setMenu(modeMenu);
    modeHint->setPopupMode(QToolButton::InstantPopup);
    auto* top = new QHBoxLayout();
    top->setSpacing(6);
    diskSourceBtn_ = new QPushButton(style()->standardIcon(QStyle::SP_DriveHDIcon), tr("Drive"), central);
    folderSourceBtn_ = new QPushButton(style()->standardIcon(QStyle::SP_DirIcon), tr("Folder"), central);
    diskSourceBtn_->setObjectName(QStringLiteral("diskSource"));
    folderSourceBtn_->setObjectName(QStringLiteral("folderSource"));
    auto* sourceGroup = new QButtonGroup(central);
    for (auto* button : {diskSourceBtn_, folderSourceBtn_}) {
        button->setCheckable(true);
        button->setProperty("segment", true);
        sourceGroup->addButton(button);
        top->addWidget(button);
    }
    driveBox_ = new QComboBox(central);
    driveBox_->setObjectName(QStringLiteral("scanDrive"));
    driveBox_->setMinimumWidth(220);
    top->addWidget(driveBox_, 1);
    scanPathEdit_ = new QLineEdit(central);
    scanPathEdit_->setObjectName(QStringLiteral("scanPath"));
    scanPathEdit_->setPlaceholderText(tr("Choose or enter a folder path"));
    scanPathEdit_->setClearButtonEnabled(true);
    top->addWidget(scanPathEdit_, 1);
    browseBtn_ = new QPushButton(tr("Choose folder..."), central);
    browseBtn_->setObjectName(QStringLiteral("browseFolder"));
    top->addWidget(browseBtn_);
    scanBtn_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), tr("Start scan"), central);
    scanBtn_->setObjectName(QStringLiteral("startScan"));
    scanBtn_->setProperty("primary", true);
    scanBtn_->setMinimumWidth(100);
    scanBtn_->setDefault(true);
    cancelBtn_ = new QPushButton(tr("Cancel"), central);
    progress_ = new QProgressBar(central);
    progress_->setRange(0, 0);
    progress_->setFixedWidth(80);
    top->addWidget(scanBtn_);
    top->addWidget(cancelBtn_);
    top->addWidget(progress_);
    top->addWidget(modeHint);
    vbox->addLayout(top);

    auto* summary = new QFrame(central);
    summary->setObjectName(QStringLiteral("scanSummary"));
    auto* summaryLayout = new QHBoxLayout(summary);
    summaryLayout->setContentsMargins(6, 4, 6, 4);
    summaryLayout->setSpacing(12);
    auto* scanStats = new QVBoxLayout();
    scanStats->setSpacing(0);
    scanTitle_ = heading(tr("Ready to scan"), summary);
    scanTitle_->setObjectName(QStringLiteral("scanTitle"));
    scanTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    scanStats->addWidget(scanTitle_);
    auto* values = new QHBoxLayout();
    values->setSpacing(6);
    metricLabel_ = new QLabel(tr("Space on disk"), summary);
    metricLabel_->setProperty("muted", true);
    metricLabel_->setProperty("summaryDetail", true);
    values->addWidget(metricLabel_);
    selLabel_ = new QLabel(QStringLiteral("—"), summary);
    selLabel_->setObjectName(QStringLiteral("scanSize"));
    values->addWidget(selLabel_);
    alternateSizeLabel_ = new QLabel(summary);
    alternateSizeLabel_->setProperty("muted", true);
    alternateSizeLabel_->setProperty("summaryDetail", true);
    values->addWidget(alternateSizeLabel_);
    values->addStretch();
    scanStateLabel_ = new QLabel(tr("Choose a location to begin"), summary);
    scanStateLabel_->setObjectName(QStringLiteral("scanState"));
    scanStateLabel_->setProperty("summaryDetail", true);
    values->addWidget(scanStateLabel_);
    scanStats->addLayout(values);
    summaryLayout->addLayout(scanStats, 1);
    auto* separator = new QFrame(summary);
    separator->setFrameShape(QFrame::VLine);
    separator->setProperty("divider", true);
    summaryLayout->addWidget(separator);
    auto* diskStats = new QVBoxLayout();
    diskStats->setSpacing(2);
    volumeLabel_ = heading(tr("Drive capacity"), summary);
    volumeLabel_->setObjectName(QStringLiteral("volumeTitle"));
    diskStats->addWidget(volumeLabel_);
    auto* capacity = new QHBoxLayout();
    capacity->setSpacing(8);
    totalLabel_ = new QLabel(summary);
    usedLabel_ = new QLabel(summary);
    freeLabel_ = new QLabel(summary);
    for (auto* label : {totalLabel_, usedLabel_, freeLabel_}) label->setProperty("summaryDetail", true);
    capacity->addWidget(totalLabel_);
    capacity->addWidget(usedLabel_);
    capacity->addWidget(freeLabel_);
    capacity->addStretch();
    diskStats->addLayout(capacity);
    volumeBar_ = new QProgressBar(summary);
    volumeBar_->setObjectName(QStringLiteral("volumeUsage"));
    volumeBar_->setRange(0, 1000);
    volumeBar_->setTextVisible(false);
    volumeBar_->setFixedHeight(4);
    diskStats->addWidget(volumeBar_);
    summaryLayout->addLayout(diskStats, 1);
    vbox->addWidget(summary);
    connect(diskSourceBtn_, &QPushButton::clicked, this, [this] { setScanSource(false); });
    connect(folderSourceBtn_, &QPushButton::clicked, this, [this] { setScanSource(true); });
    connect(browseBtn_, &QPushButton::clicked, this, &MainWindow::chooseFolder);
    connect(scanPathEdit_, &QLineEdit::returnPressed, this, &MainWindow::onScanClicked);
    setScanSource(false);

    filterBar_ = new QFrame(central);
    filterBar_->setFrameShape(QFrame::StyledPanel);
    auto* fl = new QHBoxLayout(filterBar_);
    fl->setContentsMargins(4, 2, 4, 2);
    fl->addWidget(new QLabel(tr("Include:"), filterBar_));
    filterInclude_ = new QLineEdit(filterBar_);
    filterInclude_->setObjectName(QStringLiteral("filterInclude"));
    filterInclude_->setPlaceholderText(tr("substring"));
    fl->addWidget(filterInclude_, 1);
    fl->addWidget(new QLabel(tr("Exclude:"), filterBar_));
    filterExclude_ = new QLineEdit(filterBar_);
    filterExclude_->setObjectName(QStringLiteral("filterExclude"));
    filterExclude_->setPlaceholderText(tr("substring"));
    fl->addWidget(filterExclude_, 1);
    auto* rfName = new QRadioButton(tr("Match file name only"), filterBar_);
    filterPathRadio_ = new QRadioButton(tr("Match entire path"), filterBar_);
    rfName->setChecked(true);
    fl->addWidget(rfName);
    fl->addWidget(filterPathRadio_);
    auto* applyFilterBtn = new QPushButton(tr("Apply"), filterBar_);
    auto* clearFilterBtn = new QPushButton(tr("Clear"), filterBar_);
    fl->addWidget(applyFilterBtn);
    fl->addWidget(clearFilterBtn);
    filterBar_->setVisible(false);
    vbox->addWidget(filterBar_);
    connect(applyFilterBtn, &QPushButton::clicked, this, &MainWindow::applyFilter);
    connect(clearFilterBtn, &QPushButton::clicked, this, &MainWindow::clearFilter);
    connect(filterInclude_, &QLineEdit::returnPressed, this, &MainWindow::applyFilter);
    connect(filterExclude_, &QLineEdit::returnPressed, this, &MainWindow::applyFilter);

    tabs_ = new QTabWidget(central);
    vbox->addWidget(tabs_, 1);
    auto* viewTools = new QWidget(tabs_);
    auto* viewToolsLayout = new QHBoxLayout(viewTools);
    viewToolsLayout->setContentsMargins(0, 0, 0, 0);
    viewToolsLayout->setSpacing(4);
    viewToolsLayout->addWidget(new QLabel(tr("Size metric:"), viewTools));
    logicalMetricBtn_ = new QPushButton(tr("File size"), viewTools);
    allocatedMetricBtn_ = new QPushButton(tr("Space on disk"), viewTools);
    logicalMetricBtn_->setObjectName(QStringLiteral("logicalMetric"));
    allocatedMetricBtn_->setObjectName(QStringLiteral("allocatedMetric"));
    auto* metricGroup = new QButtonGroup(viewTools);
    for (auto* button : {logicalMetricBtn_, allocatedMetricBtn_}) {
        button->setCheckable(true);
        button->setProperty("segment", true);
        metricGroup->addButton(button);
        viewToolsLayout->addWidget(button);
    }
    allocatedMetricBtn_->setChecked(true);
    connect(logicalMetricBtn_, &QPushButton::clicked, this, [this] { setSizeMetric(false); });
    connect(allocatedMetricBtn_, &QPushButton::clicked, this, [this] { setSizeMetric(true); });
    auto* metricHelp = new QToolButton(viewTools);
    metricHelp->setIcon(style()->standardIcon(QStyle::SP_MessageBoxQuestion));
    metricHelp->setAccessibleName(tr("About size metrics"));
    metricHelp->setToolTip(tr("Lists and treemap use the same size metric. Percentages in lists are relative to the displayed scan; zooming the treemap only changes the area being viewed."));
    viewToolsLayout->addWidget(metricHelp);
    auto* typesButton = new QPushButton(tr("File types"), viewTools);
    typesButton->setToolTip(tr("File types for the selected item; percentages use the scan range."));
    typesButton->setCheckable(true);
    viewToolsLayout->addWidget(typesButton);
    tabs_->setCornerWidget(viewTools, Qt::TopRightCorner);

    // ---------------- tree page
    auto* treePage = new QWidget(tabs_);
    auto* treeLayout = new QVBoxLayout(treePage);
    treeLayout->setContentsMargins(0, 0, 0, 0);
    treeLayout->setSpacing(0);
    treeSplit_ = new QSplitter(Qt::Horizontal, treePage);
    treeSplit_->setObjectName(QStringLiteral("analysisSplit"));
    treeSplit_->setChildrenCollapsible(false);
    auto* directoryPanel = new QFrame(treeSplit_);
    directoryPanel->setProperty("panel", true);
    auto* directoryLayout = new QVBoxLayout(directoryPanel);
    directoryLayout->setContentsMargins(6, 4, 6, 4);
    directoryLayout->setSpacing(4);
    auto* directoryHeading = new QHBoxLayout();
    directoryHeading->addWidget(heading(tr("Directory usage"), directoryPanel));
    auto* directorySearch = new QLineEdit(directoryPanel);
    directorySearch->setObjectName(QStringLiteral("directorySearch"));
    directorySearch->setPlaceholderText(tr("Filter files or folders"));
    directorySearch->setClearButtonEnabled(true);
    directorySearch->setMaximumWidth(250);
    directoryHeading->addWidget(directorySearch, 1);
    directoryLayout->addLayout(directoryHeading);
    auto* filterTimer = new QTimer(this);
    filterTimer->setSingleShot(true);
    filterTimer->setInterval(250);
    connect(directorySearch, &QLineEdit::textEdited, this, [this, filterTimer](const QString& text) {
        filterInclude_->setText(text);
        filterTimer->start();
    });
    connect(filterInclude_, &QLineEdit::textChanged, directorySearch, &QLineEdit::setText);
    connect(filterTimer, &QTimer::timeout, this, &MainWindow::applyFilter);
    emptyHint_ = new QLabel(tr("Choose a drive or folder, then start a scan."), directoryPanel);
    emptyHint_->setObjectName(QStringLiteral("emptyScanHint"));
    emptyHint_->setWordWrap(true);
    emptyHint_->setAlignment(Qt::AlignCenter);
    emptyHint_->setProperty("muted", true);
    directoryLayout->addWidget(emptyHint_);
    treeView_ = new QTreeView(directoryPanel);
    directoryLayout->addWidget(treeView_, 1);
    treeView_->setUniformRowHeights(true);
    treeView_->setIconSize(QSize(16, 16));
    treeView_->setAlternatingRowColors(true);
    treeView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    treeView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    treeView_->header()->setStretchLastSection(false);
    treeView_->header()->setSectionsClickable(true);
    treeView_->setObjectName(QStringLiteral("treeView"));
    auto* treemapPanel = new QFrame(treeSplit_);
    treemapPanel->setProperty("panel", true);
    treemap_ = new TreemapWidget(treemapPanel);
    treemap_->setMinimumHeight(90);
    auto* tpLayout = new QVBoxLayout(treemapPanel);
    tpLayout->setContentsMargins(6, 4, 6, 4);
    tpLayout->setSpacing(4);
    auto* chartHeading = new QHBoxLayout();
    chartHeading->addWidget(heading(tr("Space distribution"), treemapPanel));
    treemapSummary_ = new QLabel(treemapPanel);
    treemapSummary_->setObjectName(QStringLiteral("treemapSummary"));
    treemapSummary_->setProperty("muted", true);
    chartHeading->addWidget(treemapSummary_, 1);
    tpLayout->addLayout(chartHeading);
    auto* tpBar = new QHBoxLayout();
    auto* backBtn = new QPushButton(style()->standardIcon(QStyle::SP_ArrowBack), {}, treemapPanel);
    auto* forwardBtn = new QPushButton(style()->standardIcon(QStyle::SP_ArrowForward), {}, treemapPanel);
    auto* homeBtn = new QPushButton(style()->standardIcon(QStyle::SP_DirHomeIcon), {}, treemapPanel);
    backBtn->setFixedWidth(32);
    forwardBtn->setFixedWidth(32);
    homeBtn->setFixedWidth(32);
    backBtn->setToolTip(tr("Back (Backspace)"));
    forwardBtn->setToolTip(tr("Forward (Shift+Backspace)"));
    homeBtn->setToolTip(tr("Scan root (Ctrl+Home)"));
    auto* upBtn = new QPushButton(style()->standardIcon(QStyle::SP_ArrowUp), {}, treemapPanel);
    upBtn->setFixedWidth(32);
    upBtn->setToolTip(tr("Up one level"));
    for (auto* button : {backBtn, forwardBtn, upBtn, homeBtn})
    {
        button->setAccessibleName(button->toolTip());
        button->setProperty("iconOnly", true);
        button->setFixedSize(26, 26);
    }
    treemapPath_ = new QLabel(treemapPanel);
    treemapPath_->setMinimumWidth(40);
    treemapPath_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    tpBar->addWidget(backBtn);
    tpBar->addWidget(forwardBtn);
    tpBar->addWidget(upBtn);
    tpBar->addWidget(homeBtn);
    tpBar->addWidget(treemapPath_, 1);
    auto* detail = new QSpinBox(treemapPanel);
    detail->setObjectName(QStringLiteral("treemapDetail"));
    detail->setRange(1, 15);
    detail->setValue(settings_.value(QStringLiteral("treemapDetail"), 3).toInt());
    detail->setPrefix(tr("Detail: "));
    detail->setToolTip(tr("Visible folder levels (Ctrl +/-)"));
    treemap_->setDetailLevel(detail->value());
    auto* chartControls = new QHBoxLayout();
    chartControls->addWidget(detail);
    auto* classes = new QCheckBox(tr("File classes"), treemapPanel);
    classes->setChecked(settings_.value(QStringLiteral("treemapClasses"), false).toBool());
    treemap_->setFileClassesStyle(classes->isChecked());
    chartControls->addWidget(classes);
    chartControls->addStretch();
    connect(detail, &QSpinBox::valueChanged, treemap_, &TreemapWidget::setDetailLevel);
    connect(treemap_, &TreemapWidget::detailLevelChanged, detail, &QSpinBox::setValue);
    connect(treemap_, &TreemapWidget::detailLevelChanged, this, [this](int level) {
        settings_.setValue(QStringLiteral("treemapDetail"), level);
    });
    connect(classes, &QCheckBox::toggled, treemap_, &TreemapWidget::setFileClassesStyle);
    connect(treemap_, &TreemapWidget::fileClassesStyleChanged, classes, &QCheckBox::setChecked);
    connect(treemap_, &TreemapWidget::fileClassesStyleChanged, this, [this](bool on) {
        settings_.setValue(QStringLiteral("treemapClasses"), on);
    });
    tpLayout->addLayout(tpBar);
    tpLayout->addWidget(treemap_, 1);
    tpLayout->addLayout(chartControls);
    connect(upBtn, &QPushButton::clicked, this, &MainWindow::treemapUp);
    connect(backBtn, &QPushButton::clicked, treemap_, &TreemapWidget::goBack);
    connect(forwardBtn, &QPushButton::clicked, treemap_, &TreemapWidget::goForward);
    connect(homeBtn, &QPushButton::clicked, treemap_, &TreemapWidget::goHome);
    const auto updateNavigation = [this, backBtn, forwardBtn, upBtn, homeBtn] {
        backBtn->setEnabled(treemap_->canGoBack());
        forwardBtn->setEnabled(treemap_->canGoForward());
        upBtn->setEnabled(treemap_->canGoUp());
        homeBtn->setEnabled(treemap_->canGoUp());
    };
    connect(treemap_, &TreemapWidget::navigationChanged, this, updateNavigation);
    updateNavigation();
    treemap_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(treemap_, &TreemapWidget::customContextMenuRequested, this,
            &MainWindow::showTreemapMenu);
    treeSplit_->setStretchFactor(0, 1);
    treeSplit_->setStretchFactor(1, 1);
    treeSplit_->setSizes({700, 600});
    typesDock_ = new QDockWidget(tr("File types"), this);
    typesDock_->setObjectName(QStringLiteral("fileTypesDock"));
    typesDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);
    extView_ = new QTableView(typesDock_);
    extView_->setObjectName(QStringLiteral("extensionView"));
    extView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    extView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    extView_->setAlternatingRowColors(true);
    extView_->verticalHeader()->setVisible(false);
    extView_->horizontalHeader()->setStretchLastSection(true);
    typesDock_->setWidget(extView_);
    addDockWidget(Qt::BottomDockWidgetArea, typesDock_);
    typesDock_->hide();
    connect(typesButton, &QPushButton::toggled, typesDock_, &QDockWidget::setVisible);
    connect(typesDock_, &QDockWidget::visibilityChanged, typesButton, &QPushButton::setChecked);
    treeLayout->addWidget(treeSplit_);
    tabs_->addTab(treePage, tr("Space analysis"));

    // ---------------- files page
    auto* filesPage = new QWidget(tabs_);
    auto* filesLayout = new QVBoxLayout(filesPage);
    filesLayout->setContentsMargins(6, 4, 6, 4);
    filesLayout->setSpacing(4);
    auto* searchRow = new QHBoxLayout();
    searchRow->addWidget(new QLabel(tr("Filter:"), filesPage));
    searchEdit_ = new QLineEdit(filesPage);
    searchEdit_->setPlaceholderText(tr("Match file name"));
    searchRow->addWidget(searchEdit_, 1);
    showFoldersBox_ = new QCheckBox(tr("Folders"), filesPage);
    searchRow->addWidget(showFoldersBox_);
    searchRow->addWidget(new QLabel(tr("Max files:"), filesPage));
    maxFilesBox_ = new QComboBox(filesPage);
    maxFilesBox_->addItem(QStringLiteral("100"), 100);
    maxFilesBox_->addItem(QStringLiteral("1000"), 1000);
    maxFilesBox_->addItem(QStringLiteral("10000"), 10000);
    maxFilesBox_->addItem(QStringLiteral("50000"), 50000);
    maxFilesBox_->addItem(tr("ALL"), 0);
    maxFilesBox_->setCurrentIndex(1);
    searchRow->addWidget(maxFilesBox_);
    dupsBox_ = new QComboBox(filesPage);
    dupsBox_->addItem(tr("Don't locate duplicates"), 0);
    dupsBox_->addItem(tr("Locate by Name, Size"), 1);
    dupsBox_->addItem(tr("Locate by Name, Size, Date"), 2);
    searchRow->addWidget(dupsBox_);
    dupsOnlyBox_ = new QCheckBox(tr("Duplicates only"), filesPage);
    searchRow->addWidget(dupsOnlyBox_);
    filesLayout->addLayout(searchRow);
    fileView_ = new QTableView(filesPage);
    fileView_->setObjectName(QStringLiteral("fileView"));
    fileView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    fileView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fileView_->setAlternatingRowColors(true);
    fileView_->verticalHeader()->setVisible(false);
    fileView_->horizontalHeader()->setStretchLastSection(true);
    fileView_->setSortingEnabled(true);
    filesLayout->addWidget(fileView_, 1);
    tabs_->addTab(filesPage, tr("File View"));

    // ---------------- about page
    auto* aboutPage = new QWidget(tabs_);
    auto* aboutLayout = new QVBoxLayout(aboutPage);
    auto* browser = new QTextBrowser(aboutPage);
    browser->setHtml(QStringLiteral(
        "<h2>DiskCleaner 0.1</h2>"
        "<p>A disk space analyzer built with C++20 and Qt 6.</p>"
        "<p><b>Fast path:</b> reads the NTFS Master File Table ($MFT) directly when running as "
        "Administrator on an NTFS volume.</p>"
        "<p><b>Fallback:</b> recursive directory enumeration for folders, network shares and "
        "non-NTFS volumes.</p>"
        "<p>This application is free for personal use.</p>"));
    aboutLayout->addWidget(browser);
    tabs_->addTab(aboutPage, tr("About"));

    auto* selectionBar = new QFrame(central);
    selectionBar->setObjectName(QStringLiteral("selectionBar"));
    auto* selectionLayout = new QHBoxLayout(selectionBar);
    selectionLayout->setContentsMargins(0, 0, 0, 0);
    selectionLayout->setSpacing(4);
    pathLabel_ = new QLabel(selectionBar);
    pathLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    selectionLayout->addWidget(pathLabel_, 1);
    exploreBtn_ = new QPushButton(style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Explore Folder"), selectionBar);
    copyPathBtn_ = new QPushButton(style()->standardIcon(QStyle::SP_FileDialogContentsView), tr("Copy Path"), selectionBar);
    selectionLayout->addWidget(exploreBtn_);
    selectionLayout->addWidget(copyPathBtn_);
    const auto withSelection = [this](const std::function<void()>& action) {
        QScopedValueRollback<QWidget*> guard(menuSource_, tabs_->currentIndex() == 1
                                                           ? static_cast<QWidget*>(fileView_)
                                                           : static_cast<QWidget*>(treeView_));
        action();
    };
    connect(exploreBtn_, &QPushButton::clicked, this, [this, withSelection] {
        withSelection([this] { exploreSelection(); });
    });
    connect(copyPathBtn_, &QPushButton::clicked, this, [this, withSelection] {
        withSelection([this] { copyPathToClipboard(); });
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] { updateSelectionInfo(); });

    setCentralWidget(central);
    setAcceptDrops(true);

    treeModel_ = new TreeModel(this);
    fileModel_ = new FileListModel(this);
    extModel_ = new ExtensionModel(this);
    treeView_->setModel(treeModel_);
    treeView_->setContextMenuPolicy(Qt::CustomContextMenu);
    treeView_->setDragEnabled(true);
    treeView_->setDragDropMode(QAbstractItemView::DragOnly);
    treeView_->setDefaultDropAction(Qt::CopyAction);
    treeView_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    connect(treeView_, &QTreeView::customContextMenuRequested, this, &MainWindow::showTreeContextMenu);
    auto* delShortcut = new QShortcut(QKeySequence::Delete, treeView_);
    connect(delShortcut, &QShortcut::activated, this, &MainWindow::deleteSelectionToRecycleBin);
    auto* copyShortcut = new QShortcut(QKeySequence::Copy, treeView_);
    connect(copyShortcut, &QShortcut::activated, this, [this] { copyFilesToClipboard(false); });
    auto* cutShortcut = new QShortcut(QKeySequence::Cut, treeView_);
    connect(cutShortcut, &QShortcut::activated, this, [this] { copyFilesToClipboard(true); });
    auto* copyPathShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")), treeView_);
    connect(copyPathShortcut, &QShortcut::activated, this, &MainWindow::copyPathToClipboard);
    auto* openShortcut = new QShortcut(QKeySequence(Qt::Key_Return), treeView_);
    connect(openShortcut, &QShortcut::activated, this, &MainWindow::openSelection);
    fileView_->setModel(fileModel_);
    fileView_->sortByColumn(3, Qt::DescendingOrder);
    fileView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    fileView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(fileView_, &QTableView::customContextMenuRequested, this, &MainWindow::showFileContextMenu);
    extView_->setModel(extModel_);
    {
        const int widths[] = {270, 158, 124, 124, 80, 88, 88, 160, 96};
        for (int i = 0; i < 9; ++i) treeView_->setColumnWidth(i, widths[i]);
        const int fw[] = {260, 310, 110, 110, 135, 150, 80, 100, 140};
        for (int i = 0; i < 9; ++i) fileView_->setColumnWidth(i, fw[i]);
        const int ew[] = {90, 160, 100, 110, 110, 70};
        for (int i = 0; i < 6; ++i) extView_->setColumnWidth(i, ew[i]);
    }
    treeView_->header()->moveSection(treeView_->header()->visualIndex(3), 1);
    treeView_->header()->moveSection(treeView_->header()->visualIndex(1), 2);
    treeView_->header()->moveSection(treeView_->header()->visualIndex(5), 3);
    for (int column : {2, 4, 6, 7, 8}) treeView_->setColumnHidden(column, true);
    fileView_->setColumnHidden(2, true);
    fileView_->setColumnHidden(6, true);
    for (int column : {7, 8}) fileView_->setColumnHidden(column, true);
    treeView_->setItemDelegateForColumn(1, new PercentageDelegate(treeView_));
    treeView_->header()->setMinimumSectionSize(60);
    treeView_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    fileView_->horizontalHeader()->setStretchLastSection(false);
    fileView_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (auto* view : {static_cast<QAbstractItemView*>(treeView_),
                       static_cast<QAbstractItemView*>(fileView_),
                       static_cast<QAbstractItemView*>(extView_)}) {
        if (auto* table = qobject_cast<QTableView*>(view)) {
            table->setShowGrid(false);
            table->verticalHeader()->setDefaultSectionSize(26);
        }
        auto* header = view == treeView_ ? treeView_->header()
                                         : qobject_cast<QTableView*>(view)->horizontalHeader();
        header->setContextMenuPolicy(Qt::CustomContextMenu);
        header->setToolTip(tr("Right-click to choose columns"));
        connect(header, &QHeaderView::customContextMenuRequested, this, [view, header](const QPoint& pos) {
            QMenu menu(header);
            for (int col = 1; col < view->model()->columnCount(); ++col) {
                auto* action = menu.addAction(view->model()->headerData(col, Qt::Horizontal).toString());
                action->setCheckable(true);
                action->setChecked(!header->isSectionHidden(col));
                QObject::connect(action, &QAction::toggled, header, [header, col](bool checked) {
                    header->setSectionHidden(col, !checked);
                });
            }
            menu.exec(header->viewport()->mapToGlobal(pos));
        });
    }
    statusLabel_ = new QLabel(tr("Ready"), this);
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addWidget(statusLabel_, 1);
    statusBar()->addWidget(selectionBar, 3);
    statusBar()->setFixedHeight(26);

    searchTimer_ = new QTimer(this);
    searchTimer_->setSingleShot(true);
    searchTimer_->setInterval(250);
    extTimer_ = new QTimer(this);
    extTimer_->setSingleShot(true);
    extTimer_->setInterval(120);
    connect(extTimer_, &QTimer::timeout, this, &MainWindow::refreshExtensions);

    connect(scanBtn_, &QPushButton::clicked, this, &MainWindow::onScanClicked);
    connect(cancelBtn_, &QPushButton::clicked, this, &MainWindow::onCancelClicked);
    connect(driveBox_, &QComboBox::currentIndexChanged, this, &MainWindow::onDriveChanged);
    connect(searchTimer_, &QTimer::timeout, this, &MainWindow::onSearchChanged);
    connect(searchEdit_, &QLineEdit::textChanged, this, [this] { searchTimer_->start(); });
    connect(showFoldersBox_, &QCheckBox::toggled, this, [this] { searchTimer_->start(); });
    connect(maxFilesBox_, &QComboBox::currentIndexChanged, this, [this] { searchTimer_->start(); });
    connect(dupsBox_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        fileModel_->setDupMode(dupsBox_->itemData(idx).toInt());
        for (int col : {7, 8}) fileView_->setColumnHidden(col, idx == 0);
        settings_.setValue(QStringLiteral("dupMode"), dupsBox_->itemData(idx).toInt());
        searchTimer_->start();
    });
    connect(dupsOnlyBox_, &QCheckBox::toggled, this, [this](bool on) {
        settings_.setValue(QStringLiteral("dupOnly"), on);
        searchTimer_->start();
    });
    connect(treeView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { onTreeSelectionChanged(); });
    connect(fileView_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateSelectionInfo(); });
    connect(treeView_->header(), &QHeaderView::sectionClicked, this,
            &MainWindow::onTreeHeaderClicked);
    connect(extView_, &QTableView::doubleClicked, this, [this](const QModelIndex& idx) {
        if (!idx.isValid()) return;
        const QString ext = extModel_->extensionAt(idx.row());
        if (ext.isEmpty() || ext == QLatin1String("(No Extension)")) return;
        tabs_->setCurrentIndex(1);
        searchEdit_->setText(QLatin1Char('.') + ext);
        searchTimer_->start();
    });

    connect(treeModel_, &TreeModel::renameFailed, this, [this](const QString& err) {
        statusLabel_->setText(tr("Rename failed: %1").arg(err));
    });
    connect(treeModel_, &TreeModel::nodeRenamed, this, [this](uint32_t) {
        fileModel_->setTree(displayTree());
        onSearchChanged();
        onTreeSelectionChanged();
        treemap_->refresh();
    });

    connect(treeView_, &QTreeView::doubleClicked, this, [this](const QModelIndex& idx) {
        if (!idx.isValid()) return;
        wt::Tree* tree = displayTree();
        if (!tree) return;
        const uint32_t n = treeModel_->nodeOf(idx);
        if (n == wt::kNone || n >= tree->nodes.size()) return;
        if (tree->nodes[n].isDir) return;
        const QString path = tree->path(n);
        ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, nullptr,
                      SW_SHOWNORMAL);
    });

    connect(treemap_, &TreemapWidget::nodeClicked, this, &MainWindow::selectTreemapNode);
    connect(treemap_, &TreemapWidget::rootChanged, this, [this](uint32_t node) {
        if (!syncingTreemapSelection_) selectTreemapNode(node);
        updateTreemapBreadcrumb();
    });

    treeModel_->setTree(nullptr);
    fileModel_->setTree(nullptr);
}

void MainWindow::buildMenus()
{
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* scanAct = fileMenu->addAction(tr("&Scan"));
    scanAct->setShortcut(QKeySequence(Qt::Key_F5));
    connect(scanAct, &QAction::triggered, this, &MainWindow::onScanClicked);
    auto* openCsvAct = fileMenu->addAction(tr("Select CSV file..."));
    connect(openCsvAct, &QAction::triggered, this, &MainWindow::openCsvFile);
    auto* openMftAct = fileMenu->addAction(tr("Open MFT Dump..."));
    connect(openMftAct, &QAction::triggered, this, &MainWindow::openMftDump);
    auto* dumpMftAct = fileMenu->addAction(tr("Dump MFT File..."));
    connect(dumpMftAct, &QAction::triggered, this, &MainWindow::dumpMftFile);
    auto* multiAct = fileMenu->addAction(tr("Select Multiple..."));
    connect(multiAct, &QAction::triggered, this, &MainWindow::selectMultiple);
    auto* folderAct = fileMenu->addAction(tr("Select &Folder..."));
    folderAct->setShortcut(QKeySequence(Qt::Key_F3));
    connect(folderAct, &QAction::triggered, this, &MainWindow::chooseFolder);
    fileMenu->addSeparator();
    auto* exportAct = fileMenu->addAction(tr("&Export to CSV file..."));
    exportAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
    connect(exportAct, &QAction::triggered, this, &MainWindow::exportCsv);
    auto* treemapAct = fileMenu->addAction(tr("Save Treemap Image as .png file..."));
    treemapAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+S")));
    connect(treemapAct, &QAction::triggered, this, &MainWindow::exportTreemapPng);
    auto* copyInfoAct = fileMenu->addAction(tr("Copy file and size info to clipboard"));
    copyInfoAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    connect(copyInfoAct, &QAction::triggered, this, &MainWindow::copyTreeInfoToClipboard);
    auto* exportTypesAct = fileMenu->addAction(tr("Export File &Types to CSV..."));
    connect(exportTypesAct, &QAction::triggered, this, &MainWindow::exportFileTypesCsv);
    fileMenu->addSeparator();
    auto* exitAct = fileMenu->addAction(tr("E&xit"));
    exitAct->setShortcut(QKeySequence(QStringLiteral("Alt+F4")));
    connect(exitAct, &QAction::triggered, this, &QWidget::close);

    auto* optionsMenu = menuBar()->addMenu(tr("&Options"));
    auto* unitGroup = new QActionGroup(this);
    unitGroup->setExclusive(true);
    struct UnitDef { const char* label; int mode; };
    const UnitDef units[] = {{"Display Dynamic values", 0}, {"Display Byte values", 1},
                             {"Display KB values", 2},      {"Display MB values", 3},
                             {"Display GB values", 4},      {"Display TB values", 5}};
    for (const auto& u : units) {
        auto* a = optionsMenu->addAction(QString::fromLatin1(u.label));
        a->setCheckable(true);
        a->setData(u.mode);
        unitGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, mode = u.mode] { setUnit(mode); });
    }
    optionsMenu->addSeparator();
    auto* shellAct = optionsMenu->addAction(tr("Windows Explorer context menu"));
    shellAct->setCheckable(true);
    shellAct->setChecked(
        settings_.value(QStringLiteral("actWindowsContextMenu_Checked"),
                        wt::isShellIntegrationInstalled())
            .toBool());
    connect(shellAct, &QAction::toggled, this, [this](bool on) {
        if (on) {
            QString err;
            if (!wt::installShellIntegration(QCoreApplication::applicationFilePath(), err))
                statusLabel_->setText(tr("Failed to update Windows Explorer context menu"));
        } else {
            wt::removeShellIntegration();
        }
        settings_.setValue(QStringLiteral("actWindowsContextMenu_Checked"), on);
    });

    auto* updateAct = optionsMenu->addAction(tr("Check for updates now"));
    connect(updateAct, &QAction::triggered, this, &MainWindow::checkForUpdatesNow);

    auto* themeMenu = optionsMenu->addMenu(tr("UI Theme"));
    auto* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    const QString currentTheme =
        qApp->property("theme").isValid() ? qApp->property("theme").toString()
                                        : settings_.value(QStringLiteral("theme"), QStringLiteral("light")).toString();
    const struct {
        const char* label;
        const char* code;
    } themes[] = {{"Auto", "auto"}, {"Light", "light"}, {"Dark", "dark"}};
    for (const auto& t : themes) {
        auto* a = themeMenu->addAction(tr(t.label));
        a->setCheckable(true);
        a->setChecked(currentTheme == QLatin1String(t.code));
        themeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, code = QString::fromLatin1(t.code)] {
            settings_.setValue(QStringLiteral("theme"), code);
            applyAppTheme(code);
        });
    }

    filterAction_ = optionsMenu->addAction(tr("Filter Scan Results"));
    filterAction_->setCheckable(true);
    filterAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    connect(filterAction_, &QAction::toggled, this, &MainWindow::toggleFilterPanel);

    auto* namesAct = optionsMenu->addAction(tr("Show Folder and File Names on Treemap"));    namesAct->setCheckable(true);
    namesAct->setChecked(settings_.value(QStringLiteral("treemapNames"), true).toBool());
    connect(namesAct, &QAction::toggled, this, [this](bool on) {
        if (treemap_) treemap_->setShowNames(on);
        settings_.setValue(QStringLiteral("treemapNames"), on);
    });
    allocatedAction_ = optionsMenu->addAction(tr("Use space on disk in all views"));
    allocatedAction_->setCheckable(true);
    allocatedAction_->setChecked(settings_.value(QStringLiteral("treemapAllocated"), true).toBool());
    connect(allocatedAction_, &QAction::toggled, this, &MainWindow::setSizeMetric);
    optionsMenu->addAction(typesDock_->toggleViewAction());

    auto* freeAct = optionsMenu->addAction(tr("Show Free Space on Treemap"));
    freeAct->setCheckable(true);
    freeAct->setToolTip(tr("Capacity overlays are available for full drives in space-on-disk mode."));
    freeAct->setChecked(settings_.value(QStringLiteral("treemapFree"), false).toBool());
    connect(freeAct, &QAction::toggled, this, [this](bool on) {
        showFreeSpace_ = on;
        if (treemap_) treemap_->setShowFreeSpace(on);
        settings_.setValue(QStringLiteral("treemapFree"), on);
    });

    auto* langMenu = optionsMenu->addMenu(tr("Language"));
    auto* langGroup = new QActionGroup(this);
    langGroup->setExclusive(true);
    const QString currentLang =
        qApp->property("language").isValid() ? qApp->property("language").toString()
                                           : settings_.value(QStringLiteral("language"), QStringLiteral("en")).toString();
    auto* enAct = langMenu->addAction(tr("English"));
    enAct->setCheckable(true);
    enAct->setChecked(!currentLang.startsWith(QLatin1String("zh")));
    langGroup->addAction(enAct);
    connect(enAct, &QAction::triggered, this, [this] { setLanguage(QStringLiteral("en")); });
    auto* zhAct = langMenu->addAction(tr("Chinese (Simplified)"));
    zhAct->setCheckable(true);
    zhAct->setChecked(currentLang.startsWith(QLatin1String("zh")));
    langGroup->addAction(zhAct);
    connect(zhAct, &QAction::triggered, this, [this] { setLanguage(QStringLiteral("zh_CN")); });

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    auto* aboutAct = helpMenu->addAction(tr("&About..."));
    connect(aboutAct, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::refreshDrives()
{
    driveBox_->blockSignals(true);
    driveBox_->clear();
    driveBox_->addItem(tr("<Select folder...>"), -1);
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t letter = wchar_t(L'A' + i);
        wchar_t root[8];
        swprintf_s(root, L"%c:\\", letter);
        const UINT type = GetDriveTypeW(root);
        if (type == DRIVE_NO_ROOT_DIR) continue;
        const QString label = driveLabel(letter);
        QString text = QStringLiteral("%1: [%2]").arg(QChar(letter)).arg(label);
        driveBox_->addItem(text, int(letter));
    }
    driveBox_->blockSignals(false);
    driveBox_->setCurrentIndex(driveBox_->count() > 1 ? 1 : 0);
}

void MainWindow::onDriveChanged(int index)
{
    if (index <= 0) {
        if (scanPathEdit_) setScanSource(true);
        return;
    }
    const wchar_t letter = wchar_t(driveBox_->itemData(index).toInt());
    if (!letter) return;
    wchar_t root[8];
    swprintf_s(root, L"%c:\\", letter);
    Q_UNUSED(root);
}

void MainWindow::scanPathFromCli(const QString& path)
{
    if (path.contains(QLatin1Char(';'))) {
        QStringList parts = path.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (QString& part : parts) part = part.trimmed();
        parts.removeAll(QString());
        if (parts.size() > 1) {
            startMultiScan(parts);
            return;
        }
        if (parts.size() == 1) { scanPathFromCli(parts.first()); return; }
        statusLabel_->setText(tr("Choose an existing folder to scan."));
        return;
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("mft") || suffix == QLatin1String("bin")) {
        startScanPath(path, 0, 2);
        return;
    }
    if (suffix == QLatin1String("csv") || suffix == QLatin1String("tsv")) {
        startScanPath(path, 0, 3);
        return;
    }
    startScan(path, 0, false);
}

void MainWindow::setTabIndex(int index)
{
    if (tabs_ && index >= 0 && index < tabs_->count()) tabs_->setCurrentIndex(index);
}

void MainWindow::sortFiles(int column, bool descending)
{
    if (fileView_)
        fileView_->sortByColumn(column, descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void MainWindow::configureDuplicates(int mode, bool only)
{
    if (dupsBox_) dupsBox_->setCurrentIndex(qBound(0, mode, 2));
    if (dupsOnlyBox_) dupsOnlyBox_->setChecked(only);
}

void MainWindow::onScanClicked()
{
    if (scanning_) return;
    if (folderSource_) {
        const QString path = QDir::fromNativeSeparators(scanPathEdit_->text().trimmed());
        if (path.isEmpty()) { chooseFolder(); return; }
        if (path.contains(QLatin1Char(';'))) { scanPathFromCli(path); return; }
        if (!QFileInfo(path).isDir()) {
            statusLabel_->setText(tr("Choose an existing folder to scan."));
            scanPathEdit_->setFocus();
            scanPathEdit_->selectAll();
            return;
        }
        startScan(QDir::cleanPath(path), 0, false);
        return;
    }
    const int index = driveBox_->currentIndex();
    if (index <= 0) {
        chooseFolder();
        return;
    }
    const wchar_t letter = wchar_t(driveBox_->itemData(index).toInt());
    wchar_t root[8];
    swprintf_s(root, L"%c:\\", letter);
    startScan(QString::fromWCharArray(root), letter, true);
}

void MainWindow::chooseFolder()
{
    if (scanning_) return;
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select Folder"),
                                                          scanPathEdit_->text().isEmpty()
                                                              ? QDir::homePath() : scanPathEdit_->text());
    if (dir.isEmpty()) return;
    setScanSource(true);
    scanPathEdit_->setText(QDir::toNativeSeparators(dir));
    scanBtn_->setFocus();
}

void MainWindow::startScan(const QString& target, wchar_t letter, bool isDrive)
{
    startScanPath(target, letter, isDrive ? 1 : 0);
}

void MainWindow::startScanPath(const QString& target, wchar_t letter, int mode)
{
    if (scanning_ || dumping_ || closing_) return;
    if (liveBusy_) {
        liveCancel_ = true;
        pendingScan_ = [this, target, letter, mode] { startScanPath(target, letter, mode); };
        return;
    }
    if (worker_.joinable()) worker_.join();

    if (mode == 0) {
        setScanSource(true);
        scanPathEdit_->setText(QDir::toNativeSeparators(target));
    } else if (mode == 1) {
        setScanSource(false);
        const int index = driveBox_->findData(int(letter));
        if (index >= 0) driveBox_->setCurrentIndex(index);
    }
    watchScanMode_ = mode;
    watchDriveLetter_ = letter;
    beginDeletionWatch(mode == 0 || mode == 1 ? QStringList{target} : QStringList{});

    scanning_ = true;
    cancel_ = false;
    currentTarget_ = target;
    displayingPreviousScan_ = bool(result_);
    setBusy(true);
    statusLabel_->setText(tr("Scanning..."));
    pathLabel_->setText(target);

    wt::ScanOptions opt;
    opt.preferMft = (mode == 1);
    opt.unit = unit_;
    opt.cancel = &cancel_;
    opt.status = [this](const QString& s) {
        QMetaObject::invokeMethod(
            this, [this, s] { statusLabel_->setText(s); }, Qt::QueuedConnection);
    };

    worker_ = std::thread([this, target, letter, mode, opt]() mutable {
        wt::ScanResult r = wt::runScanTask([&] {
            if (mode == 1) return wt::scanDrive(letter, opt);
            if (mode == 2) return wt::scanMftDump(target, opt);
            if (mode == 3) return wt::importCsvTree(target, opt);
            return wt::scanFolder(target, opt);
        });
        if (cancel_.load()) { r.ok = false; r.error = QStringLiteral("canceled"); }
        auto shared = std::make_shared<wt::ScanResult>(std::move(r));
        QMetaObject::invokeMethod(
            this, [this, shared] { onScanFinished(shared); }, Qt::QueuedConnection);
    });
}

void MainWindow::onScanFinished(const std::shared_ptr<wt::ScanResult>& r)
{
    if (worker_.joinable()) worker_.join();
    scanning_ = false;
    if (closing_) { close(); return; }
    setBusy(false);

    if (!r || !r->ok || cancel_.load()) {
        deletionWatcher_->stop();
        statusLabel_->setText(tr("Scan failed"));
        scanStateLabel_->setText(cancel_.load() || (r && r->error == QLatin1String("canceled"))
                                    ? tr("Scan canceled") : tr("Scan failed"));
        updateStats();
        updateSelectionInfo();
        if (!cancel_.load() && r && !r->error.isEmpty() && r->error != QLatin1String("canceled") && !batchMode_)
            QMessageBox::warning(this, tr("Error"), r->error);
        return;
    }

    result_ = std::make_unique<wt::ScanResult>(std::move(*r));
    resultScanMode_ = watchScanMode_;
    displayingPreviousScan_ = false;
    scanStateLabel_->setText(tr("Scan complete"));
    expandedNodes_.clear();
    filteredTree_.reset();
    treeModel_->setTree(&result_->tree);
    fileModel_->setTree(&result_->tree);
    treemap_->setTree(&result_->tree, result_->tree.root);
    treemap_->clearTags();
    updateTreemapBreadcrumb();
    refreshExtensions();

    const QModelIndex rootIdx = treeModel_->index(0, 0, QModelIndex());
    treeView_->expand(rootIdx);
    treeView_->setCurrentIndex(rootIdx);

    setWindowTitle(QStringLiteral("[%1]  - DiskCleaner").arg(currentTarget_));
    {
        quint64 total = 0, freeB = 0;
        if ((resultScanMode_ == 0 || resultScanMode_ == 1) &&
            volumeUsage(result_->tree.path(result_->tree.root), total, freeB))
            treemap_->setFreeSpace(freeB, total);
    }
    treeModel_->sort(sortCol_, sortOrder_);
    updateStats();
    onSearchChanged();
    if (filterBar_ &&
        (!filterInclude_->text().trimmed().isEmpty() ||
         !filterExclude_->text().trimmed().isEmpty()))
        applyFilter();
    statusLabel_->setText(tr("Scan complete in %1 s (%2 records, %3 items)")
                              .arg(r->seconds, 0, 'f', 2)
                              .arg(result_->records)
                              .arg(result_->tree.nodes.size()));
    if (!watchError_.isEmpty()) statusLabel_->setText(watchError_);
    flushDeletions(); // also apply notifications received while the scan was running
    if (scanning_) return;
    if (onScanDone_) {
        auto cb = std::move(onScanDone_);
        onScanDone_ = nullptr;
        cb();
    }
}

void MainWindow::beginDeletionWatch(const QStringList& targets)
{
    stopLiveUpdates();
    deletionTimer_->stop();
    pendingDeletions_.clear();
    watchNeedsRescan_ = false;
    watchError_.clear();
    watchTargets_ = targets;
    deletionWatcher_->start(targets);
}

void MainWindow::stopLiveUpdates()
{
    ++liveGeneration_;
    liveCancel_ = true;
    if (liveWorker_.joinable()) liveWorker_.join();
    liveBusy_ = false;
    readyUpdate_.reset();
    pendingChanges_ = {};
    liveRetries_.clear();
}

void MainWindow::flushDeletions()
{
    if (closing_ || scanning_ || liveBusy_ || !result_) return;
    // Nested menu/dialog loops must not invalidate the nodes their actions refer to.
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget()) {
        deletionTimer_->start();
        return;
    }
    if (watchNeedsRescan_) {
        const auto targets = watchTargets_;
        watchNeedsRescan_ = false;
        if (targets.size() == 1 && watchScanMode_ >= 0)
            startScanPath(targets.front(), watchDriveLetter_, watchScanMode_);
        else if (!targets.isEmpty()) startMultiScan(targets);
        statusLabel_->setText(tr("Notifications overflowed; rescanning to recover changes..."));
        return;
    }
    if (readyUpdate_) {
        const auto update = std::move(readyUpdate_);
        applyLiveResult(*update);
        for (const auto& entry : update->entries)
            if (entry.state != wt::LiveEntry::Unavailable) liveRetries_.remove(wt::livePathKey(entry.path));
        for (const auto& path : update->retryPaths) {
            if (++liveRetries_[wt::livePathKey(path)] <= 6) pendingDeletions_.insert(path);
            else statusLabel_->setText(tr("Cannot refresh %1; waiting for the next file change").arg(path));
        }
        if (!pendingDeletions_.isEmpty() || !pendingChanges_.renames.isEmpty()) deletionTimer_->start(400);
        return;
    }
    if (pendingDeletions_.isEmpty() && pendingChanges_.renames.isEmpty()) return;
    wt::FileChanges changes{pendingDeletions_.values(), std::move(pendingChanges_.renames), std::move(pendingChanges_.createdPaths)};
    pendingDeletions_.clear(); pendingChanges_ = {};
    QStringList queries = changes.paths;
    for (const auto& rename : changes.renames) queries.append(rename.first);
    QSet<QString> knownDirectories;
    for (auto id : result_->tree.findPaths(queries))
        if (result_->tree.nodes[id].isDir) knownDirectories.insert(wt::livePathKey(result_->tree.path(id)));
    if (liveWorker_.joinable()) liveWorker_.join();
    const auto generation = liveGeneration_;
    liveCancel_ = false; liveBusy_ = true;
    liveWorker_ = std::thread([this, changes, knownDirectories, generation] {
        auto update = std::make_shared<wt::LiveUpdate>();
        const auto outcome = wt::runScanTask([&] {
            *update = wt::readLiveUpdate(changes, knownDirectories, liveCancel_);
            wt::ScanResult result; result.ok = true; return result;
        });
        QMetaObject::invokeMethod(this, [this, generation, update, error = outcome.error] {
            if (liveWorker_.joinable()) liveWorker_.join();
            liveBusy_ = false;
            if (closing_) { close(); return; }
            if (pendingScan_) {
                auto scan = std::move(pendingScan_);
                pendingScan_ = {};
                scan();
                return;
            }
            if (generation != liveGeneration_) return;
            if (!error.isEmpty()) { statusLabel_->setText(error); return; }
            readyUpdate_ = std::make_unique<wt::LiveUpdate>(std::move(*update));
            flushDeletions();
        }, Qt::QueuedConnection);
    });
}

void MainWindow::applyLiveResult(const wt::LiveUpdate& update)
{
    auto* shown = displayTree();
    QScopedValueRollback<bool> guard(syncingTreemapSelection_, true);
    auto selected = treeModel_->nodeOf(treeView_->currentIndex());
    const auto expanded = expandedNodes_;
    const auto selectedFiles = selectedNodes(fileView_);
    const auto currentFile = fileModel_->nodeAt(fileView_->currentIndex().row());
    const int treeScroll = treeView_->verticalScrollBar()->value();
    const int fileScroll = fileView_->verticalScrollBar()->value();
    const auto selectedPath = shown->path(selected);
    const auto mapPath = shown->path(treemap_->root());
    QStringList expandedPaths;
    if (filteredTree_)
        for (auto id : expanded) expandedPaths.append(shown->path(id));
    wt::LiveDelta delta;
    bool rebuiltFilter = false;
    if (filteredTree_) {
        const auto sourceDelta = wt::applyLiveUpdate(result_->tree, update);
        for (auto node : sourceDelta.removal.ancestors)
            result_->tree.sortChildrenBy(node, sortCol_ == 1 ? (showAllocated_ ? 3 : 2) : sortCol_,
                                         sortOrder_ == Qt::AscendingOrder);
        if (sourceDelta.structural) {
            // Structural/name changes can change filter membership anywhere below a renamed directory.
            // Rebuild only the in-memory projection; no filesystem rescan.
            filteredTree_ = std::make_unique<wt::Tree>(wt::filterTree(result_->tree, activeInclude_, activeExclude_, activeMatchPath_));
            refreshFilteredViews();
            treeModel_->sort(sortCol_, sortOrder_);
            shown = displayTree();
            rebuiltFilter = true;
            const auto remap = [&](QString path) {
                for (const auto& rename : update.renames) {
                    const auto key = wt::livePathKey(path), oldKey = wt::livePathKey(rename.first);
                    if (key == oldKey || key.startsWith(oldKey + '/'))
                        path = QDir::toNativeSeparators(rename.second) + QDir::toNativeSeparators(path).mid(QDir::toNativeSeparators(rename.first).size());
                }
                return path;
            };
            const auto selectedIds = shown->findPaths({remap(selectedPath)});
            selected = selectedIds.empty() ? shown->root : selectedIds.front();
            const auto mapIds = shown->findPaths({remap(mapPath)});
            if (!mapIds.empty()) treemap_->setRoot(mapIds.front());
            QStringList remappedExpanded;
            for (const auto& path : expandedPaths) remappedExpanded.append(remap(path));
            for (auto id : shown->findPaths(remappedExpanded)) treeView_->expand(treeModel_->indexForNode(id));
            treeView_->verticalScrollBar()->setValue(treeScroll);
            fileView_->verticalScrollBar()->setValue(fileScroll);
            delta.renames = sourceDelta.renames;
        } else delta = treeModel_->updateLive(update, sortCol_, sortOrder_, false);
    } else delta = treeModel_->updateLive(update, sortCol_, sortOrder_);
    if (!rebuiltFilter) {
        expandedNodes_.clear();
        for (auto node : expanded) {
            const auto index = treeModel_->indexForNode(node);
            if (index.isValid()) treeView_->expand(index);
        }
        fileModel_->refreshAfterUpdate(delta);
        const QSet<uint32_t> fileSelection(selectedFiles.begin(), selectedFiles.end());
        for (int row = 0; row < fileModel_->rowCount(); ++row) {
            if (fileSelection.contains(fileModel_->nodeAt(row)))
                fileView_->selectionModel()->select(fileModel_->index(row, 0),
                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
            if (fileModel_->nodeAt(row) == currentFile)
                fileView_->selectionModel()->setCurrentIndex(fileModel_->index(row, 0), QItemSelectionModel::NoUpdate);
        }
        treeView_->verticalScrollBar()->setValue(treeScroll);
        fileView_->verticalScrollBar()->setValue(fileScroll);
    }
    treeView_->setCurrentIndex(treeModel_->indexForNode(shown->survivingAncestor(selected)));
    quint64 total = 0, freeBytes = 0;
    volumeUsage(currentTarget_, total, freeBytes);
    treemap_->remapTags(update.renames);
    treemap_->refreshAfterRemoval(delta.removal, result_->tree.nodes[result_->tree.root].alloc, freeBytes, total);
    updateTreemapBreadcrumb();
    updateStats();
    const auto node = shown->survivingAncestor(selected);
    setWindowTitle(QStringLiteral("[%1]  - DiskCleaner").arg(currentTarget_));
    updateSelectionInfo();
    extTimer_->stop();
    extModel_->applyUpdate(shown, node, delta, unit_);
    const auto& root = result_->tree.nodes[result_->tree.root];
    statusLabel_->setText(tr("Live update: %1 files, %2 folders").arg(root.files).arg(root.folders));
    compactLiveTree();
}

void MainWindow::compactLiveTree()
{
    if (!result_ || !result_->tree.needsCompaction()) return;
    if (filteredTree_) { result_->tree.compact(); return; }
    QScopedValueRollback<bool> guard(syncingTreemapSelection_, true);
    const auto selected = treeModel_->nodeOf(treeView_->currentIndex());
    const auto expanded = expandedNodes_;
    const auto files = selectedNodes(fileView_);
    const auto currentFile = fileModel_->nodeAt(fileView_->currentIndex().row());
    const int treeScroll = treeView_->verticalScrollBar()->value();
    const int fileScroll = fileView_->verticalScrollBar()->value();
    const auto mapping = treeModel_->compactTree();
    const auto remap = [&](uint32_t id) { return id < mapping.size() ? mapping[id] : wt::kNone; };
    expandedNodes_.clear();
    for (auto id : expanded) {
        const auto index = treeModel_->indexForNode(remap(id));
        if (index.isValid()) treeView_->expand(index);
    }
    fileModel_->setTree(&result_->tree);
    // Remap the treemap before selection signals can ask it to render again.
    treemap_->remapNodes(mapping);
    onSearchChanged();
    QSet<uint32_t> selectedFiles;
    for (auto id : files) if (remap(id) != wt::kNone) selectedFiles.insert(remap(id));
    for (int row = 0; row < fileModel_->rowCount(); ++row) {
        const auto id = fileModel_->nodeAt(row);
        if (selectedFiles.contains(id))
            fileView_->selectionModel()->select(fileModel_->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        if (id == remap(currentFile))
            fileView_->selectionModel()->setCurrentIndex(fileModel_->index(row, 0), QItemSelectionModel::NoUpdate);
    }
    treeView_->setCurrentIndex(treeModel_->indexForNode(remap(selected)));
    treeView_->verticalScrollBar()->setValue(treeScroll);
    fileView_->verticalScrollBar()->setValue(fileScroll);
    refreshExtensions();
    updateSelectionInfo();
    updateTreemapBreadcrumb();
}

void MainWindow::setBusy(bool busy)
{
    scanBtn_->setEnabled(!busy);
    cancelBtn_->setVisible(busy);
    progress_->setVisible(busy);
    driveBox_->setEnabled(!busy);
    scanPathEdit_->setEnabled(!busy);
    diskSourceBtn_->setEnabled(!busy);
    folderSourceBtn_->setEnabled(!busy);
    browseBtn_->setEnabled(!busy);
    scanStateLabel_->setText(busy ? tr("Scanning...") : (result_ ? tr("Scan complete") : tr("Choose a location to begin")));
    scanStateLabel_->setProperty("complete", !busy && bool(result_));
    updateStats();
    updateSelectionInfo();
}

void MainWindow::setScanSource(bool folder)
{
    folderSource_ = folder;
    diskSourceBtn_->setChecked(!folder);
    folderSourceBtn_->setChecked(folder);
    driveBox_->setVisible(!folder);
    scanPathEdit_->setVisible(folder);
}

void MainWindow::setSizeMetric(bool allocated)
{
    const auto selected = treeModel_->nodeOf(treeView_->currentIndex());
    const auto expanded = expandedNodes_;
    const auto fileSelection = selectedNodes(fileView_);
    const auto fileSort = fileView_->horizontalHeader()->sortIndicatorSection();
    const auto fileOrder = fileView_->horizontalHeader()->sortIndicatorOrder();
    QScopedValueRollback<bool> guard(syncingTreemapSelection_, true);
    showAllocated_ = allocated;
    treeModel_->setShowAllocated(allocated);
    fileModel_->setShowAllocated(allocated);
    extModel_->setShowAllocated(allocated);
    treemap_->setShowAllocated(allocated);
    logicalMetricBtn_->setChecked(!allocated);
    allocatedMetricBtn_->setChecked(allocated);
    if (allocatedAction_) {
        QSignalBlocker blocker(allocatedAction_);
        allocatedAction_->setChecked(allocated);
    }
    treeView_->setColumnHidden(2, allocated);
    treeView_->setColumnHidden(3, !allocated);
    const int sizeColumn = allocated ? 3 : 2;
    treeView_->header()->moveSection(treeView_->header()->visualIndex(sizeColumn), 1);
    fileView_->setColumnHidden(2, allocated);
    fileView_->setColumnHidden(3, !allocated);
    if (sortCol_ == 2 || sortCol_ == 3) sortCol_ = sizeColumn;
    treeModel_->sort(sortCol_, sortOrder_);
    treeView_->header()->setSortIndicator(sortCol_, sortOrder_);
    fileView_->sortByColumn(fileSort == 2 || fileSort == 3 ? sizeColumn : fileSort, fileOrder);
    for (auto node : expanded) treeView_->expand(treeModel_->indexForNode(node));
    const auto index = treeModel_->indexForNode(selected);
    if (index.isValid()) treeView_->setCurrentIndex(index);
    const QSet<uint32_t> selectedFiles(fileSelection.begin(), fileSelection.end());
    for (int row = 0; row < fileModel_->rowCount(); ++row)
        if (selectedFiles.contains(fileModel_->nodeAt(row)))
            fileView_->selectionModel()->select(fileModel_->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    metricLabel_->setText(allocated ? tr("Space on disk") : tr("File size"));
    settings_.setValue(QStringLiteral("treemapAllocated"), allocated);
    updateStats();
    updateTreemapBreadcrumb();
    updateSelectionInfo();
}

void MainWindow::onCancelClicked()
{
    cancel_ = true;
    statusLabel_->setText(tr("Canceling..."));
}

void MainWindow::updateStats()
{
    auto* shown = displayTree();
    const bool haveResult = shown && shown->root < shown->nodes.size();
    emptyHint_->setVisible(!haveResult);
    if (!haveResult) {
        selLabel_->setText(QStringLiteral("—"));
        alternateSizeLabel_->clear();
        scanTitle_->setText(tr("Ready to scan"));
        volumeLabel_->setText(tr("Drive capacity"));
        totalLabel_->setText(tr("Available after scanning"));
        usedLabel_->clear();
        freeLabel_->clear();
        volumeBar_->hide();
        return;
    }
    const wt::Tree& t = *shown;
    const wt::Node& root = t.nodes[t.root];
    const QString rootPath = t.path(t.root);
    QString name = QFileInfo(QDir::fromNativeSeparators(rootPath)).fileName();
    if (name.isEmpty()) name = t.name(t.root);
    const QString title = displayingPreviousScan_
                              ? (filteredTree_ ? tr("Previous filtered scan · %1") : tr("Previous scan · %1"))
                              : (filteredTree_ ? tr("Filtered scan · %1") : tr("This scan · %1"));
    scanTitle_->setText(title.arg(name));
    scanTitle_->setToolTip(rootPath);
    selLabel_->setText(wt::formatSize(showAllocated_ ? root.alloc : root.size, unit_, decimals_));
    alternateSizeLabel_->setText(QStringLiteral("%1 %2")
        .arg(showAllocated_ ? tr("File size") : tr("Space on disk"),
             wt::formatSize(showAllocated_ ? root.size : root.alloc, unit_, decimals_)));
    quint64 total = 0, freeB = 0;
    // Imported or merged roots do not imply a single local drive capacity.
    if ((resultScanMode_ == 0 || resultScanMode_ == 1) && volumeUsage(rootPath, total, freeB)) {
        const QString volume = QDir::toNativeSeparators(QFileInfo(rootPath).absolutePath()).left(2);
        volumeLabel_->setText(tr("Drive capacity · %1").arg(volume));
        totalLabel_->setText(tr("Total %1").arg(wt::formatSize(total, unit_, decimals_)));
        usedLabel_->setText(tr("Used %1 (%2%)")
                                .arg(wt::formatSize(total - freeB, unit_, decimals_))
                                .arg(total ? 100.0 * double(total - freeB) / double(total) : 0.0,
                                     0, 'f', 1));
        freeLabel_->setText(tr("Free %1").arg(wt::formatSize(freeB, unit_, decimals_)));
        volumeBar_->setValue(total ? int(1000.0 * double(total - freeB) / double(total)) : 0);
        volumeBar_->show();
    } else {
        volumeLabel_->setText(resultScanMode_ < 0 ? tr("Multiple locations") : tr("Imported scan"));
        totalLabel_->setText(tr("Drive capacity unavailable"));
        usedLabel_->clear();
        freeLabel_->clear();
        volumeBar_->hide();
    }
}

void MainWindow::updateSelectionInfo()
{
    if (!pathLabel_ || !treeView_->model() || !fileView_->model()) return;
    const auto nodes = selectedNodes(tabs_->currentIndex() == 1 ? static_cast<QWidget*>(fileView_)
                                                              : static_cast<QWidget*>(treeView_));
    auto* tree = displayTree();
    const bool available = tree && !nodes.isEmpty() && !scanning_;
    exploreBtn_->setEnabled(available);
    copyPathBtn_->setEnabled(available);
    if (!tree || nodes.isEmpty()) {
        pathLabel_->setText(tr("Select an item to view its details"));
        pathLabel_->setToolTip({});
        return;
    }
    if (nodes.size() == 1) {
        const auto node = nodes.first();
        const auto& nd = tree->nodes[node];
        QString name = QFileInfo(tree->path(node)).fileName();
        if (name.isEmpty()) name = tree->name(node);
        pathLabel_->setText(tr("Selected: %1 · %2 %3")
            .arg(name, showAllocated_ ? tr("Space on disk") : tr("File size"),
                 wt::formatSize(showAllocated_ ? nd.alloc : nd.size, unit_, decimals_)));
        pathLabel_->setToolTip(tree->path(node));
    } else {
        pathLabel_->setText(tr("%1 items selected").arg(nodes.size()));
        pathLabel_->setToolTip({});
    }
}

void MainWindow::onTreeSelectionChanged()
{
    if (!result_) return;
    const QModelIndex idx = treeView_->currentIndex();
    if (!idx.isValid()) return;
    const uint32_t n = treeModel_->nodeOf(idx);
    if (n == wt::kNone) return;
    wt::Tree* shown = displayTree();
    if (!shown || n >= shown->nodes.size()) return;
    const wt::Node& nd = shown->nodes[n];
    updateSelectionInfo();
    if (!syncingTreemapSelection_) {
        QScopedValueRollback<bool> guard(syncingTreemapSelection_, true);
        treemap_->setRoot(nd.isDir ? n : nd.parent);
        treemap_->selectNode(n);
    }
    updateTreemapBreadcrumb();
    if (extTimer_) extTimer_->start();
}

void MainWindow::refreshExtensions()
{
    if (!result_) {
        extModel_->rebuild(nullptr, wt::kNone, unit_);
        return;
    }
    wt::Tree* shown = displayTree();
    if (!shown) {
        extModel_->rebuild(nullptr, wt::kNone, unit_);
        return;
    }
    uint32_t node = shown->root;
    const QModelIndex idx = treeView_->currentIndex();
    if (idx.isValid()) {
        const uint32_t n = treeModel_->nodeOf(idx);
        if (n != wt::kNone) node = n;
    }
    extModel_->rebuild(shown, node, unit_);
}

void MainWindow::onTreeHeaderClicked(int column)
{
    if (!result_) return;
    if (column == sortCol_)
        sortOrder_ = (sortOrder_ == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder;
    else
        sortOrder_ = Qt::DescendingOrder;
    sortCol_ = column;
    treeModel_->sort(sortCol_, sortOrder_);
    treeView_->expand(treeModel_->index(0, 0, QModelIndex()));
    settings_.setValue(QStringLiteral("sortCol"), sortCol_);
    settings_.setValue(QStringLiteral("sortOrder"), int(sortOrder_));
}

void MainWindow::onSearchChanged()
{
    if (!result_) return;
    const int maxFiles = maxFilesBox_->currentData().toInt();
    fileModel_->setUnit(unit_);
    treemap_->setUnit(unit_);
    fileModel_->rebuild(searchEdit_->text(), false, showFoldersBox_->isChecked(), maxFiles,
                        dupsOnlyBox_->isChecked());
    if (result_) {
        const wt::Node& root = result_->tree.nodes[result_->tree.root];
        if (fileModel_->dupMode() > 0)
            statusLabel_->setText(tr("%1 files, %2 folders, %3 duplicate groups")
                                      .arg(root.files)
                                      .arg(root.folders)
                                      .arg(fileModel_->duplicateGroups()));
        else
            statusLabel_->setText(tr("%1 files, %2 folders").arg(root.files).arg(root.folders));
    }
}

void MainWindow::exportCsv()
{
    if (!result_) {
        QMessageBox::information(this, tr("Export"), tr("Scan something first."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Export"));
    auto* form = new QFormLayout(&dlg);
    auto* files = new QCheckBox(tr("Export Files"), &dlg);
    files->setChecked(true);
    auto* folders = new QCheckBox(tr("Export Folders"), &dlg);
    folders->setChecked(true);
    auto* driveSize = new QCheckBox(tr("Export drive size information"), &dlg);
    auto* dates = new QCheckBox(tr("Export \"Last Accessed\" and \"Created\" dates"), &dlg);
    auto* folderSize =
        new QCheckBox(tr("Export total size and allocated size of folders (excluding sub folders)"),
                      &dlg);
    auto* splitNames =
        new QCheckBox(tr("Split file names into root, folder, filename and extension"), &dlg);
    auto* percent = new QCheckBox(tr("Export percentage of parent value"), &dlg);
    auto* mftNo = new QCheckBox(tr("Export MFT record number"), &dlg);
    auto* selectedOnly = new QCheckBox(tr("Selected only"), &dlg);
    auto* depth = new QSpinBox(&dlg);
    depth->setRange(0, 1000000);
    depth->setSpecialValueText(tr("no limit"));

    form->addRow(files);
    form->addRow(folders);
    form->addRow(driveSize);
    form->addRow(dates);
    form->addRow(folderSize);
    form->addRow(splitNames);
    form->addRow(percent);
    form->addRow(mftNo);
    form->addRow(selectedOnly);
    form->addRow(tr("Max folder depth (0 = no limit):"), depth);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    if (!files->isChecked() && !folders->isChecked()) {
        QMessageBox::warning(this, tr("Export"),
                             tr("Either one or both of \"Export Files\" and \"Export Folders\" must "
                                "be selected for the export to be valid."));
        return;
    }

    QString path = QFileDialog::getSaveFileName(this, tr("Export to CSV"),
                                                QDir::homePath() + QStringLiteral("/wiztree.csv"),
                                                tr("CSV files (*.csv);;All files (*.*)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive)) path += QStringLiteral(".csv");

    ExportOptions opt;
    opt.files = files->isChecked();
    opt.folders = folders->isChecked();
    opt.driveSize = driveSize->isChecked();
    opt.dates = dates->isChecked();
    opt.folderSize = folderSize->isChecked();
    opt.splitNames = splitNames->isChecked();
    opt.percentOfParent = percent->isChecked();
    opt.mftNo = mftNo->isChecked();
    opt.selectedOnly = selectedOnly->isChecked();
    opt.maxDepth = depth->value();
    writeCsv(path, opt);
}

void MainWindow::exportCsvTo(const QString& path)
{
    writeCsv(path, ExportOptions{});
}

void MainWindow::exportTreemapPng()
{
    if (!result_) {
        QMessageBox::information(this, tr("Export"), tr("Scan something first."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Save Treemap Image as .png file"));
    auto* form = new QFormLayout(&dlg);
    auto* width = new QSpinBox(&dlg);
    width->setRange(1, 30000);
    width->setValue(settings_.value(QStringLiteral("treemapW"), 1920).toInt());
    auto* height = new QSpinBox(&dlg);
    height->setRange(1, 30000);
    height->setValue(settings_.value(QStringLiteral("treemapH"), 1080).toInt());

    auto* wrow = new QWidget(&dlg);
    auto* wl = new QHBoxLayout(wrow);
    wl->setContentsMargins(0, 0, 0, 0);
    wl->addWidget(width, 1);
    auto* preset = new QToolButton(wrow);
    preset->setText(QStringLiteral("..."));
    auto* pmenu = new QMenu(preset);
    const int presets[][2] = {{1024, 768},  {1280, 720},  {1680, 1050}, {1920, 1080},
                              {2560, 1440}, {3840, 2160}, {7680, 4320}};
    for (const auto& pr : presets) {
        auto* item = pmenu->addAction(QStringLiteral("%1x%2").arg(pr[0]).arg(pr[1]));
        connect(item, &QAction::triggered, &dlg, [width, height, pr] {
            width->setValue(pr[0]);
            height->setValue(pr[1]);
        });
    }
    preset->setMenu(pmenu);
    preset->setPopupMode(QToolButton::InstantPopup);
    wl->addWidget(preset);
    form->addRow(tr("Width:"), wrow);
    form->addRow(tr("Height:"), height);

    auto* gray = new QCheckBox(tr("Shades of Gray (single color)"), &dlg);
    auto* freeBox = new QCheckBox(tr("Show Free Space"), &dlg);
    freeBox->setChecked(showFreeSpace_);
    freeBox->setEnabled(showAllocated_);
    freeBox->setToolTip(tr("Capacity overlays are available for full drives in space-on-disk mode."));
    form->addRow(gray);
    form->addRow(freeBox);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);

    if (dlg.exec() != QDialog::Accepted) return;
    QString path = QFileDialog::getSaveFileName(this, tr("Save Treemap Image"),
                                                QDir::homePath() + QStringLiteral("/treemap.png"),
                                                tr("PNG files (*.png)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) path += QStringLiteral(".png");
    settings_.setValue(QStringLiteral("treemapW"), width->value());
    settings_.setValue(QStringLiteral("treemapH"), height->value());
    exportTreemapTo(path, QSize(width->value(), height->value()), gray->isChecked(),
                    freeBox->isChecked());
}

void MainWindow::exportTreemapTo(const QString& path, const QSize& size, bool grayscale,
                                 bool freeSpace)
{
    if (!treemap_) return;
    const QImage img = treemap_->renderImage(size, grayscale, freeSpace);
    if (img.isNull() || !img.save(path, "PNG")) {
        QMessageBox::warning(this, tr("Export"), tr("Cannot write %1").arg(path));
        return;
    }
    statusLabel_->setText(
        tr("Treemap image saved: %1 x %2").arg(size.width()).arg(size.height()));
}

void MainWindow::writeCsv(const QString& path, const ExportOptions& opt)
{
    if (!result_) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export"), tr("Cannot write %1").arg(path));
        return;
    }
    QTextStream out(&f);

    QString header = QStringLiteral("File Name,Path,Size,Allocated,Modified,Attributes,Files,Folders");
    if (opt.mftNo) header += QStringLiteral(",MFTRECNO");
    if (opt.dates) header += QStringLiteral(",CREATEDDATE,LASTACCESSDATE");
    if (opt.folderSize) header += QStringLiteral(",FOLDERSIZE,FOLDERALLOCATED");
    if (opt.splitNames) header += QStringLiteral(",ROOT,FOLDERNAME,FILENAME,FILEEXT");
    if (opt.percentOfParent) header += QStringLiteral(",PERCENTOFPARENT");
    if (opt.driveSize) header += QStringLiteral(",DRIVECAPACITY,USEDSPACE,FREESPACE,RESERVEDSPACE");
    out << header << '\n';

    const wt::Tree& t = result_->tree;
    uint32_t startNode = t.root;
    if (opt.selectedOnly) {
        const QModelIndex current = treeView_->currentIndex();
        const uint32_t sel = current.isValid() ? treeModel_->nodeOf(current) : wt::kNone;
        if (sel != wt::kNone && sel < t.nodes.size()) startNode = sel;
    }

    quint64 driveTotal = 0, driveFree = 0;
    const bool haveDrive = volumeUsage(currentTarget_, driveTotal, driveFree);

    auto csvEscape = [](const QString& s) {
        if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) ||
            s.contains(QLatin1Char('\n'))) {
            QString e = s;
            e.replace(QLatin1Char('"'), QLatin1String("\"\""));
            return QLatin1Char('"') + e + QLatin1Char('"');
        }
        return s;
    };
    auto dateText = [](int64_t ft) {
        if (ft <= 0) return QString();
        const qint64 ms = ft / 10000 - 11644473600000LL;
        if (ms <= 0) return QString();
        return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    };

    struct Item {
        uint32_t node;
        int depth;
    };
    std::vector<Item> stack;
    stack.push_back({startNode, 0});

    QProgressDialog dlg(tr("Exporting..."), tr("Cancel"), 0,
                        int(qMin<size_t>(t.nodes.size(), 2000000)), this);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(400);

    uint32_t written = 0;
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        const wt::Node& nd = t.nodes[it.node];
        if (opt.maxDepth > 0 && it.depth > opt.maxDepth) continue;

        if (it.node != t.root && (nd.isDir ? opt.folders : opt.files)) {
            const QString name = csvEscape(t.name(it.node));
            const uint32_t p = nd.parent;
            const QString parentPath = (p != wt::kNone && p < t.nodes.size())
                                           ? csvEscape(t.path(p))
                                           : QString();
            out << name << ',' << parentPath << ',' << nd.size << ',' << nd.alloc << ','
                << dateText(nd.mtime) << ',' << csvEscape(wt::attributesText(nd.attrs)) << ','
                << nd.files << ',' << nd.folders;
            if (opt.mftNo) out << ',' << nd.mftRec;
            if (opt.dates) out << ',' << dateText(nd.ctime) << ',' << dateText(nd.atime);
            if (opt.folderSize)
                out << ',' << (nd.isDir ? QString::number(nd.size) : QString()) << ','
                    << (nd.isDir ? QString::number(nd.alloc) : QString());
            if (opt.splitNames) {
                const QString full = t.path(it.node);
                const int sep = full.lastIndexOf(QLatin1Char('\\'));
                const QString dirPart = sep > 0 ? full.left(sep) : QString();
                const QString filePart = sep >= 0 ? full.mid(sep + 1) : full;
                const int dot = filePart.lastIndexOf(QLatin1Char('.'));
                const QString base = dot > 0 ? filePart.left(dot) : filePart;
                const QString ext =
                    (dot > 0 && dot + 1 < filePart.size()) ? filePart.mid(dot + 1) : QString();
                QString rootPart;
                if (dirPart.size() >= 2 && dirPart.at(1) == QLatin1Char(':'))
                    rootPart = dirPart.left(2);
                out << ',' << csvEscape(rootPart) << ',' << csvEscape(dirPart) << ','
                    << csvEscape(base) << ',' << csvEscape(ext);
            }
            if (opt.percentOfParent) {
                const uint64_t whole =
                    (p != wt::kNone && p < t.nodes.size()) ? t.nodes[p].size : nd.size;
                out << ','
                    << (whole ? QString::number(100.0 * double(nd.size) / double(whole), 'f', 3)
                              : QStringLiteral("0"));
            }
            if (opt.driveSize) {
                const quint64 used = haveDrive ? driveTotal - driveFree : 0;
                out << ',' << (haveDrive ? QString::number(driveTotal) : QString()) << ','
                    << (haveDrive ? QString::number(used) : QString()) << ','
                    << (haveDrive ? QString::number(driveFree) : QString()) << ','
                    << (haveDrive ? QStringLiteral("0") : QString());
            }
            out << '\n';
            ++written;
            if ((written & 0x3FFF) == 0) {
                dlg.setValue(int(qMin<uint32_t>(written, 2000000)));
                QApplication::processEvents();
                if (dlg.wasCanceled()) {
                    f.close();
                    f.remove();
                    return;
                }
            }
        }

        if (opt.maxDepth > 0 && it.depth >= opt.maxDepth) continue;
        for (uint32_t i = nd.childCount; i > 0; --i)
            stack.push_back({t.children[nd.childBegin + (i - 1)], it.depth + 1});
    }
    dlg.setValue(dlg.maximum());
    statusLabel_->setText(tr("Export complete."));
}

static ExportOptions fullExportOptions()
{
    ExportOptions o;
    o.dates = o.folderSize = o.splitNames = o.percentOfParent = o.mftNo = o.driveSize = true;
    return o;
}

void MainWindow::exportCsvFullTo(const QString& path)
{
    writeCsv(path, fullExportOptions());
}

void MainWindow::exportFileTypesCsv()
{
    if (!result_) return;
    QString path = QFileDialog::getSaveFileName(this, tr("Export File Types"),
                                                QDir::homePath() + QStringLiteral("/filetypes.csv"),
                                                tr("CSV files (*.csv)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive)) path += QStringLiteral(".csv");

    struct Agg { uint64_t size = 0, alloc = 0; uint32_t count = 0; };
    std::map<QString, Agg> agg;
    const wt::Tree& t = result_->tree;
    for (uint32_t i = 1; i < t.nodes.size(); ++i) {
        const wt::Node& nd = t.nodes[i];
        if (nd.removed || nd.isDir) continue;
        const QString name = t.name(i);
        const int dot = name.lastIndexOf(QLatin1Char('.'));
        QString ext = dot > 0 ? name.mid(dot + 1).toLower() : QStringLiteral("(No Extension)");
        auto& a = agg[ext];
        a.size += nd.size;
        a.alloc += nd.alloc;
        ++a.count;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream out(&f);
    out << "Extension,Files,Size,Allocated\n";
    for (const auto& kv : agg)
        out << kv.first << ',' << kv.second.count << ',' << kv.second.size << ','
            << kv.second.alloc << '\n';
    statusLabel_->setText(tr("Export complete."));
}

wt::Tree* MainWindow::displayTree() const
{
    if (filteredTree_) return filteredTree_.get();
    return result_ ? &result_->tree : nullptr;
}

void MainWindow::toggleFilterPanel(bool on)
{
    if (!filterBar_) return;
    filterBar_->setVisible(on);
    if (filterAction_) {
        QSignalBlocker blocker(filterAction_);
        filterAction_->setChecked(on);
    }
    settings_.setValue(QStringLiteral("filterscanresults"), on);
    if (on && filterInclude_) filterInclude_->setFocus();
}

void MainWindow::applyFilter()
{
    if (!result_) return;
    const QString inc = filterInclude_->text().trimmed();
    const QString exc = filterExclude_->text().trimmed();
    if (inc.isEmpty() && exc.isEmpty()) {
        clearFilter();
        return;
    }
    activeInclude_ = inc;
    activeExclude_ = exc;
    activeMatchPath_ = filterPathRadio_->isChecked();
    filteredTree_ = std::make_unique<wt::Tree>(
        wt::filterTree(result_->tree, inc, exc, activeMatchPath_));
    refreshFilteredViews();
    statusLabel_->setText(tr("Filter applied: %1 items").arg(qulonglong(filteredTree_->nodes.size())));
}

void MainWindow::clearFilter()
{
    filterInclude_->clear();
    filterExclude_->clear();
    activeInclude_.clear();
    activeExclude_.clear();
    if (!filteredTree_) return;
    filteredTree_.reset();
    refreshFilteredViews();
    statusLabel_->setText(tr("Filter cleared"));
}

void MainWindow::refreshFilteredViews()
{
    expandedNodes_.clear();
    wt::Tree* t = displayTree();
    treeModel_->setTree(t);
    fileModel_->setTree(t);
    if (t) {
        treemap_->setTree(t, t->root);
        treemap_->setScannedAllocated(result_->tree.nodes[result_->tree.root].alloc);
        quint64 total = 0, freeBytes = 0;
        if ((resultScanMode_ == 0 || resultScanMode_ == 1) &&
            volumeUsage(result_->tree.path(result_->tree.root), total, freeBytes))
            treemap_->setFreeSpace(freeBytes, total);
        updateTreemapBreadcrumb();
        extModel_->rebuild(t, t->root, unit_);
    } else {
        treemap_->setTree(nullptr, wt::kNone);
        extModel_->rebuild(nullptr, wt::kNone, unit_);
    }
    const QModelIndex rootIdx = treeModel_->index(0, 0, QModelIndex());
    treeModel_->sort(sortCol_, sortOrder_);
    treeView_->expand(rootIdx);
    treeView_->setCurrentIndex(rootIdx);
    onSearchChanged();
    updateStats();
    updateSelectionInfo();
}

void MainWindow::applyFilterFromCli(const QString& text, bool matchPath)
{
    if (!filterBar_) return;
    toggleFilterPanel(true);
    filterInclude_->setText(text);
    if (matchPath) filterPathRadio_->setChecked(true);
    applyFilter();
}

QList<uint32_t> MainWindow::selectedNodes(QWidget* source) const
{
    QList<uint32_t> out;
    wt::Tree* t = displayTree();
    if (!t) return out;
    if (source == treeView_) {
        const QModelIndex idx = treeView_->currentIndex();
        if (idx.isValid()) {
            const uint32_t n = treeModel_->nodeOf(idx);
            if (n != wt::kNone) out << n;
        }
    } else if (source == fileView_ && fileView_->selectionModel()) {
        const QModelIndexList rows = fileView_->selectionModel()->selectedRows(0);
        for (const QModelIndex& idx : rows) {
            const uint32_t n = fileModel_->nodeAt(idx.row());
            if (n != wt::kNone) out << n;
        }
    }
    return out;
}

QList<uint32_t> MainWindow::actionNodes() const
{
    return selectedNodes(menuSource_ ? menuSource_ : treeView_);
}

void MainWindow::showItemMenu(QWidget* source, const QPoint& globalPos)
{
    if (!result_) return;
    const QList<uint32_t> nodes = selectedNodes(source);
    if (nodes.isEmpty()) return;
    QMenu menu(this);
    auto* copyFilesAct = menu.addAction(tr("Copy"));
    auto* cutFilesAct = menu.addAction(tr("Cut"));
    menu.addSeparator();
    auto* openAct = menu.addAction(tr("Open"));
    auto* exploreAct = menu.addAction(tr("Explore Folder"));
    auto* cmdAct = menu.addAction(tr("Command Prompt Here"));
    menu.addSeparator();
    auto* copyAct = menu.addAction(tr("Copy Path"));
    auto* propsAct = menu.addAction(tr("Properties"));
    menu.addSeparator();
    auto* deleteAct = menu.addAction(tr("Delete (to Recycle Bin)"));

    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return;
    QScopedValueRollback<QWidget*> sourceGuard(menuSource_, source);
    if (chosen == copyFilesAct) copyFilesToClipboard(false);
    else if (chosen == cutFilesAct) copyFilesToClipboard(true);
    else if (chosen == openAct) openSelection();
    else if (chosen == exploreAct) exploreSelection();
    else if (chosen == cmdAct) commandPromptHere();
    else if (chosen == copyAct) copyPathToClipboard();
    else if (chosen == propsAct) showProperties();
    else if (chosen == deleteAct) deleteSelectionToRecycleBin();
}

void MainWindow::showTreeContextMenu(const QPoint& pos)
{
    const QModelIndex idx = treeView_->indexAt(pos);
    if (idx.isValid()) treeView_->setCurrentIndex(idx);
    showItemMenu(treeView_, treeView_->viewport()->mapToGlobal(pos));
}

void MainWindow::showFileContextMenu(const QPoint& pos)
{
    const QModelIndex idx = fileView_->indexAt(pos);
    if (idx.isValid()) fileView_->selectRow(idx.row());
    showItemMenu(fileView_, fileView_->viewport()->mapToGlobal(pos));
}

void MainWindow::openSelection()
{
    wt::Tree* t = displayTree();
    if (!t) return;
    for (uint32_t n : actionNodes()) {
        const QString p = t->path(n);
        if (p.isEmpty()) continue;
        ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(p.utf16()), nullptr, nullptr,
                      SW_SHOWNORMAL);
    }
}

void MainWindow::exploreSelection()
{
    wt::Tree* t = displayTree();
    if (!t) return;
    for (uint32_t n : actionNodes()) {
        const QString p = t->path(n);
        if (p.isEmpty()) continue;
        if (t->nodes[n].isDir) {
            ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(p.utf16()), nullptr, nullptr,
                          SW_SHOWNORMAL);
        } else {
            const QString args = QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(p));
            ShellExecuteW(nullptr, L"open", L"explorer.exe",
                          reinterpret_cast<LPCWSTR>(args.utf16()), nullptr, SW_SHOWNORMAL);
        }
    }
}

void MainWindow::commandPromptHere()
{
    wt::Tree* t = displayTree();
    if (!t) return;
    const QList<uint32_t> nodes = actionNodes();
    if (nodes.isEmpty()) return;
    QString dir = t->path(nodes.first());
    if (!t->nodes[nodes.first()].isDir) dir = QFileInfo(dir).absolutePath();
    QProcess::startDetached(QStringLiteral("cmd.exe"), QStringList(), dir);
}

void MainWindow::copyPathToClipboard()
{
    wt::Tree* t = displayTree();
    if (!t) return;
    QStringList paths;
    for (uint32_t n : actionNodes()) paths << t->path(n);
    if (paths.isEmpty()) return;
    QApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
    statusLabel_->setText(tr("Copied %1 path(s) to clipboard").arg(paths.size()));
}

void MainWindow::showProperties()
{
    wt::Tree* t = displayTree();
    if (!t) return;
    const QList<uint32_t> nodes = actionNodes();
    if (nodes.isEmpty()) return;
    const QString p = t->path(nodes.first());
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.lpVerb = L"properties";
    sei.lpFile = reinterpret_cast<LPCWSTR>(p.utf16());
    sei.nShow = SW_SHOW;
    ShellExecuteExW(&sei);
}

void MainWindow::deleteSelectionToRecycleBin()
{
    wt::Tree* t = displayTree();
    if (!t) return;
    QStringList paths;
    for (uint32_t n : actionNodes()) paths << t->path(n);
    if (paths.isEmpty()) return;
    if (QMessageBox::question(this, tr("Delete"),
                              tr("Move %1 item(s) to the Recycle Bin?").arg(paths.size()),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    if (wt::moveToRecycleBin(paths))
        statusLabel_->setText(tr("Moved %1 item(s) to the Recycle Bin")
                                  .arg(paths.size()));
    else
        statusLabel_->setText(tr("Delete failed"));
}

void MainWindow::copyTreeInfoToClipboard()
{
    wt::Tree* t = displayTree();
    if (!t) return;
    uint32_t root = t->root;
    const QModelIndex idx = treeView_->currentIndex();
    if (idx.isValid()) {
        const uint32_t n = treeModel_->nodeOf(idx);
        if (n != wt::kNone) root = n;
    }
    QString out;
    struct Item {
        uint32_t node;
        int depth;
    };
    std::vector<Item> stack{{root, 0}};
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        const wt::Node& nd = t->nodes[it.node];
        out += QString(it.depth * 2, QLatin1Char(' '));
        out += t->name(it.node);
        out += QLatin1Char('\t');
        out += wt::formatSize(nd.size, unit_, 1);
        out += QLatin1Char('\n');
        for (int i = int(nd.childCount) - 1; i >= 0; --i)
            stack.push_back({t->children[nd.childBegin + uint32_t(i)], it.depth + 1});
    }
    out += QStringLiteral("\nGenerated by DiskCleaner 0.1 (%1)\n")
               .arg(QDate::currentDate().toString(Qt::ISODate));
    QApplication::clipboard()->setText(out);
    statusLabel_->setText(tr("Copied tree info to clipboard"));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty()) return;
    const QString path = urls.first().toLocalFile();
    if (path.isEmpty()) return;
    const QFileInfo fi(path);
    if (fi.isDir()) {
        startScan(path, 0, false);
    } else {
        const QString suffix = fi.suffix().toLower();
        if (suffix == QLatin1String("mft") || suffix == QLatin1String("bin")) {
            startScanPath(path, 0, 2);
        } else if (suffix == QLatin1String("csv") || suffix == QLatin1String("tsv")) {
            startScanPath(path, 0, 3);
        } else {
            startScan(fi.absolutePath(), 0, false);
        }
    }
    e->acceptProposedAction();
}

void MainWindow::updateTreemapBreadcrumb()
{
    if (!treemapPath_) return;
    wt::Tree* t = displayTree();
    if (!t || treemap_->root() >= t->nodes.size()) {
        treemapPath_->clear();
        treemapSummary_->setText(tr("No scan results yet"));
        return;
    }
    const auto node = treemap_->root();
    const QString path = t->path(node);
    treemapPath_->setText(QFontMetrics(treemapPath_->font()).elidedText(path, Qt::ElideMiddle,
                                                                      std::max(80, treemapPath_->width())));
    treemapPath_->setToolTip(path);
    treemapSummary_->setText(QStringLiteral("%1 · %2")
        .arg(showAllocated_ ? tr("Space on disk") : tr("File size"),
             wt::formatSize(showAllocated_ ? t->nodes[node].alloc : t->nodes[node].size, unit_, decimals_)));
    treemapSummary_->setToolTip(tr("Area is proportional to bytes in this directory. Small hidden cells keep their area."));
}

void MainWindow::treemapUp()
{
    treemap_->goUp();
}

void MainWindow::selectTreemapNode(uint32_t node)
{
    wt::Tree* tree = displayTree();
    if (!tree || node >= tree->nodes.size()) return;
    std::vector<uint32_t> chain;
    uint32_t current = node;
    while (current != tree->root && current < tree->nodes.size() && chain.size() < 512) {
        chain.push_back(current);
        current = tree->nodes[current].parent;
    }
    if (current != tree->root) return;
    // TreeModel includes a visible root row; traverse from that row, not an invalid index.
    QModelIndex index = treeModel_->index(0, 0, QModelIndex());
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        treeView_->expand(index);
        index = treeModel_->index(int(tree->nodes[*it].rowInParent), 0, index);
        if (!index.isValid()) return;
    }
    QScopedValueRollback<bool> guard(syncingTreemapSelection_, true);
    treeView_->setCurrentIndex(index);
    treeView_->scrollTo(index, QAbstractItemView::PositionAtCenter);
}

void MainWindow::showTreemapMenu(const QPoint& pos)
{
    wt::Tree* tree = displayTree();
    if (!tree) return;
    const uint32_t node = treemap_->nodeAt(pos);
    QMenu menu(this);
    auto* backAct = menu.addAction(tr("Back (Backspace)"));
    backAct->setEnabled(treemap_->canGoBack());
    auto* forwardAct = menu.addAction(tr("Forward (Shift+Backspace)"));
    forwardAct->setEnabled(treemap_->canGoForward());
    auto* upAct = menu.addAction(tr("Up one level"));
    upAct->setEnabled(treemap_->canGoUp());
    auto* homeAct = menu.addAction(tr("Scan root (Ctrl+Home)"));
    auto* zoomAct = menu.addAction(tr("Zoom into folder"));
    zoomAct->setEnabled(node < tree->nodes.size() && tree->nodes[node].isDir);
    menu.addSeparator();
    auto* copyAct = menu.addAction(tr("Copy Path"));
    copyAct->setEnabled(node != wt::kNone);
    auto* tagsMenu = menu.addMenu(tr("Tag"));
    tagsMenu->setEnabled(node != wt::kNone);
    const QStringList labels = {tr("Red (1)"), tr("Yellow (2)"), tr("Green (3)"), tr("Blue (4)")};
    for (int i = 0; i < labels.size(); ++i) {
        auto* action = tagsMenu->addAction(labels[i]);
        connect(action, &QAction::triggered, this, [this, node, i] { treemap_->toggleTag(node, i + 1); });
    }
    auto* clearTagsAct = menu.addAction(tr("Clear tags in this view (0)"));
    menu.addSeparator();
    auto* unknownAct = menu.addAction(tr("Show unknown space"));
    unknownAct->setCheckable(true);
    unknownAct->setToolTip(tr("Capacity overlays are available for full drives in space-on-disk mode."));
    unknownAct->setChecked(treemap_->showUnknownSpace());
    auto* pixelsAct = menu.addAction(tr("Minimum cell size..."));
    auto* saveAct = menu.addAction(tr("Save Treemap Image as .png file..."));
    QAction* chosen = menu.exec(treemap_->mapToGlobal(pos));
    if (chosen == backAct) treemap_->goBack();
    else if (chosen == forwardAct) treemap_->goForward();
    else if (chosen == homeAct) treemap_->goHome();
    else if (chosen == zoomAct) treemap_->setRoot(node);
    else if (chosen == clearTagsAct) treemap_->clearTagsInRoot();
    else if (chosen == unknownAct) {
        treemap_->setShowUnknownSpace(unknownAct->isChecked());
        settings_.setValue(QStringLiteral("treemapUnknown"), unknownAct->isChecked());
    } else if (chosen == pixelsAct) {
        bool ok = false;
        const int pixels = QInputDialog::getInt(this, tr("Minimum cell size"), tr("Pixels:"),
                                                treemap_->minimumCellSize(), 1, 40, 1, &ok);
        if (ok) {
            treemap_->setMinimumCellSize(pixels);
            settings_.setValue(QStringLiteral("treemapMinPixels"), pixels);
        }
    } else if (chosen == upAct) {
        treemapUp();
    } else if (chosen == copyAct) {
        if (node < tree->nodes.size()) QApplication::clipboard()->setText(tree->path(node));
    } else if (chosen == saveAct) {
        exportTreemapPng();
    }
}

void MainWindow::openCsvFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Select CSV file"), QDir::homePath(),
                                                      tr("CSV/TSV (*.csv *.tsv);;All files (*.*)"));
    if (path.isEmpty()) return;
    startScanPath(path, 0, 3);
}

void MainWindow::openMftDump()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open MFT Dump"), QDir::homePath(),
                                                      tr("MFT dumps (*.mft *.bin);;All files (*.*)"));
    if (path.isEmpty()) return;
    startScanPath(path, 0, 2);
}

void MainWindow::dumpMftFile()
{
    if (scanning_ || dumping_ || closing_) return;
    const int idx = driveBox_->currentIndex();
    if (idx <= 0) {
        QMessageBox::information(this, tr("Dump MFT File"), tr("Please select a drive first."));
        return;
    }
    const wchar_t letter = wchar_t(driveBox_->itemData(idx).toInt());
    QString path = QFileDialog::getSaveFileName(this, tr("Dump MFT File"),
                                                QDir::homePath() + QStringLiteral("/mft.mft"),
                                                tr("MFT files (*.mft);;All files (*.*)"));
    if (path.isEmpty()) return;
    startMftDump(letter, path);
}

void MainWindow::startMftDump(wchar_t letter, const QString& path, DumpFunction dump)
{
    if (scanning_ || dumping_ || closing_) return;
    if (dumpWorker_.joinable()) dumpWorker_.join();
    dumping_ = true;
    dumpCancel_ = false;
    dumpProgress_ = new QProgressDialog(tr("Dumping MFT file, please wait..."), tr("Cancel"), 0, 0, this);
    dumpProgress_->setWindowModality(Qt::WindowModal);
    dumpProgress_->setMinimumDuration(0);
    dumpProgress_->setAutoClose(false);
    dumpProgress_->setAutoReset(false);
    connect(dumpProgress_, &QProgressDialog::canceled, this, [this] { dumpCancel_ = true; });
    dumpProgress_->show();
    dumpWorker_ = std::thread([this, letter, path, dump = std::move(dump)] {
        auto result = wt::runScanTask([&] {
            return dump(letter, path, dumpCancel_, [this](const QString& status) {
                QMetaObject::invokeMethod(this, [this, status] {
                    if (dumpProgress_) dumpProgress_->setLabelText(status);
                }, Qt::QueuedConnection);
            });
        });
        QMetaObject::invokeMethod(this, [this, path, result = std::move(result)] {
            if (dumpWorker_.joinable()) dumpWorker_.join();
            dumping_ = false;
            dumpProgress_->hide();
            dumpProgress_->deleteLater();
            dumpProgress_ = nullptr;
            if (closing_) { close(); return; }
            if (result.ok) statusLabel_->setText(tr("MFT data dumped to file %1").arg(path));
            else if (result.error == QLatin1String("canceled")) statusLabel_->setText(tr("MFT Dump cancelled..."));
            else {
                statusLabel_->setText(result.error);
                if (!batchMode_) QMessageBox::warning(this, tr("Dump MFT File"), result.error);
            }
        }, Qt::QueuedConnection);
    });
}

void MainWindow::selectMultiple()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Select Multiple"));
    dlg.resize(540, 380);
    auto* layout = new QVBoxLayout(&dlg);
    auto* list = new QListWidget(&dlg);
    auto addItem = [&](const QString& text, const QString& path, bool checked) {
        auto* it = new QListWidgetItem(text, list);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        it->setData(Qt::UserRole, path);
    };
    for (int i = 0; i < driveBox_->count(); ++i) {
        const int letter = driveBox_->itemData(i).toInt();
        if (letter <= 0) continue;
        addItem(driveBox_->itemText(i), QString(QChar(letter)) + QStringLiteral(":\\"), false);
    }
    layout->addWidget(list, 1);

    auto* row = new QHBoxLayout();
    auto* addBtn = new QPushButton(tr("Add Folder..."), &dlg);
    auto* rmBtn = new QPushButton(tr("Remove"), &dlg);
    auto* allBtn = new QPushButton(tr("Select All"), &dlg);
    auto* noneBtn = new QPushButton(tr("Select None"), &dlg);
    row->addWidget(addBtn);
    row->addWidget(rmBtn);
    row->addStretch(1);
    row->addWidget(allBtn);
    row->addWidget(noneBtn);
    layout->addLayout(row);

    connect(addBtn, &QPushButton::clicked, &dlg, [&] {
        const QString dir = QFileDialog::getExistingDirectory(&dlg, tr("Add Folder"), QStringLiteral("C:\\"));
        if (!dir.isEmpty()) addItem(dir, dir, true);
    });
    connect(rmBtn, &QPushButton::clicked, &dlg, [&] {
        delete list->currentItem();
    });
    connect(allBtn, &QPushButton::clicked, &dlg, [&] {
        for (int i = 0; i < list->count(); ++i) list->item(i)->setCheckState(Qt::Checked);
    });
    connect(noneBtn, &QPushButton::clicked, &dlg, [&] {
        for (int i = 0; i < list->count(); ++i) list->item(i)->setCheckState(Qt::Unchecked);
    });

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(bb);

    if (dlg.exec() != QDialog::Accepted) return;
    QStringList targets;
    for (int i = 0; i < list->count(); ++i) {
        auto* it = list->item(i);
        if (it->checkState() == Qt::Checked) targets << it->data(Qt::UserRole).toString();
    }
    if (targets.isEmpty()) return;
    startMultiScan(targets);
}

void MainWindow::startMultiScan(const QStringList& targets)
{
    if (scanning_ || dumping_ || closing_ || targets.isEmpty()) return;
    if (liveBusy_) {
        liveCancel_ = true;
        pendingScan_ = [this, targets] { startMultiScan(targets); };
        return;
    }
    if (worker_.joinable()) worker_.join();

    watchScanMode_ = -1; // preserve multi-scan and MFT selection during overflow recovery
    beginDeletionWatch(targets);

    scanning_ = true;
    cancel_ = false;
    currentTarget_ = targets.join(QStringLiteral("; "));
    displayingPreviousScan_ = bool(result_);
    setScanSource(true);
    scanPathEdit_->setText(QDir::toNativeSeparators(currentTarget_));
    setBusy(true);
    statusLabel_->setText(tr("Scanning..."));
    pathLabel_->setText(currentTarget_);

    wt::ScanOptions opt;
    opt.preferMft = true;
    opt.unit = unit_;
    opt.cancel = &cancel_;
    opt.status = [this](const QString& st) {
        QMetaObject::invokeMethod(this, [this, st] { statusLabel_->setText(st); },
                                  Qt::QueuedConnection);
    };

    worker_ = std::thread([this, targets, opt]() mutable {
        auto result = wt::runScanTask([&] {
            std::vector<wt::Tree> parts;
            uint64_t totalRecords = 0;
            int index = 0;
            for (const QString& target : targets) {
                if (cancel_.load(std::memory_order_relaxed)) break;
                ++index;
                opt.status(tr("Scanning %1 of %2: %3").arg(index).arg(targets.size()).arg(target));
                const bool isDrive = target.size() == 3 && target.at(1) == QLatin1Char(':');
                wt::ScanResult r = isDrive ? wt::scanDrive(target.at(0).unicode(), opt)
                                           : wt::scanFolder(target, opt);
                if (r.ok) {
                    totalRecords += r.records;
                    parts.push_back(std::move(r.tree));
                }
            }
            wt::ScanResult result;
            result.records = totalRecords;
            result.tree = wt::mergeTrees(parts, tr("Multiple selections"), &cancel_);
            result.ok = !parts.empty();
            if (!result.ok) result.error = tr("No targets scanned");
            return result;
        });
        if (cancel_.load()) { result.ok = false; result.error = QStringLiteral("canceled"); }
        auto shared = std::make_shared<wt::ScanResult>(std::move(result));
        QMetaObject::invokeMethod(this, [this, shared] { onScanFinished(shared); },
                                  Qt::QueuedConnection);
    });
}

void MainWindow::copyFilesToClipboard(bool cut)
{
    wt::Tree* t = displayTree();
    if (!t) return;
    QList<uint32_t> nodes = actionNodes();
    if (nodes.isEmpty() && fileView_->hasFocus()) nodes = selectedNodes(fileView_);
    if (nodes.isEmpty()) nodes = selectedNodes(fileView_);
    if (nodes.isEmpty()) return;

    QList<QUrl> urls;
    for (uint32_t n : nodes) {
        if (n >= t->nodes.size()) continue;
        const QString p = t->path(n);
        if (!p.isEmpty()) urls << QUrl::fromLocalFile(p);
    }
    if (urls.isEmpty()) return;
    auto* mime = new QMimeData();
    mime->setUrls(urls);
    if (cut) {
        QByteArray effect(4, '\0');
        effect[0] = 2;  // DROPEFFECT_MOVE
        mime->setData(QStringLiteral("Preferred DropEffect"), effect);
    }
    QApplication::clipboard()->setMimeData(mime);
    statusLabel_->setText(cut ? tr("Cut %1 item(s)").arg(urls.size())
                              : tr("Copied %1 item(s)").arg(urls.size()));
}

void MainWindow::checkForUpdatesNow()

{
    const QString url = settings_.value(QStringLiteral("updateUrl")).toString();
    if (url.trimmed().isEmpty()) {
        QMessageBox::information(this, tr("Check for updates"),
                                 tr("Update check is disabled: no endpoint configured."));
        return;
    }
    const wt::UpdateInfo info = wt::checkForUpdates(url);
    if (!info.ok) {
        QMessageBox::warning(this, tr("Error"),
                             tr("Error checking for updates: %1").arg(info.error));
        return;
    }
    const QString current = QCoreApplication::applicationVersion();
    if (wt::isNewerVersion(info.version, current)) {
        QString text = tr("An update is available (%1). Current version: %2.")
                           .arg(info.version, current);
        if (!info.infoUrl.isEmpty()) text += QStringLiteral("\n\n") + info.infoUrl;
        QMessageBox::information(this, tr("Check for updates"), text);
    } else {
        QMessageBox::information(this, tr("Check for updates"), tr("No update found."));
    }
}

void MainWindow::showAbout()



{
    QMessageBox::about(this, tr("About DiskCleaner"),
                       tr("<b>DiskCleaner 0.1</b><br/>A clean-room disk space analyzer "
                          "built with C++20 and Qt 6.<br/><br/>"
                          "Scans NTFS volumes through the Master File Table when elevated, "
                          "and falls back to a directory walk otherwise."));
}

void MainWindow::setUnit(int mode)
{
    unit_ = wt::UnitMode(mode);
    treeModel_->setUnit(unit_);
    fileModel_->setUnit(unit_);
    treemap_->setUnit(unit_);
    refreshExtensions();
    updateStats();
    onTreeSelectionChanged();
    updateTreemapBreadcrumb();
    settings_.setValue(QStringLiteral("unit"), mode);
}

void MainWindow::applySettings()
{
    restoreGeometry(settings_.value(QStringLiteral("geometry")).toByteArray());
    const QByteArray split = settings_.value(QStringLiteral("analysisSplitV2")).toByteArray();
    if (!split.isEmpty()) treeSplit_->restoreState(split);
    unit_ = wt::UnitMode(settings_.value(QStringLiteral("unit"), 0).toInt());
    decimals_ = settings_.value(QStringLiteral("decimals"), 1).toInt();
    treeModel_->setUnit(unit_);
    treeModel_->setDecimals(decimals_);
    fileModel_->setUnit(unit_);
    treemap_->setUnit(unit_);
    const bool names = settings_.value(QStringLiteral("treemapNames"), true).toBool();
    const bool alloc = settings_.value(QStringLiteral("treemapAllocated"), true).toBool();
    treemap_->setShowNames(names);
    treemap_->setShowUnknownSpace(settings_.value(QStringLiteral("treemapUnknown"), false).toBool());
    treemap_->setMinimumCellSize(settings_.value(QStringLiteral("treemapMinPixels"), 10).toInt());
    maxFilesBox_->setCurrentIndex(settings_.value(QStringLiteral("maxFilesIndex"), 1).toInt());
    showFoldersBox_->setChecked(settings_.value(QStringLiteral("showFolders"), false).toBool());
    {
        const int mode = settings_.value(QStringLiteral("dupMode"), 0).toInt();
        dupsBox_->setCurrentIndex(qBound(0, mode, 2));
        fileModel_->setDupMode(dupsBox_->currentData().toInt());
        dupsOnlyBox_->setChecked(settings_.value(QStringLiteral("dupOnly"), false).toBool());
    }

    showFreeSpace_ = settings_.value(QStringLiteral("treemapFree"), false).toBool();
    toggleFilterPanel(settings_.value(QStringLiteral("filterscanresults"), false).toBool());
    // Filters are session-only: never silently hide files on the next launch.
    settings_.remove(QStringLiteral("edFilterInclude_Text"));
    settings_.remove(QStringLiteral("edFilterExclude_Text"));
    filterInclude_->clear();
    filterExclude_->clear();
    treemap_->setShowFreeSpace(showFreeSpace_);
    sortCol_ = settings_.value(QStringLiteral("sortCol"), 2).toInt();
    sortOrder_ = Qt::SortOrder(settings_.value(QStringLiteral("sortOrder"), int(Qt::DescendingOrder)).toInt());

    const QByteArray treeHeader = settings_.value(QStringLiteral("treeHeaderV2")).toByteArray();
    if (!treeHeader.isEmpty()) treeView_->header()->restoreState(treeHeader);
    const QByteArray fileHeader = settings_.value(QStringLiteral("fileHeaderV2")).toByteArray();
    if (!fileHeader.isEmpty()) fileView_->horizontalHeader()->restoreState(fileHeader);
    treeView_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    fileView_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    fileView_->horizontalHeader()->setStretchLastSection(false);
    setSizeMetric(alloc);
    updateTreemapBreadcrumb();
}

void MainWindow::saveSettings()
{
    settings_.setValue(QStringLiteral("geometry"), saveGeometry());
    settings_.setValue(QStringLiteral("analysisSplitV2"), treeSplit_->saveState());
    settings_.setValue(QStringLiteral("unit"), int(unit_));
    settings_.setValue(QStringLiteral("decimals"), decimals_);
    settings_.setValue(QStringLiteral("maxFilesIndex"), maxFilesBox_->currentIndex());
    settings_.setValue(QStringLiteral("showFolders"), showFoldersBox_->isChecked());
    settings_.setValue(QStringLiteral("treeHeaderV2"), treeView_->header()->saveState());
    settings_.setValue(QStringLiteral("fileHeaderV2"), fileView_->horizontalHeader()->saveState());
    settings_.setValue(QStringLiteral("treemapFree"), showFreeSpace_);
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    closing_ = true;
    cancel_ = true;
    liveCancel_ = true;
    dumpCancel_ = true;
    pendingScan_ = {};
    deletionTimer_->stop();
    searchTimer_->stop();
    extTimer_->stop();
    // Completion callbacks retry close. Never join an active I/O task on the UI thread.
    if (scanning_ || liveBusy_ || dumping_) {
        e->ignore();
        return;
    }
    deletionWatcher_->stop();
    stopLiveUpdates();
    saveSettings();
    QMainWindow::closeEvent(e);
}

void MainWindow::setLanguage(const QString& code)
{
    const QString current = qApp->property("language").isValid()
                                ? qApp->property("language").toString()
                                : settings_.value(QStringLiteral("language"), QStringLiteral("en")).toString();
    if (current == code)
        return;
    settings_.setValue(QStringLiteral("language"), code);
    if (QMessageBox::question(this, tr("Language"),
                              tr("Restart DiskCleaner to apply the new language?"),
                              QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
        QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList());
        QApplication::quit();
    }
}

void MainWindow::restartAsAdmin()
{
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    const HINSTANCE r = ShellExecuteW(nullptr, L"runas", exe, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) > 32) QApplication::quit();
}
































