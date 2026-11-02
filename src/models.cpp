#include "models.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QIcon>
#include <QMimeData>
#include <QUrl>
#include <QStyle>
#include <climits>
#include <unordered_map>
#include <algorithm>

namespace {
QString fileTimeToString(int64_t ft)
{
    if (ft <= 0) return {};
    const qint64 ms = ft / 10000 - 11644473600000LL;
    if (ms <= 0) return {};
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString percentText(uint64_t part, uint64_t whole)
{
    if (whole == 0) return QStringLiteral("-");
    const double p = 100.0 * double(part) / double(whole);
    return QStringLiteral("%1 %").arg(p, 0, 'f', 1);
}
}  // namespace

// ------------------------------------------------------------------ TreeModel

TreeModel::TreeModel(QObject* parent) : QAbstractItemModel(parent) {}

void TreeModel::setTree(wt::Tree* t)
{
    beginResetModel();
    tree_ = t;
    endResetModel();
}

std::vector<uint32_t> TreeModel::compactTree()
{
    beginResetModel();
    auto mapping = tree_->compact();
    endResetModel();
    return mapping;
}

void TreeModel::setShowAllocated(bool on)
{
    if (showAllocated_ == on) return;
    beginResetModel();
    showAllocated_ = on;
    endResetModel();
}

uint32_t TreeModel::nodeOf(const QModelIndex& idx) const
{
    if (!idx.isValid() || !tree_) return wt::kNone;
    return uint32_t(idx.internalId());
}

QModelIndex TreeModel::index(int row, int column, const QModelIndex& parent) const
{
    if (!tree_ || row < 0 || column < 0 || column >= 9) return {};
    uint32_t node;
    if (!parent.isValid()) {
        if (row != 0) return {};
        node = tree_->root;
    } else {
        const uint32_t p = uint32_t(parent.internalId());
        if (p >= tree_->nodes.size()) return {};
        const wt::Node& pn = tree_->nodes[p];
        if (row >= int(pn.childCount)) return {};
        node = tree_->children[pn.childBegin + uint32_t(row)];
    }
    if (node == wt::kNone) return {};
    return createIndex(row, column, quintptr(node));
}

QModelIndex TreeModel::parent(const QModelIndex& child) const
{
    if (!child.isValid() || !tree_) return {};
    const uint32_t node = uint32_t(child.internalId());
    if (node >= tree_->nodes.size() || node == tree_->root) return {};
    const wt::Node& n = tree_->nodes[node];
    if (n.parent == wt::kNone || n.parent >= tree_->nodes.size()) return {};
    return indexForNode(n.parent);
}

int TreeModel::rowCount(const QModelIndex& parent) const
{
    if (!tree_ || tree_->nodes.empty()) return 0;
    if (!parent.isValid()) return 1;
    if (parent.column() > 0) return 0;
    const uint32_t node = uint32_t(parent.internalId());
    if (node >= tree_->nodes.size()) return 0;
    return int(tree_->nodes[node].childCount);
}

int TreeModel::columnCount(const QModelIndex&) const { return 9; }

QVariant TreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || !tree_) return {};
    const uint32_t node = uint32_t(index.internalId());
    if (node >= tree_->nodes.size()) return {};
    const wt::Node& n = tree_->nodes[node];
    const int col = index.column();

    if (role == Qt::DisplayRole) {
        switch (col) {
        case 0: {
            const QString name = tree_->name(node);
            const QString leaf = node == tree_->root ? QFileInfo(name).fileName() : name;
            return leaf.isEmpty() ? name : leaf;
        }
        case 1: {
            const auto& root = tree_->nodes[tree_->root];
            return percentText(showAllocated_ ? n.alloc : n.size,
                               showAllocated_ ? root.alloc : root.size);
        }
        case 2: return wt::formatSize(n.size, unit_, decimals_);
        case 3: return wt::formatSize(n.alloc, unit_, decimals_);
        case 4: return QString::number(n.childCount);
        case 5: return QString::number(n.isDir ? n.files : 1);
        case 6: return QString::number(n.folders);
        case 7: return fileTimeToString(n.mtime);
        case 8: return wt::attributesText(n.attrs);
        default: return {};
        }
    }
    if (role == Qt::TextAlignmentRole) {
        if (col >= 1 && col <= 6) return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (role == Qt::DecorationRole && col == 0) {
        static const QIcon dirIcon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
        static const QIcon fileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
        return n.isDir ? dirIcon : fileIcon;
    }
    if (role == Qt::ToolTipRole) return tree_->path(node);
    if (role == Qt::UserRole) return QVariant::fromValue<qulonglong>(showAllocated_ ? n.alloc : n.size);
    if (role == Qt::UserRole + 1 && col == 1) {
        const auto& root = tree_->nodes[tree_->root];
        const auto whole = showAllocated_ ? root.alloc : root.size;
        return whole ? double(showAllocated_ ? n.alloc : n.size) / double(whole) : 0.0;
    }
    return {};
}

