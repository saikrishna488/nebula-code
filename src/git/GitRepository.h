#pragma once

#include <QList>
#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>

class QProcess;

namespace nebula {

struct GitEntry {
    QString path;
    QString originalPath;
    QChar index = u' ';
    QChar workTree = u' ';

    bool operator==(const GitEntry &) const = default;

    bool untracked() const { return index == u'?' && workTree == u'?'; }
    bool staged() const { return !untracked() && index != u' '; }
    bool unstaged() const { return workTree != u' '; }
};

class GitRepository : public QObject {
    Q_OBJECT

public:
    explicit GitRepository(QObject *parent = nullptr);
    ~GitRepository() override;

    void setWorkspace(const QString &folder);
    QString root() const { return root_; }
    QString workspace() const { return workspace_; }
    bool busy() const { return running_ || !queue_.isEmpty(); }
    void refresh();
    void showDiff(const GitEntry &entry, bool staged);
    void stage(const GitEntry &entry);
    void unstage(const GitEntry &entry);
    void commit(const QString &message);

signals:
    void statusReady(const QList<GitEntry> &entries);
    void diffReady(const QString &title, const QString &diff);
    void messageChanged(const QString &message);
    void errorOccurred(const QString &message);
    void busyChanged();
    void commitSucceeded();

private:
    struct Result {
        int code;
        QByteArray output;
        QByteArray error;
    };

    struct Job {
        QStringList arguments;
        QString folder;
        QString label;
        int generation;
        bool write;
        std::function<void(const Result &)> completed;
    };

    void enqueue(const QString &label, const QString &folder, const QStringList &arguments,
                 std::function<void(const Result &)> completed, bool write = false,
                 bool priority = false);
    void pump();
    void finish(Result result);
    void report(const QString &label, const Result &result);
    void queueStatus(bool priority);
    void write(const QString &label, const QStringList &arguments);

    QString workspace_;
    QString root_;
    QString executable_;
    QString lockedRoot_;
    QProcess *process_;
    QList<Job> queue_;
    std::unique_ptr<Job> running_;
    int generation_ = 0;
    bool waitingForWrite_ = false;
};

}
