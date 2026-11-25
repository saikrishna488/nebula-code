#include "GitRepository.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>

#include <utility>

namespace nebula {

namespace {

QSet<QString> writeLocks;

bool renameCode(char code)
{
    return code == 'R' || code == 'C';
}

QList<GitEntry> parseStatus(const QByteArray &output)
{
    const auto records = output.split('\0');
    QList<GitEntry> entries;
    for (qsizetype i = 0; i < records.size(); ++i) {
        const auto &record = records.at(i);
        if (record.size() < 4 || record.at(2) != ' ')
            continue;
        GitEntry entry{QFile::decodeName(record.mid(3)), {},
                       QLatin1Char(record.at(0)), QLatin1Char(record.at(1))};
        if ((renameCode(record.at(0)) || renameCode(record.at(1))) && i + 1 < records.size())
            entry.originalPath = QFile::decodeName(records.at(++i));
        entries.append(entry);
    }
    return entries;
}

QStringList entryPaths(const GitEntry &entry, bool includeOriginal)
{
    QStringList paths{entry.path};
    if (includeOriginal && !entry.originalPath.isEmpty())
        paths.append(entry.originalPath);
    return paths;
}

}

GitRepository::GitRepository(QObject *parent)
    : QObject(parent), process_(new QProcess(this))
{
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const QString &variable : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR",
                                    "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_PREFIX"})
        environment.remove(variable);
    environment.insert("GIT_OPTIONAL_LOCKS", "0");
    environment.insert("GIT_TERMINAL_PROMPT", "0");
    environment.insert("GCM_INTERACTIVE", "never");
    process_->setProcessEnvironment(environment);
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        finish({status == QProcess::NormalExit ? code : -1, process_->readAllStandardOutput(),
                process_->readAllStandardError()});
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish({-1, {}, process_->errorString().toUtf8()});
    });
}

GitRepository::~GitRepository()
{
    disconnect(process_, nullptr, this, nullptr);
    if (process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(500);
    }
    writeLocks.remove(lockedRoot_);
}

void GitRepository::setWorkspace(const QString &folder)
{
    if (workspace_ == folder)
        return;
    workspace_ = folder;
    root_.clear();
    ++generation_;
    queue_.clear();
    emit statusReady({});
    emit busyChanged();
    refresh();
}

void GitRepository::refresh()
{
    if (workspace_.isEmpty()) {
        emit messageChanged(tr("Open a folder to use Git."));
        return;
    }
    if (!root_.isEmpty()) {
        queueStatus(false);
        return;
    }
    executable_ = QStandardPaths::findExecutable("git");
    if (executable_.isEmpty()) {
        emit messageChanged(tr("Git was not found on PATH."));
        return;
    }
    const auto label = tr("Find Git repository");
    enqueue(label, workspace_, {"rev-parse", "--show-toplevel"}, [this, label](const Result &result) {
        if (result.code != 0) {
            report(label, result);
            return;
        }
        QByteArray path = result.output;
        if (path.endsWith('\n'))
            path.chop(1);
        const QDir directory(QFile::decodeName(path));
        root_ = directory.canonicalPath();
        if (root_.isEmpty())
            root_ = directory.absolutePath();
        queueStatus(true);
    });
}

void GitRepository::showDiff(const GitEntry &entry, bool staged)
{
    if (root_.isEmpty() || entry.path.isEmpty())
        return;
    QStringList arguments{"diff", "--no-ext-diff", "--no-textconv", "--no-color"};
    if (staged)
        arguments.append("--cached");
    const bool untracked = !staged && entry.untracked();
    if (untracked)
        arguments.append("--no-index");
    arguments.append("--");
    if (untracked)
        arguments.append("/dev/null");
    const auto status = staged ? entry.index : entry.workTree;
    arguments.append(entryPaths(entry, renameCode(status.toLatin1())));

    const auto label = staged ? tr("Staged diff") : tr("Unstaged diff");
    enqueue(label, root_, arguments, [this, label, path = entry.path, untracked](const Result &result) {
        if (result.code == 0 || (untracked && result.code == 1)) {
            emit diffReady(tr("%1 — %2").arg(label, path), QString::fromUtf8(result.output));
        } else {
            report(label, result);
        }
    });
}

void GitRepository::stage(const GitEntry &entry)
{
    if (!entry.path.isEmpty())
        write(tr("Stage"), QStringList{"add", "-A", "--"} + entryPaths(entry, entry.workTree == u'R'));
}

void GitRepository::unstage(const GitEntry &entry)
{
    if (!entry.path.isEmpty())
        write(tr("Unstage"), QStringList{"reset", "-q", "--"} + entryPaths(entry, entry.index == u'R'));
}

void GitRepository::commit(const QString &message)
{
    if (root_.isEmpty() || message.trimmed().isEmpty())
        return;
    const auto label = tr("Commit");
    enqueue(label, root_, {"commit", "-m", message}, [this, label](const Result &result) {
        report(label, result);
        if (result.code == 0)
            emit commitSucceeded();
        queueStatus(true);
    }, true);
}

void GitRepository::enqueue(const QString &label, const QString &folder, const QStringList &arguments,
                            std::function<void(const Result &)> completed, bool write, bool priority)
{
    Job job{arguments, folder, label, generation_, write, std::move(completed)};
    if (priority)
        queue_.prepend(std::move(job));
    else
        queue_.append(std::move(job));
    emit busyChanged();
    pump();
}

void GitRepository::pump()
{
    if (running_ || queue_.isEmpty() || waitingForWrite_)
        return;
    const auto &next = queue_.first();
    if (next.write && writeLocks.contains(next.folder)) {
        waitingForWrite_ = true;
        QTimer::singleShot(25, this, [this] {
            waitingForWrite_ = false;
            pump();
        });
        return;
    }
    running_ = std::make_unique<Job>(queue_.takeFirst());
    if (running_->write) {
        lockedRoot_ = running_->folder;
        writeLocks.insert(lockedRoot_);
    }
    process_->setWorkingDirectory(running_->folder);
    emit messageChanged(tr("%1…").arg(running_->label));
    process_->start(executable_, QStringList{"--no-pager", "--literal-pathspecs"} + running_->arguments);
}

void GitRepository::finish(Result result)
{
    if (!running_)
        return;
    if (!lockedRoot_.isEmpty())
        writeLocks.remove(std::exchange(lockedRoot_, {}));
    if (running_->generation == generation_)
        running_->completed(result);
    running_.reset();
    emit busyChanged();
    pump();
}

void GitRepository::report(const QString &label, const Result &result)
{
    if (result.code != 0) {
        const auto detail = QString::fromUtf8(result.error.trimmed());
        emit errorOccurred(detail.isEmpty() ? tr("%1 failed (exit code %2).").arg(label).arg(result.code)
                                             : tr("%1 failed. %2").arg(label, detail));
    }
}

void GitRepository::queueStatus(bool priority)
{
    const auto label = tr("Git status");
    enqueue(label, root_, {"status", "--porcelain=v1", "-z", "--untracked-files=all", "--renames"},
            [this, label](const Result &result) {
        if (result.code != 0) {
            report(label, result);
            return;
        }
        emit statusReady(parseStatus(result.output));
    }, false, priority);
}

void GitRepository::write(const QString &label, const QStringList &arguments)
{
    if (root_.isEmpty())
        return;
    enqueue(label, root_, arguments, [this, label](const Result &result) {
        report(label, result);
        queueStatus(true);
    }, true);
}

}