QVariant TreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    static const char* headers[] = {"Name", "% of Scan", "File size", "Space on disk", "Items",
                                    "File count", "Folders", "Modified", "Attributes"};
    if (section < 0 || section >= 9) return {};
    return tr(headers[section]);
}

Qt::ItemFlags TreeModel::flags(const QModelIndex& index) const
{
    Qt::ItemFlags f = QAbstractItemModel::flags(index);
    if (!index.isValid() || !tree_) return f;
    const uint32_t node = uint32_t(index.internalId());
    if (node == tree_->root) return f;
    f |= Qt::ItemIsDragEnabled;
    if (index.column() == 0) f |= Qt::ItemIsEditable;
    return f;
}

bool TreeModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::EditRole || !index.isValid() || !tree_ || index.column() != 0) return false;
    const uint32_t node = uint32_t(index.internalId());
    if (node >= tree_->nodes.size() || node == tree_->root) return false;
    const QString newName = value.toString().trimmed();
    if (newName.isEmpty() || newName == tree_->name(node)) return false;

    const QString oldPath = tree_->path(node);
    QString newPath;
    QString error;
    if (!wt::renameFileOrFolder(oldPath, newName, newPath, error)) {
        emit renameFailed(error);
        return false;
    }
    wt::Node& nd = tree_->nodes[node];
    tree_->liveChildren.remove(nd.parent);
    const char16_t* p = reinterpret_cast<const char16_t*>(newName.utf16());
    nd.nameOff = uint32_t(tree_->names.size());
    nd.nameLen = uint32_t(newName.size());
    tree_->names.insert(tree_->names.end(), p, p + newName.size());
    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    emit nodeRenamed(node);
    return true;
}

QStringList TreeModel::mimeTypes() const
{
    return {QStringLiteral("text/uri-list")};
}

QMimeData* TreeModel::mimeData(const QModelIndexList& indexes) const
{
    if (!tree_) return nullptr;
    auto* mime = new QMimeData();
    QStringList paths;
    for (const QModelIndex& idx : indexes) {
        if (!idx.isValid() || idx.column() != 0) continue;
        const uint32_t node = uint32_t(idx.internalId());
        if (node >= tree_->nodes.size()) continue;
        const QString p = tree_->path(node);
        if (!p.isEmpty() && !paths.contains(p)) paths << p;
    }
    QList<QUrl> urls;
    for (const QString& p : paths) urls << QUrl::fromLocalFile(p);
    mime->setUrls(urls);
    return mime;
}

void TreeModel::sort(int column, Qt::SortOrder order)
{
    if (!tree_) return;
    emit layoutAboutToBeChanged();
    tree_->sortRecursive(column == 1 ? (showAllocated_ ? 3 : 2) : column,
                         order == Qt::AscendingOrder);
    emit layoutChanged();
    emit sorted();
}

