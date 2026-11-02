#pragma once

#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <memory>
#include <vector>
#include "liveupdate.h"

// One recursive native subscription per scan root, not one handle per file.
// All callbacks run on the Qt thread; overlapped I/O never blocks that thread.
class DeletionWatcher : public QObject {
    Q_OBJECT
public:
    explicit DeletionWatcher(QObject* parent = nullptr);
    ~DeletionWatcher() override;
    void start(const QStringList& roots);
    void stop();
    int subscriptionCount() const { return int(watches_.size()); }

signals:
    void pathsRemoved(const QStringList& paths);
    void changesReady(const wt::FileChanges& changes);
    void rescanRequired();
    void watchError(const QString& message);

private:
    struct Watch;
    bool addWatch(const QString& path, const QString& selfName = {});
    bool arm(Watch& watch);
    void ready(Watch& watch);
    void enqueue(const QString& path);
    void overflow();
    void flush();
    std::vector<std::unique_ptr<Watch>> watches_;
    QSet<QString> pending_;
    QHash<QString, QString> changed_;
    QList<QPair<QString, QString>> renames_;
    QSet<QString> created_;
    QTimer timer_;
    bool lostChanges_ = false;
};
