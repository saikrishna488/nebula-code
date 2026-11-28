#include "GitPanel.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace nebula {

GitPanel::GitPanel(QWidget *parent)
    : QWidget(parent), repository_(new GitRepository(this)),
      status_(new QLabel(tr("Open a folder to use Git."), this)), error_(new QLabel(this)),
      files_(new QTreeWidget(this)), diff_(new QPlainTextEdit(this)), message_(new QLineEdit(this)),
      refresh_(new QPushButton(tr("Refresh"), this)), stage_(new QPushButton(tr("Stage"), this)),
      unstage_(new QPushButton(tr("Unstage"), this)),
      stagedDiff_(new QPushButton(tr("Staged Diff"), this)),
      unstagedDiff_(new QPushButton(tr("Unstaged Diff"), this)),
      commit_(new QPushButton(tr("Commit"), this)),
      statusRefreshTimer_(new QTimer(this))
{
    statusRefreshTimer_->setInterval(1500);
    connect(statusRefreshTimer_, &QTimer::timeout, this, [this] {
        if (!repository_->root().isEmpty() && !repository_->busy())
            repository_->refresh();
    });
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    error_->setTextFormat(Qt::PlainText);
    error_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    error_->setWordWrap(true);
    error_->hide();
    files_->setHeaderLabels({tr("Index"), tr("Work Tree"), tr("Path")});
    files_->setRootIsDecorated(false);
    files_->setUniformRowHeights(true);
    files_->resizeColumnToContents(0);
    files_->resizeColumnToContents(1);
    diff_->setReadOnly(true);
    diff_->setLineWrapMode(QPlainTextEdit::NoWrap);
    diff_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    diff_->setPlaceholderText(tr("Select a file and choose a diff."));
    message_->setPlaceholderText(tr("Commit message"));
    message_->setAccessibleName(tr("Commit message"));

    auto *tools = new QGridLayout;
    tools->addWidget(stage_, 0, 0);
    tools->addWidget(unstage_, 0, 1);
    tools->addWidget(stagedDiff_, 1, 0);
    tools->addWidget(unstagedDiff_, 1, 1);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(files_);
    splitter->addWidget(diff_);
    splitter->setStretchFactor(1, 1);

    auto *commitRow = new QHBoxLayout;
    commitRow->addWidget(message_, 1);
    commitRow->addWidget(commit_);

    auto *layout = new QVBoxLayout(this);
    auto *summary = new QHBoxLayout;
    summary->addWidget(status_, 1);
    summary->addWidget(refresh_);
    layout->addLayout(summary);
    layout->addWidget(error_);
    layout->addLayout(tools);
    layout->addWidget(splitter, 1);
    layout->addLayout(commitRow);

    connect(repository_, &GitRepository::messageChanged, status_, &QLabel::setText);
    connect(repository_, &GitRepository::errorOccurred, this, [this](const QString &error) {
        error_->setText(error);
        error_->show();
    });
    connect(repository_, &GitRepository::statusReady, this, &GitPanel::updateStatus);
    connect(repository_, &GitRepository::busyChanged, this, &GitPanel::updateControls);
    connect(repository_, &GitRepository::commitSucceeded, message_, &QLineEdit::clear);
    connect(repository_, &GitRepository::diffReady, this, [this](const QString &title, const QString &diff) {
        status_->setText(title);
        diff_->setPlainText(diff.isEmpty() ? tr("No changes in this diff.") : diff);
    });
    connect(files_, &QTreeWidget::itemSelectionChanged, this, &GitPanel::updateControls);
    connect(message_, &QLineEdit::textChanged, this, &GitPanel::updateControls);
    connect(message_, &QLineEdit::returnPressed, commit_, &QPushButton::click);
    connect(refresh_, &QPushButton::clicked, this, [this] {
        error_->hide();
        repository_->refresh();
    });
    connect(stage_, &QPushButton::clicked, this, [this] {
        if (const auto *entry = selectedEntry()) {
            error_->hide();
            repository_->stage(*entry);
        }
    });
    connect(unstage_, &QPushButton::clicked, this, [this] {
        if (const auto *entry = selectedEntry()) {
            error_->hide();
            repository_->unstage(*entry);
        }
    });
    connect(stagedDiff_, &QPushButton::clicked, this, [this] {
        if (const auto *entry = selectedEntry()) {
            error_->hide();
            repository_->showDiff(*entry, true);
        }
    });
    connect(unstagedDiff_, &QPushButton::clicked, this, [this] {
        if (const auto *entry = selectedEntry()) {
            error_->hide();
            repository_->showDiff(*entry, false);
        }
    });
    connect(files_, &QTreeWidget::itemActivated, this, [this] {
        if (unstagedDiff_->isEnabled())
            unstagedDiff_->click();
        else if (stagedDiff_->isEnabled())
            stagedDiff_->click();
    });
    connect(commit_, &QPushButton::clicked, this, [this] {
        error_->hide();
        repository_->commit(message_->text());
    });
    updateControls();
}

void GitPanel::setWorkspace(const QString &root)
{
    if (root.isEmpty())
        statusRefreshTimer_->stop();
    else
        statusRefreshTimer_->start();
    if (root == repository_->workspace()) {
        if (!root.isEmpty() && !repository_->busy()) {
            error_->hide();
            repository_->refresh();
        }
        return;
    }
    diff_->clear();
    message_->clear();
    error_->hide();
    repository_->setWorkspace(root);
}

const GitEntry *GitPanel::selectedEntry() const
{
    const auto *current = files_->currentItem();
    if (!current || !current->isSelected())
        return nullptr;
    const auto index = current->data(0, Qt::UserRole).toInt();
    return index >= 0 && index < entries_.size() ? &entries_.at(index) : nullptr;
}

void GitPanel::updateStatus(const QList<GitEntry> &entries)
{
    const auto *current = selectedEntry();
    const QString selected = current ? current->path : QString{};
    files_->clear();
    diff_->clear();
    entries_ = entries;
    for (qsizetype index = 0; index < entries_.size(); ++index) {
        const auto &entry = entries_.at(index);
        const QString path = entry.originalPath.isEmpty() ? entry.path
            : tr("%1 → %2").arg(entry.originalPath, entry.path);
        auto *item = new QTreeWidgetItem(files_, {QString(entry.index), QString(entry.workTree), path});
        item->setData(0, Qt::UserRole, index);
        item->setToolTip(2, path);
        if (entry.path == selected)
            files_->setCurrentItem(item);
    }
    if (!repository_->root().isEmpty()) {
        status_->setText(entries_.isEmpty() ? tr("Working tree clean.")
            : tr("%n changed file(s)", nullptr, entries_.size()));
    }
    updateControls();
}

void GitPanel::updateControls()
{
    const bool available = !repository_->busy();
    const auto *entry = selectedEntry();
    refresh_->setEnabled(available);
    stage_->setEnabled(available && entry && entry->unstaged());
    unstage_->setEnabled(available && entry && entry->staged());
    stagedDiff_->setEnabled(unstage_->isEnabled());
    unstagedDiff_->setEnabled(stage_->isEnabled());
    const bool hasStaged = std::any_of(entries_.cbegin(), entries_.cend(),
                                       [](const GitEntry &item) { return item.staged(); });
    commit_->setEnabled(available && hasStaged && !message_->text().trimmed().isEmpty());
}

}