// -------------------------------------------------------------- FileListModel

wt::LiveDelta TreeModel::updateLive(const wt::LiveUpdate& update, int column, Qt::SortOrder order, bool allowInsert)
{
    if (!tree_) return {};
    beginResetModel();
    auto delta = wt::applyLiveUpdate(*tree_, update, allowInsert);
    for (auto node : delta.removal.ancestors)
        tree_->sortChildrenBy(node, column == 1 ? (showAllocated_ ? 3 : 2) : column,
                              order == Qt::AscendingOrder);
    endResetModel();
    return delta;
}

QModelIndex TreeModel::indexForNode(uint32_t node) const
{
    if (!tree_ || node >= tree_->nodes.size() ||
        (node != tree_->root && tree_->nodes[node].removed)) return {};
    return createIndex(node == tree_->root ? 0 : int(tree_->nodes[node].rowInParent), 0, quintptr(node));
}

wt::Removal TreeModel::removeNodes(const std::vector<uint32_t>& ids, int column, Qt::SortOrder order)
{
    if (!tree_ || ids.empty()) return {};
    beginResetModel();
    auto removed = tree_->removeNodes(ids);
    for (auto node : removed.ancestors)
        tree_->sortChildrenBy(node, column == 1 ? (showAllocated_ ? 3 : 2) : column,
                              order == Qt::AscendingOrder);
    endResetModel();
    return removed;
}

FileListModel::FileListModel(QObject* parent) : QAbstractTableModel(parent) {}

void FileListModel::setTree(wt::Tree* t)
{
    beginResetModel();
    tree_ = t;
    rows_.clear();
    totalFiles_ = 0;
    if (tree_ && tree_->root < tree_->nodes.size()) totalFiles_ = tree_->nodes[tree_->root].files;
    endResetModel();
    buildDupIndex();
}

void FileListModel::sortRows()
{
    if (!tree_ || rows_.isEmpty() || sortCol_ < 0) return;
    const int col = sortCol_;
    const bool asc = sortOrder_ == Qt::AscendingOrder;
    std::sort(rows_.begin(), rows_.end(), [&](uint32_t a, uint32_t b) {
        const wt::Node& x = tree_->nodes[a];
        const wt::Node& y = tree_->nodes[b];
        int cmp = 0;
        switch (col) {
        case 0:
            cmp = QString::compare(tree_->name(a), tree_->name(b), Qt::CaseInsensitive);
            break;
        case 1: {
            const uint32_t px = x.parent == wt::kNone ? a : x.parent;
            const uint32_t py = y.parent == wt::kNone ? b : y.parent;
            cmp = QString::compare(tree_->path(px), tree_->path(py), Qt::CaseInsensitive);
            break;
        }
        case 2: cmp = x.size < y.size ? -1 : (x.size > y.size ? 1 : 0); break;
        case 3: cmp = x.alloc < y.alloc ? -1 : (x.alloc > y.alloc ? 1 : 0); break;
        case 4: {
            const auto sx = showAllocated_ ? x.alloc : x.size;
            const auto sy = showAllocated_ ? y.alloc : y.size;
            cmp = sx < sy ? -1 : (sx > sy ? 1 : 0);
            break;
        }
        case 5: cmp = x.mtime < y.mtime ? -1 : (x.mtime > y.mtime ? 1 : 0); break;
        case 6: {
            const QString ax = wt::attributesText(x.attrs);
            const QString ay = wt::attributesText(y.attrs);
            cmp = QString::compare(ax, ay, Qt::CaseInsensitive);
            break;
        }
        case 7: {
            const uint16_t cx = a < dupCount_.size() ? dupCount_[a] : 0;
            const uint16_t cy = b < dupCount_.size() ? dupCount_[b] : 0;
            cmp = cx < cy ? -1 : (cx > cy ? 1 : 0);
            break;
        }
        case 8: {
            const uint64_t sx = a < dupSize_.size() ? dupSize_[a] : 0;
            const uint64_t sy = b < dupSize_.size() ? dupSize_[b] : 0;
            cmp = sx < sy ? -1 : (sx > sy ? 1 : 0);
            break;
        }
        default: break;
        }
        if (cmp == 0) cmp = QString::compare(tree_->name(a), tree_->name(b), Qt::CaseInsensitive);
        return asc ? cmp < 0 : cmp > 0;
    });
}

