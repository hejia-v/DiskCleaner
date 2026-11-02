#include "deletionwatcher.h"

#include <windows.h>
#include <QDir>
#include <QFileInfo>
#include <QWinEventNotifier>
#include <array>
#include <utility>

struct DeletionWatcher::Watch {
    QString path;
    QString selfName; // Nonrecursive parent subscription detects removal of scan root itself.
    HANDLE directory = INVALID_HANDLE_VALUE;
    OVERLAPPED io{};
    alignas(DWORD) std::array<std::byte, 64 * 1024> buffer{};
    std::unique_ptr<QWinEventNotifier> notifier;
    bool pending = false;
    QString renameOld;

    ~Watch()
    {
        notifier.reset();
        if (directory != INVALID_HANDLE_VALUE) {
            if (pending) {
                CancelIoEx(directory, &io);
                DWORD ignored = 0;
                GetOverlappedResult(directory, &io, &ignored, TRUE);
            }
            CloseHandle(directory);
        }
        if (io.hEvent) CloseHandle(io.hEvent);
    }
};

DeletionWatcher::DeletionWatcher(QObject* parent) : QObject(parent)
{
    timer_.setSingleShot(true);
    timer_.setInterval(200);
    connect(&timer_, &QTimer::timeout, this, &DeletionWatcher::flush);
}

DeletionWatcher::~DeletionWatcher() { stop(); }

void DeletionWatcher::stop()
{
    timer_.stop();
    watches_.clear();
    pending_.clear();
    changed_.clear();
    renames_.clear();
    created_.clear();
    lostChanges_ = false;
}

void DeletionWatcher::start(const QStringList& roots)
{
    stop();
    QStringList seen;
    for (const auto& root : roots) {
        const QString path = QDir::toNativeSeparators(QFileInfo(root).absoluteFilePath());
        if (seen.contains(path, Qt::CaseInsensitive)) continue;
        seen.append(path);
        if (!addWatch(path)) continue;
        const QFileInfo info(path);
        if (!info.fileName().isEmpty()) addWatch(info.absolutePath(), info.fileName());
    }
}

bool DeletionWatcher::addWatch(const QString& path, const QString& selfName)
{
    auto watch = std::make_unique<Watch>();
    watch->path = QDir::toNativeSeparators(path);
    watch->selfName = selfName;
    watch->directory = CreateFileW(reinterpret_cast<LPCWSTR>(watch->path.utf16()),
        FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (watch->directory == INVALID_HANDLE_VALUE) {
        emit watchError(tr("Cannot monitor %1 (Windows error %2)").arg(path).arg(GetLastError()));
        return false;
    }
    watch->io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!watch->io.hEvent) {
        emit watchError(tr("Cannot create file notification event (%1)").arg(GetLastError()));
        return false;
    }
    watch->notifier = std::make_unique<QWinEventNotifier>(watch->io.hEvent);
    connect(watch->notifier.get(), &QWinEventNotifier::activated, this,
            [this, ptr = watch.get()] { ready(*ptr); });
    if (!arm(*watch)) return false;
    watches_.push_back(std::move(watch));
    return true;
}

bool DeletionWatcher::arm(Watch& watch)
{
    ResetEvent(watch.io.hEvent);
    watch.pending = ReadDirectoryChangesW(watch.directory, watch.buffer.data(),
        DWORD(watch.buffer.size()), watch.selfName.isEmpty(),
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            (watch.selfName.isEmpty() ? FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                       FILE_NOTIFY_CHANGE_ATTRIBUTES : 0),
        nullptr, &watch.io, nullptr);
    const DWORD error = GetLastError();
    watch.notifier->setEnabled(watch.pending);
    if (!watch.pending)
        emit watchError(tr("Live monitoring stopped for %1 (Windows error %2)")
                            .arg(watch.path).arg(error));
    return watch.pending;
}

