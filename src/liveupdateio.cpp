#include <windows.h>
#include "liveupdate.h"
#include <QDir>
#include <QFileInfo>
#include <algorithm>

namespace wt {
namespace {
QString nativeLongPath(const QString& path)
{
    QString p = QDir::toNativeSeparators(path);
    if (p.startsWith("\\\\?\\")) return p;
    if (p.startsWith("\\\\")) return QStringLiteral("\\\\?\\UNC\\") + p.mid(2);
    return QStringLiteral("\\\\?\\") + p;
}
int64_t fileTime(FILETIME t) { return int64_t((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime); }
LiveEntry probe(const QString& path, QHash<QString, uint64_t>& clusters)
{
    LiveEntry entry;
    entry.path = QDir::toNativeSeparators(path);
    entry.name = QFileInfo(path).fileName();
    const QString native = nativeLongPath(path);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(reinterpret_cast<LPCWSTR>(native.utf16()), GetFileExInfoStandard, &data)) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) entry.state = LiveEntry::Missing;
        return entry;
    }
    // Preserve the filesystem's spelling, including a case-only rename.
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(reinterpret_cast<LPCWSTR>(native.utf16()), &found);
    if (search != INVALID_HANDLE_VALUE) { entry.name = QString::fromWCharArray(found.cFileName); FindClose(search); }
    auto& node = entry.metadata;
    node.attrs = data.dwFileAttributes;
    node.isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    node.mtime = fileTime(data.ftLastWriteTime);
    node.ctime = fileTime(data.ftCreationTime);
    node.atime = fileTime(data.ftLastAccessTime);
    if (!node.isDir) {
        node.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        // Handle metadata reports current EOF/allocation even for a growing, open file.
        HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        bool standardRead = false;
        if (file != INVALID_HANDLE_VALUE) {
            FILE_STANDARD_INFO standard{};
            if (GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard))) {
                node.size = uint64_t(standard.EndOfFile.QuadPart);
                node.alloc = uint64_t(standard.AllocationSize.QuadPart);
                standardRead = true;
            }
            CloseHandle(file);
        } else if (GetLastError() == ERROR_SHARING_VIOLATION) return entry;
        if (!standardRead) {
            wchar_t volume[MAX_PATH + 1]{};
            uint64_t cluster = 4096;
            if (GetVolumePathNameW(reinterpret_cast<LPCWSTR>(entry.path.utf16()), volume, MAX_PATH)) {
                const QString key = QString::fromWCharArray(volume);
                if (!clusters.contains(key)) {
                    DWORD sectors = 0, bytes = 0, freeClusters = 0, totalClusters = 0;
                    if (GetDiskFreeSpaceW(volume, &sectors, &bytes, &freeClusters, &totalClusters))
                        cluster = uint64_t(sectors) * bytes;
                    clusters.insert(key, cluster);
                }
                cluster = clusters.value(key);
            }
            node.alloc = (node.size + cluster - 1) / cluster * cluster;
        }
    }
    entry.state = LiveEntry::Present;
    return entry;
}
}

LiveUpdate readLiveUpdate(const FileChanges& changes, const QSet<QString>& knownDirectories,
                          std::atomic_bool& cancel)
{
    LiveUpdate result;
    result.renames = changes.renames;
    QHash<QString, QString> paths;
    QSet<QString> known = knownDirectories;
    for (const auto& path : changes.createdPaths) known.remove(livePathKey(path));
    for (const auto& path : changes.paths) paths.insert(livePathKey(path), path);
    for (const auto& rename : changes.renames) {
        const auto oldKey = livePathKey(rename.first), newKey = livePathKey(rename.second);
        const auto snapshot = paths.values();
        for (const auto& path : snapshot) {
            const auto key = livePathKey(path);
            if (key == oldKey || key.startsWith(oldKey + '/')) {
                const QString moved = rename.second + QDir::toNativeSeparators(path).mid(QDir::toNativeSeparators(rename.first).size());
                paths.insert(livePathKey(moved), moved);
            }
        }
        if (known.contains(oldKey)) known.insert(newKey);
        paths.insert(oldKey, rename.first);
        paths.insert(newKey, rename.second);
    }
    auto ordered = paths.values();
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.size() < b.size(); });
    QSet<QString> scanned;
    QHash<QString, uint64_t> clusters;
    for (const auto& path : ordered) {
        if (cancel.load()) break;
        const auto key = livePathKey(path);
        bool covered = false;
        auto parentKey = key;
        while (parentKey.lastIndexOf('/') > 1) {
            parentKey.truncate(parentKey.lastIndexOf('/'));
            if (scanned.contains(parentKey)) { covered = true; break; }
        }
        if (covered) continue;
        auto entry = probe(path, clusters);
        if (entry.state == LiveEntry::Unavailable) result.retryPaths.append(path);
        if (entry.state == LiveEntry::Present && entry.metadata.isDir && !known.contains(key) &&
            !(entry.metadata.attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            ScanOptions options;
            options.preferMft = false;
            options.cancel = &cancel;
            auto scan = scanFolder(path, options);
            if (scan.ok) { entry.subtree = std::move(scan.tree); scanned.insert(key); }
            else { entry.state = LiveEntry::Unavailable; result.retryPaths.append(path); }
        }
        result.entries.push_back(std::move(entry));
    }
    return result;
}
}