void FileListModel::sort(int column, Qt::SortOrder order)
{
    sortCol_ = column;
    sortOrder_ = order;
    if (rows_.isEmpty()) return;
    beginResetModel();
    sortRows();
    endResetModel();
}

void FileListModel::setUnit(wt::UnitMode m)
{
    unit_ = m;
    if (!rows_.isEmpty())
        emit dataChanged(index(0, 0), index(int(rows_.size()) - 1, 8));
}

void FileListModel::setShowAllocated(bool on)
{
    if (showAllocated_ == on) return;
    showAllocated_ = on;
    sort(sortCol_, sortOrder_);
    if (!rows_.isEmpty()) emit dataChanged(index(0, 0), index(int(rows_.size()) - 1, 8));
}

void FileListModel::setDupMode(int mode)
{
    if (mode == dupMode_) return;
    dupMode_ = mode;
    buildDupIndex();
    if (!rows_.isEmpty())
        emit dataChanged(index(0, 7), index(int(rows_.size()) - 1, 8));
}

void FileListModel::buildDupIndex()
{
    dupCount_.clear();
    dupSize_.clear();
    dupGroups_ = 0;
    if (!tree_ || dupMode_ == 0) return;

    const uint32_t n = uint32_t(tree_->nodes.size());
    dupCount_.assign(n, 0);
    dupSize_.assign(n, 0);
    std::vector<uint32_t> next(n, wt::kNone);
    std::unordered_map<uint64_t, uint32_t> head;
    head.reserve(size_t(n) / 4 + 16);

    for (uint32_t i = 1; i < n; ++i) {
        const wt::Node& nd = tree_->nodes[i];
        if (nd.removed || nd.isDir || nd.nameLen == 0) continue;
        uint64_t h = 1469598103934665603ull;  // FNV-1a over the UTF-16 name
        const char16_t* s = tree_->names.data() + nd.nameOff;
        for (uint32_t k = 0; k < nd.nameLen; ++k) {
            h ^= uint16_t(s[k]);
            h *= 1099511628211ull;
        }
        h ^= nd.size * 0x9E3779B97F4A7C15ull;
        if (dupMode_ == 2) h ^= uint64_t(nd.mtime) * 0xC2B2AE3D27D4EB4Full;
        const auto it = head.find(h);
        if (it == head.end()) {
            head.emplace(h, i);
        } else {
            next[i] = it->second;
            it->second = i;
        }
    }

    for (const auto& kv : head) {
        uint32_t count = 0;
        for (uint32_t p = kv.second; p != wt::kNone; p = next[p]) ++count;
        if (count < 2) continue;
        ++dupGroups_;
        const uint64_t groupSize = tree_->nodes[kv.second].size * count;
        for (uint32_t p = kv.second; p != wt::kNone; p = next[p]) {
            dupCount_[p] = uint16_t(std::min<uint32_t>(count, 65535));
            dupSize_[p] = groupSize;
        }
    }
}

void FileListModel::rebuild(const QString& filter, bool matchPath, bool includeFolders, int maxFiles,
                            bool duplicatesOnly)
{
    beginResetModel();
    rows_.clear();
    filter_ = filter;
    matchPath_ = matchPath;
    includeFolders_ = includeFolders;
    duplicatesOnly_ = duplicatesOnly;
    cap_ = maxFiles > 0 ? maxFiles : INT_MAX;
    scannedUntil_ = 1;
    appendRows();
    sortRows();
    endResetModel();
}