void DeletionWatcher::ready(Watch& watch)
{
    watch.notifier->setEnabled(false);
    DWORD bytes = 0;
    const bool ok = GetOverlappedResult(watch.directory, &watch.io, &bytes, FALSE);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    watch.pending = false;
    if (!ok && error != ERROR_NOTIFY_ENUM_DIR) {
        // A removed root invalidates its recursive handle; its parent watch also covers this.
        const DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(watch.path.utf16()));
        const DWORD pathError = GetLastError();
        if (attrs == INVALID_FILE_ATTRIBUTES &&
            (pathError == ERROR_FILE_NOT_FOUND || pathError == ERROR_PATH_NOT_FOUND))
            enqueue(watch.path);
        else if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) && watch.selfName.isEmpty()) {
            // Delete + recreate can race the completion of the old directory handle.
            CloseHandle(watch.directory);
            watch.directory = CreateFileW(reinterpret_cast<LPCWSTR>(watch.path.utf16()), FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (watch.directory != INVALID_HANDLE_VALUE && arm(watch)) {
                created_.insert(watch.path);
                enqueue(watch.path);
            } else if (watch.directory == INVALID_HANDLE_VALUE)
                emit watchError(tr("Cannot monitor %1 (Windows error %2)").arg(watch.path).arg(GetLastError()));
        } else
            emit watchError(tr("Live monitoring stopped for %1 (Windows error %2)")
                                .arg(watch.path).arg(error));
        return;
    }

    // Copy completed buffer, then immediately rearm before parsing/batching events.
    alignas(DWORD) const auto completed = watch.buffer;
    arm(watch);
    if (!bytes || error == ERROR_NOTIFY_ENUM_DIR) { overflow(); return; }
    for (DWORD offset = 0; offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= bytes;) {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(completed.data() + offset);
        const DWORD header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        if (info->FileNameLength % sizeof(wchar_t) || info->FileNameLength > bytes - offset - header) {
            overflow(); break;
        }
        const QString name = QString::fromWCharArray(info->FileName, info->FileNameLength / sizeof(wchar_t));
        const QString path = QDir::toNativeSeparators(QDir(watch.path).filePath(name));
        const bool relevant = watch.selfName.isEmpty() || name.compare(watch.selfName, Qt::CaseInsensitive) == 0;
        if (info->Action == FILE_ACTION_RENAMED_NEW_NAME && !watch.renameOld.isEmpty()) {
            const QString old = std::exchange(watch.renameOld, {});
            renames_.append({old, path});
            changed_.insert(wt::livePathKey(path), path);
            // Parent subscription follows a renamed scan root; recursive handles remain valid.
            if (!watch.selfName.isEmpty()) {
                for (auto& other : watches_) {
                    const auto key = wt::livePathKey(other->path);
                    const auto oldKey = wt::livePathKey(old);
                    if (key == oldKey || key.startsWith(oldKey + '/'))
                        other->path = path + other->path.mid(old.size());
                }
                watch.selfName = name;
            }
        } else {
            if (!watch.renameOld.isEmpty()) { enqueue(watch.renameOld); watch.renameOld.clear(); }
            if (relevant) {
                changed_.insert(wt::livePathKey(path), path);
                if (info->Action == FILE_ACTION_ADDED) created_.insert(path);
                if (info->Action == FILE_ACTION_ADDED && !watch.selfName.isEmpty()) {
                    // A deleted scan root may be recreated. Reuse its inactive recursive slot.
                    for (auto& other : watches_) {
                        if (!other->selfName.isEmpty() || other->pending ||
                            wt::livePathKey(other->path) != wt::livePathKey(path)) continue;
                        if (other->directory != INVALID_HANDLE_VALUE) CloseHandle(other->directory);
                        other->directory = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_LIST_DIRECTORY,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
                        if (other->directory != INVALID_HANDLE_VALUE) arm(*other);
                    }
                }
                if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) watch.renameOld = path;
                else if (info->Action == FILE_ACTION_REMOVED) enqueue(path);
            }
        }
        if (changed_.size() + renames_.size() > 32768) overflow();
        if (!timer_.isActive() && (!changed_.isEmpty() || !renames_.isEmpty())) timer_.start();
        if (!info->NextEntryOffset) break;
        if (info->NextEntryOffset < header || info->NextEntryOffset > bytes - offset) {
            overflow(); break;
        }
        offset += info->NextEntryOffset;
    }
}

void DeletionWatcher::enqueue(const QString& path)
{
    if (!lostChanges_) pending_.insert(QDir::toNativeSeparators(path).toCaseFolded());
    if (!lostChanges_) changed_.insert(wt::livePathKey(path), path);
    if (pending_.size() > 32768) overflow();
    if (!timer_.isActive()) timer_.start(); // bounded latency even during continuous deletion
}

void DeletionWatcher::overflow()
{
    pending_.clear();
    changed_.clear();
    renames_.clear();
    created_.clear();
    for (auto& watch : watches_) watch->renameOld.clear();
    lostChanges_ = true;
    if (!timer_.isActive()) timer_.start();
}

void DeletionWatcher::flush()
{
    for (auto& watch : watches_) {
        if (!watch->renameOld.isEmpty()) { enqueue(watch->renameOld); watch->renameOld.clear(); }
    }
    const bool lost = lostChanges_;
    const QStringList paths = pending_.values();
    wt::FileChanges changes{changed_.values(), std::move(renames_), created_.values()};
    pending_.clear();
    changed_.clear();
    renames_.clear();
    created_.clear();
    timer_.stop();
    lostChanges_ = false;
    if (lost) emit rescanRequired();
    else {
        if (!changes.paths.isEmpty() || !changes.renames.isEmpty()) emit changesReady(changes);
        if (!paths.isEmpty()) emit pathsRemoved(paths);
    }
}
