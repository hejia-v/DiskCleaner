#pragma once

#include <QAbstractItemModel>
#include <QAbstractTableModel>
#include <QVector>
#include <climits>

#include "core.h"
#include "liveupdate.h"

class TreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit TreeModel(QObject* parent = nullptr);

    void setTree(wt::Tree* tree);
    wt::Tree* tree() const { return tree_; }
    uint32_t nodeOf(const QModelIndex& idx) const;
    QModelIndex indexForNode(uint32_t node) const;
    wt::Removal removeNodes(const std::vector<uint32_t>& ids, int sortColumn, Qt::SortOrder order);
    wt::LiveDelta updateLive(const wt::LiveUpdate& update, int sortColumn, Qt::SortOrder order, bool allowInsert = true);

    QModelIndex index(int row, int column, const QModelIndex& parent) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent) const override;
    int columnCount(const QModelIndex& parent) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void sort(int column, Qt::SortOrder order) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    QStringList mimeTypes() const override;

    void setUnit(wt::UnitMode mode)
    {
        unit_ = mode;
        if (rowCount(QModelIndex()) > 0)
            emit dataChanged(index(0, 0, QModelIndex()), index(0, 8, QModelIndex()));
    }
    void setDecimals(int d) { decimals_ = d; }
    void setShowAllocated(bool on);
    wt::UnitMode unit() const { return unit_; }

signals:
    void sorted();
    void renameFailed(const QString& error);
    void nodeRenamed(uint32_t node);

private:
    wt::Tree* tree_ = nullptr;
    wt::UnitMode unit_ = wt::UnitMode::Dynamic;
    int decimals_ = 1;
    bool showAllocated_ = false;
};

class FileListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit FileListModel(QObject* parent = nullptr);

    void setTree(wt::Tree* tree);
    void rebuild(const QString& filter, bool matchPath, bool includeFolders, int maxFiles,
                 bool duplicatesOnly);
    uint32_t nodeAt(int row) const;
    void refreshAfterRemoval();
    void refreshAfterUpdate(const wt::LiveDelta& delta);

    // 0 = don't locate duplicates, 1 = name+size, 2 = name+size+date
    void setDupMode(int mode);
    int dupMode() const { return dupMode_; }
    int duplicateGroups() const { return dupGroups_; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void sort(int column, Qt::SortOrder order) override;

    void setUnit(wt::UnitMode m);
    void setShowAllocated(bool on);
    int totalFiles() const { return int(totalFiles_); }

private:
    void buildDupIndex();
    void sortRows();
    bool matches(uint32_t node) const;
    void appendRows();
    QString filter_;
    bool matchPath_ = false, includeFolders_ = false, duplicatesOnly_ = false;
    int cap_ = INT_MAX;
    uint32_t scannedUntil_ = 1;
    int sortCol_ = -1;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;

    wt::Tree* tree_ = nullptr;
    QVector<uint32_t> rows_;
    uint64_t totalFiles_ = 0;
    wt::UnitMode unit_ = wt::UnitMode::Dynamic;
    int dupMode_ = 0;
    bool showAllocated_ = false;
    int dupGroups_ = 0;
    std::vector<uint16_t> dupCount_;
    std::vector<uint64_t> dupSize_;
};

// Aggregated per-extension statistics for the currently selected subtree.
class ExtensionModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit ExtensionModel(QObject* parent = nullptr);

    void rebuild(wt::Tree* tree, uint32_t root, wt::UnitMode unit);
    void applyRemoval(wt::Tree* tree, uint32_t root, const wt::Removal& removal, wt::UnitMode unit);
    void applyUpdate(wt::Tree* tree, uint32_t root, const wt::LiveDelta& delta, wt::UnitMode unit);
    QString extensionAt(int row) const;
    void setShowAllocated(bool on);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    struct Row {
        QString ext;
        QString type;
        uint64_t size = 0;
        uint64_t alloc = 0;
        uint32_t files = 0;
    };
    QVector<Row> rows_;
    bool showAllocated_ = false;
    wt::Tree* tree_ = nullptr;
    uint32_t root_ = wt::kNone;
    wt::UnitMode unit_ = wt::UnitMode::Dynamic;
};