bool FileListModel::matches(uint32_t i) const
{
    const auto& node = tree_->nodes[i];
    if (node.removed || (node.isDir && !includeFolders_)) return false;
    if (duplicatesOnly_ && dupMode_ > 0 && (i >= dupCount_.size() || dupCount_[i] < 2)) return false;
    return filter_.isEmpty() || tree_->name(i).contains(filter_, Qt::CaseInsensitive) ||
           (matchPath_ && tree_->path(i).contains(filter_, Qt::CaseInsensitive));
}

void FileListModel::appendRows()
{
    if (!tree_) return;
    while (scannedUntil_ < tree_->nodes.size() && rows_.size() < cap_) {
        const auto node = scannedUntil_++;
        if (matches(node)) rows_.push_back(node);
    }
}

void FileListModel::refreshAfterRemoval()
{
    if (!tree_) return;
    beginResetModel();
    totalFiles_ = tree_->nodes[tree_->root].files;
    // Duplicate groups depend on all files; only this optional mode rebuilds its index.
    if (dupMode_ > 0) buildDupIndex();
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
                              [this](auto node) { return !matches(node); }), rows_.end());
    appendRows(); // refill a limited table from the previous scan cursor, not from node zero
    sortRows();
    endResetModel();
}

uint32_t FileListModel::nodeAt(int row) const
{
    if (row < 0 || row >= rows_.size()) return wt::kNone;
    return rows_[row];
}

void FileListModel::refreshAfterUpdate(const wt::LiveDelta& delta)
{
    if (!tree_) return;
    totalFiles_ = tree_->nodes[tree_->root].files;
    if (dupMode_ > 0) {
        buildDupIndex();
        rebuild(filter_, matchPath_, includeFolders_, cap_, duplicatesOnly_);
        return;
    }
    beginResetModel();
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [this](auto id) { return !matches(id); }), rows_.end());
    QSet<uint32_t> included(rows_.begin(), rows_.end());
    for (auto id : delta.changed)
        if (id < scannedUntil_ && id != tree_->root && !included.contains(id) && matches(id)) rows_.append(id);
    if (rows_.size() > cap_) {
        std::sort(rows_.begin(), rows_.end());
        rows_.resize(cap_);
        scannedUntil_ = std::min(scannedUntil_, rows_.back() + 1);
    }
    appendRows();
    sortRows();
    endResetModel();
}

int FileListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return rows_.size();
}

int FileListModel::columnCount(const QModelIndex&) const { return 9; }

QVariant FileListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || !tree_) return {};
    const uint32_t node = rows_[index.row()];
    const wt::Node& n = tree_->nodes[node];
    const int col = index.column();
    if (role == Qt::DisplayRole) {
        switch (col) {
        case 0: return tree_->name(node);
        case 1: {
            const uint32_t p = n.parent;
            return (p != wt::kNone && p < tree_->nodes.size()) ? tree_->path(p) : QString();
        }
        case 2: return wt::formatSize(n.size, unit_, 1);
        case 3: return wt::formatSize(n.alloc, unit_, 1);
        case 4: {
            const auto& root = tree_->nodes[tree_->root];
            return percentText(showAllocated_ ? n.alloc : n.size,
                               showAllocated_ ? root.alloc : root.size);
        }
        case 5: return fileTimeToString(n.mtime);
        case 6: return wt::attributesText(n.attrs);
        case 7: {
            const uint16_t c = node < dupCount_.size() ? dupCount_[node] : 0;
            return c >= 2 ? QString::number(c) : QString();
        }
        case 8: {
            const uint16_t c = node < dupCount_.size() ? dupCount_[node] : 0;
            const uint64_t sz = node < dupSize_.size() ? dupSize_[node] : 0;
            return c >= 2 ? wt::formatSize(sz, unit_, 1) : QString();
        }
        default: return {};
        }
    }
    if (role == Qt::TextAlignmentRole) {
        if (col >= 2 && col <= 4) return int(Qt::AlignRight | Qt::AlignVCenter);
        if (col == 7 || col == 8) return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }
    return {};
}

