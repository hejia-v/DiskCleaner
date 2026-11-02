#pragma once

#include "core.h"
#include <QHash>
#include <QPair>
#include <QSet>

namespace wt {
QString livePathKey(const QString& path);
struct FileChanges {
    QStringList paths;
    QList<QPair<QString, QString>> renames;
    QStringList createdPaths;
};
struct LiveEntry {
    enum State { Present, Missing, Unavailable } state = Unavailable;
    QString path;
    QString name;
    Node metadata;
    Tree subtree; // populated only for newly discovered directories
};
struct LiveUpdate {
    QList<QPair<QString, QString>> renames;
    std::vector<LiveEntry> entries;
    QStringList retryPaths;
};
struct FileBefore {
    QString name;
    uint64_t size = 0, alloc = 0;
    std::vector<uint32_t> ancestors; // empty for new files
};
struct LiveDelta {
    Removal removal;
    QSet<uint32_t> changed;
    QHash<uint32_t, FileBefore> files;
    QList<QPair<QString, QString>> renames;
    bool structural = false;
};

// No Tree is shared with the I/O worker. knownDirectories is a batch-sized snapshot.
LiveUpdate readLiveUpdate(const FileChanges& changes, const QSet<QString>& knownDirectories,
                          std::atomic_bool& cancel);
LiveDelta applyLiveUpdate(Tree& tree, const LiveUpdate& update, bool allowInsert = true);
}
