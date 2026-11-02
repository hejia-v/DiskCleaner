#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <QString>
#include <QStringList>
#include <QHash>

namespace wt {

constexpr uint32_t kNone = 0xFFFFFFFFu;

enum class UnitMode { Dynamic = 0, Bytes = 1, KB = 2, MB = 3, GB = 4, TB = 5 };

// Compact node record; names live in a shared arena.
struct Node {
    uint64_t size = 0;      // logical size (subtree total for folders)
    uint64_t alloc = 0;     // allocated size (subtree total for folders)
    int64_t mtime = 0;      // FILETIME (100ns since 1601-01-01)
    int64_t ctime = 0;      // creation time
    int64_t atime = 0;      // last access time
    uint32_t parent = kNone;
    uint32_t childBegin = 0;
    uint32_t childCount = 0;
    uint32_t childCapacity = 0; // geometric growth for incremental insertions
    uint32_t nameOff = 0;
    uint32_t nameLen = 0;
    uint32_t rowInParent = 0;
    uint32_t files = 0;     // descendant files
    uint32_t folders = 0;   // descendant folders
    uint32_t mftRec = 0;
    uint32_t attrs = 0;
    uint8_t isDir = 0;
    uint8_t removed = 0;   // stable node IDs; disconnected tombstones are skipped by flat views
    uint8_t pad[2] = {0, 0};
};

struct Removal {
    std::vector<uint32_t> nodes;
    std::vector<uint32_t> ancestors;
};

struct Tree {
    std::vector<Node> nodes;
    std::vector<char16_t> names;
    std::vector<uint32_t> children;  // global child index array
    uint32_t root = kNone;
    // Created lazily for directories touched by live updates, not for every scanned file.
    mutable QHash<uint32_t, QHash<QString, uint32_t>> liveChildren;

    void clear()
    {
        nodes.clear();
        names.clear();
        children.clear();
        liveChildren.clear();
        root = kNone;
    }
    QString name(uint32_t i) const;
    QString path(uint32_t i) const;
    std::vector<uint32_t> findPaths(const QStringList& paths) const;
    Removal removeNodes(const std::vector<uint32_t>& ids);
    uint32_t survivingAncestor(uint32_t id) const;
    void finalize();  // build child ranges, aggregates, default sort
    void sortChildrenBy(uint32_t node, int column, bool ascending);
    void sortRecursive(int column, bool ascending);
};

struct ScanResult {
    Tree tree;
    QString error;
    bool ok = false;
    uint64_t records = 0;
    uint64_t parsed = 0;
    uint64_t noDataFiles = 0;     // file records without an unnamed $DATA attribute
    uint64_t zeroSizeFiles = 0;   // file records whose resolved size is 0
    uint64_t sumDataSizes = 0;    // total of $DATA-derived sizes
    uint64_t sumFileNameSizes = 0;  // total of FILE_NAME-derived sizes (diagnostic)
    double seconds = 0.0;
    bool usesMft = false;
};

using StatusFn = std::function<void(const QString&)>;

struct ScanOptions {
    bool preferMft = true;
    UnitMode unit = UnitMode::Dynamic;
    StatusFn status;
    std::atomic_bool* cancel = nullptr;
};

ScanResult scanDrive(wchar_t driveLetter, const ScanOptions& opt);
ScanResult scanMftDump(const QString& path, const ScanOptions& opt);
ScanResult importCsvTree(const QString& path, const ScanOptions& opt);
ScanResult dumpMftToFile(wchar_t driveLetter, const QString& outputPath, std::atomic_bool& cancel,
                         const StatusFn& status);
bool writeSyntheticMft(const QString& path, int recordCount, QString& error);
ScanResult scanFolder(const QString& path, const ScanOptions& opt);

// Builds a filtered copy of `src` keeping nodes whose name (or full path when
// matchPath is true) contains `include`, minus those containing `exclude`.
// Directories are kept when any descendant survives; sizes are recomputed.
Tree filterTree(const Tree& src, const QString& include, const QString& exclude, bool matchPath);
Tree mergeTrees(std::vector<Tree>& parts, const QString& rootName);

QString formatSize(uint64_t bytes, UnitMode mode, int decimals);
QString attributesText(uint32_t attrs);
bool isProcessElevated();
bool selfTest(QString& out);
bool moveToRecycleBin(const QStringList& paths);
bool renameFileOrFolder(const QString& path, const QString& newName, QString& outNewPath,
                        QString& error);

}  // namespace wt