QVariant FileListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    static const char* headers[] = {"File Name", "Path", "File size", "Space on disk", "% of Scan",
                                    "Modified", "Attributes", "Dup Count", "Duplicate file size"};
    if (section < 0 || section >= 9) return {};
    return tr(headers[section]);
}
// ------------------------------------------------------------ ExtensionModel

namespace {
QString fileTypeFor(const QString& ext)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("txt"), QStringLiteral("Text Document")},
        {QStringLiteral("log"), QStringLiteral("Text Document")},
        {QStringLiteral("md"), QStringLiteral("Text Document")},
        {QStringLiteral("ini"), QStringLiteral("Configuration")},
        {QStringLiteral("cfg"), QStringLiteral("Configuration")},
        {QStringLiteral("exe"), QStringLiteral("Application")},
        {QStringLiteral("com"), QStringLiteral("Application")},
        {QStringLiteral("dll"), QStringLiteral("Application Extension")},
        {QStringLiteral("ocx"), QStringLiteral("Application Extension")},
        {QStringLiteral("sys"), QStringLiteral("System File")},
        {QStringLiteral("drv"), QStringLiteral("System File")},
        {QStringLiteral("jpg"), QStringLiteral("Image")},
        {QStringLiteral("jpeg"), QStringLiteral("Image")},
        {QStringLiteral("png"), QStringLiteral("Image")},
        {QStringLiteral("gif"), QStringLiteral("Image")},
        {QStringLiteral("bmp"), QStringLiteral("Image")},
        {QStringLiteral("ico"), QStringLiteral("Icon")},
        {QStringLiteral("svg"), QStringLiteral("Image")},
        {QStringLiteral("mp3"), QStringLiteral("Audio")},
        {QStringLiteral("wav"), QStringLiteral("Audio")},
        {QStringLiteral("flac"), QStringLiteral("Audio")},
        {QStringLiteral("mp4"), QStringLiteral("Video")},
        {QStringLiteral("avi"), QStringLiteral("Video")},
        {QStringLiteral("mkv"), QStringLiteral("Video")},
        {QStringLiteral("mov"), QStringLiteral("Video")},
        {QStringLiteral("zip"), QStringLiteral("Archive")},
        {QStringLiteral("7z"), QStringLiteral("Archive")},
        {QStringLiteral("rar"), QStringLiteral("Archive")},
        {QStringLiteral("gz"), QStringLiteral("Archive")},
        {QStringLiteral("cab"), QStringLiteral("Archive")},
        {QStringLiteral("pdf"), QStringLiteral("PDF Document")},
        {QStringLiteral("doc"), QStringLiteral("Word Document")},
        {QStringLiteral("docx"), QStringLiteral("Word Document")},
        {QStringLiteral("xls"), QStringLiteral("Excel Worksheet")},
        {QStringLiteral("xlsx"), QStringLiteral("Excel Worksheet")},
        {QStringLiteral("ppt"), QStringLiteral("PowerPoint")},
        {QStringLiteral("pptx"), QStringLiteral("PowerPoint")},
        {QStringLiteral("html"), QStringLiteral("HTML Document")},
        {QStringLiteral("htm"), QStringLiteral("HTML Document")},
        {QStringLiteral("css"), QStringLiteral("Style Sheet")},
        {QStringLiteral("js"), QStringLiteral("Script")},
        {QStringLiteral("json"), QStringLiteral("JSON File")},
        {QStringLiteral("xml"), QStringLiteral("XML Document")},
        {QStringLiteral("csv"), QStringLiteral("CSV File")},
        {QStringLiteral("tsv"), QStringLiteral("TSV File")},
        {QStringLiteral("bat"), QStringLiteral("Batch File")},
        {QStringLiteral("cmd"), QStringLiteral("Batch File")},
        {QStringLiteral("ps1"), QStringLiteral("Script")},
        {QStringLiteral("py"), QStringLiteral("Python Script")},
        {QStringLiteral("cpp"), QStringLiteral("C++ Source")},
        {QStringLiteral("h"), QStringLiteral("C/C++ Header")},
        {QStringLiteral("c"), QStringLiteral("C Source")},
        {QStringLiteral("qm"), QStringLiteral("Qt Translation")},
    };
    if (ext == QLatin1String("(No Extension)"))
        return QCoreApplication::translate("FileType", "File");
    const auto it = map.constFind(ext);
    if (it == map.constEnd()) return QCoreApplication::translate("FileType", "File");
    return QCoreApplication::translate("FileType", it.value().toUtf8().constData());
}
}  // namespace

ExtensionModel::ExtensionModel(QObject* parent) : QAbstractTableModel(parent) {}

void ExtensionModel::rebuild(wt::Tree* tree, uint32_t root, wt::UnitMode unit)
{
    beginResetModel();
    tree_ = tree;
    root_ = root;
    rows_.clear();
    unit_ = unit;
    if (tree && root < tree->nodes.size()) {
        QHash<QString, int> index;
        std::vector<uint32_t> stack;
        stack.push_back(root);
        while (!stack.empty()) {
            const uint32_t n = stack.back();
            stack.pop_back();
            const wt::Node& nd = tree->nodes[n];
            if (nd.isDir) {
                for (uint32_t i = 0; i < nd.childCount; ++i)
                    stack.push_back(tree->children[nd.childBegin + i]);
                continue;
            }
            const QString name = tree->name(n);
            const int dot = name.lastIndexOf(QLatin1Char('.'));
            const QString ext = (dot > 0 && dot < name.size() - 1)
                                    ? name.mid(dot + 1).toLower()
                                    : QStringLiteral("(No Extension)");
            int idx = index.value(ext, -1);
            if (idx < 0) {
                idx = rows_.size();
                index.insert(ext, idx);
                Row r;
                r.ext = ext;
                r.type = fileTypeFor(ext);
                rows_.push_back(r);
            }
            rows_[idx].size += nd.size;
            rows_[idx].alloc += nd.alloc;
            ++rows_[idx].files;
        }
        std::sort(rows_.begin(), rows_.end(),
                  [this](const Row& a, const Row& b) {
                      return showAllocated_ ? a.alloc > b.alloc : a.size > b.size;
                  });
    }
    endResetModel();
}

void ExtensionModel::applyRemoval(wt::Tree* tree, uint32_t root, const wt::Removal& removal,
                                  wt::UnitMode unit)
{
    if (tree != tree_ || root != root_ || unit != unit_) { rebuild(tree, root, unit); return; }
    beginResetModel();
    QHash<QString, int> byExtension;
    for (int i = 0; i < rows_.size(); ++i) byExtension.insert(rows_[i].ext, i);
    for (auto id : removal.nodes) {
        const auto& node = tree->nodes[id];
        if (node.isDir) continue;
        auto ancestor = id;
        while (ancestor != wt::kNone && ancestor != root) ancestor = tree->nodes[ancestor].parent;
        if (ancestor != root) continue;
        const auto name = tree->name(id);
        const int dot = name.lastIndexOf('.');
        const QString ext = dot > 0 && dot < name.size() - 1 ? name.mid(dot + 1).toLower()
                                                             : QStringLiteral("(No Extension)");
        const int index = byExtension.value(ext, -1);
        if (index < 0) continue;
        auto& row = rows_[index];
        row.size -= node.size;
        row.alloc -= node.alloc;
        --row.files;
    }
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [](const auto& row) { return !row.files; }), rows_.end());
    std::stable_sort(rows_.begin(), rows_.end(), [this](const auto& a, const auto& b) {
        return showAllocated_ ? a.alloc > b.alloc : a.size > b.size;
    });
    endResetModel();
}

QString ExtensionModel::extensionAt(int row) const
{
    return (row >= 0 && row < rows_.size()) ? rows_[row].ext : QString();
}

void ExtensionModel::applyUpdate(wt::Tree* tree, uint32_t root, const wt::LiveDelta& delta, wt::UnitMode unit)
{
    if (tree != tree_ || root != root_ || unit != unit_) { rebuild(tree, root, unit); return; }
    beginResetModel();
    QHash<QString, int> byExtension;
    for (int i = 0; i < rows_.size(); ++i) byExtension.insert(rows_[i].ext, i);
    const auto change = [&](const QString& name, uint64_t size, uint64_t alloc, bool add) {
        const int dot = name.lastIndexOf('.');
        const QString ext = dot > 0 && dot < name.size() - 1 ? name.mid(dot + 1).toLower() : QStringLiteral("(No Extension)");
        int index = byExtension.value(ext, -1);
        if (index < 0) {
            if (!add) return;
            index = int(rows_.size());
            Row row; row.ext = ext; row.type = fileTypeFor(ext);
            rows_.push_back(row); byExtension.insert(ext, index);
        }
        auto& row = rows_[index];
        if (add) { row.size += size; row.alloc += alloc; ++row.files; }
        else { row.size -= size; row.alloc -= alloc; --row.files; }
    };
    for (auto it = delta.files.cbegin(); it != delta.files.cend(); ++it) {
        const auto& before = it.value();
        if (std::find(before.ancestors.begin(), before.ancestors.end(), root) != before.ancestors.end())
            change(before.name, before.size, before.alloc, false);
        const auto& node = tree->nodes[it.key()];
        if (node.removed) continue;
        auto parent = it.key();
        while (parent != wt::kNone && parent != root) parent = tree->nodes[parent].parent;
        if (parent == root) change(tree->name(it.key()), node.size, node.alloc, true);
    }
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [](const auto& row) { return !row.files; }), rows_.end());
    std::stable_sort(rows_.begin(), rows_.end(), [this](const auto& a, const auto& b) {
        return showAllocated_ ? a.alloc > b.alloc : a.size > b.size;
    });
    endResetModel();
}

int ExtensionModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : rows_.size();
}

void ExtensionModel::setShowAllocated(bool on)
{
    if (showAllocated_ == on) return;
    beginResetModel();
    showAllocated_ = on;
    std::stable_sort(rows_.begin(), rows_.end(), [this](const auto& a, const auto& b) {
        return showAllocated_ ? a.alloc > b.alloc : a.size > b.size;
    });
    endResetModel();
}

int ExtensionModel::columnCount(const QModelIndex&) const { return 6; }

QVariant ExtensionModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    const Row& r = rows_[index.row()];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0:
            return r.ext == QLatin1String("(No Extension)") ? tr("(No Extension)") : r.ext;
        case 1: return r.type;
        case 2: {
            const auto& root = tree_->nodes[tree_->root];
            return percentText(showAllocated_ ? r.alloc : r.size,
                               showAllocated_ ? root.alloc : root.size);
        }
        case 3: return wt::formatSize(r.size, unit_, 1);
        case 4: return wt::formatSize(r.alloc, unit_, 1);
        case 5: return QString::number(r.files);
        default: return {};
        }
    }
    if (role == Qt::TextAlignmentRole) {
        if (index.column() == 2 || index.column() == 3 || index.column() == 4 ||
            index.column() == 5)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }
    return {};
}

QVariant ExtensionModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    static const char* headers[] = {"Extension", "File Type", "% of Scan", "File size", "Space on disk",
                                    "Files"};
    if (section < 0 || section >= 6) return {};
    return tr(headers[section]);
}






