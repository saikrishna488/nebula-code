#include "app/WorkspaceSearch.h"

#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

namespace nebula {
namespace {

constexpr int kMaxIndexedFiles = 20000;
constexpr int kMaxQuickOpenResults = 200;
constexpr int kMaxSearchMatches = 1000;
constexpr qint64 kMaxSearchFileBytes = 2 * 1024 * 1024;

bool shouldSkipDir(const QString &name) {
  return name.startsWith('.') || name == QStringLiteral("build") ||
         name == QStringLiteral("node_modules") ||
         name == QStringLiteral("__pycache__");
}

QStringList collectWorkspaceFiles(const QString &root) {
  QStringList files;
  if (root.isEmpty())
    return files;
  const QDir rootDir(root);
  QStringList dirs{root};
  while (!dirs.isEmpty() && files.size() < kMaxIndexedFiles) {
    const QDir dir(dirs.takeLast());
    for (const QFileInfo &entry :
         dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                           QDir::Name)) {
      if (entry.isSymLink())
        continue;
      if (entry.isDir()) {
        if (!shouldSkipDir(entry.fileName()))
          dirs.append(entry.filePath());
      } else {
        files.append(rootDir.relativeFilePath(entry.filePath()));
        if (files.size() >= kMaxIndexedFiles)
          break;
      }
    }
  }
  files.sort(Qt::CaseInsensitive);
  return files;
}

bool matchesAllTokens(const QString &candidate, const QStringList &tokens) {
  for (const QString &token : tokens) {
    if (!candidate.contains(token, Qt::CaseInsensitive))
      return false;
  }
  return true;
}

bool isWordChar(QChar ch) { return ch.isLetterOrNumber() || ch == '_'; }

bool isWholeWordMatch(const QString &line, qsizetype index, qsizetype length) {
  const bool leftOk = index == 0 || !isWordChar(line.at(index - 1));
  const bool rightOk = index + length >= line.size() ||
                       !isWordChar(line.at(index + length));
  return leftOk && rightOk;
}

struct SearchMatch {
  QString absolutePath;
  QString relativePath;
  int line = 0;
  int column = 0;
  QString preview;
};

} // namespace

QString quickOpenFile(QWidget *parent, const QString &root) {
  if (root.isEmpty())
    return {};
  const QStringList files = collectWorkspaceFiles(root);
  const QDir rootDir(root);

  QDialog dialog(parent);
  dialog.setObjectName(QStringLiteral("quickOpenDialog"));
  dialog.setWindowTitle(QObject::tr("Quick Open"));
  dialog.resize(540, 380);

  auto *layout = new QVBoxLayout(&dialog);
  auto *query = new QLineEdit(&dialog);
  query->setPlaceholderText(QObject::tr("Open file by name or path"));
  auto *list = new QListWidget(&dialog);
  layout->addWidget(query);
  layout->addWidget(list);

  const auto populate = [&] {
    list->clear();
    const QStringList tokens =
        query->text().split(' ', Qt::SkipEmptyParts);
    int added = 0;
    for (const QString &rel : files) {
      if (!matchesAllTokens(rel, tokens))
        continue;
      auto *item = new QListWidgetItem(rel, list);
      item->setData(Qt::UserRole, rootDir.filePath(rel));
      if (++added >= kMaxQuickOpenResults)
        break;
    }
    if (list->count() > 0)
      list->setCurrentRow(0);
  };

  QObject::connect(query, &QLineEdit::textChanged, &dialog, populate);
  QObject::connect(query, &QLineEdit::returnPressed, &dialog, &QDialog::accept);
  QObject::connect(list, &QListWidget::itemActivated, &dialog,
                   &QDialog::accept);
  populate();

  if (dialog.exec() == QDialog::Accepted && list->currentItem())
    return list->currentItem()->data(Qt::UserRole).toString();
  return {};
}

FindInFilesPanel::FindInFilesPanel(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);

  auto *controls = new QHBoxLayout;
  query_ = new QLineEdit(this);
  query_->setObjectName(QStringLiteral("findInFilesQuery"));
  query_->setPlaceholderText(tr("Search in workspace files"));
  caseSensitive_ = new QCheckBox(tr("Match Case"), this);
  wholeWord_ = new QCheckBox(tr("Whole Word"), this);
  searchButton_ = new QPushButton(tr("Search"), this);
  status_ = new QLabel(this);

  controls->addWidget(query_, 1);
  controls->addWidget(caseSensitive_);
  controls->addWidget(wholeWord_);
  controls->addWidget(searchButton_);
  controls->addWidget(status_);
  layout->addLayout(controls);

  results_ = new QListWidget(this);
  results_->setObjectName(QStringLiteral("findInFilesResults"));
  layout->addWidget(results_, 1);

  connect(query_, &QLineEdit::returnPressed, this,
          &FindInFilesPanel::startSearch);
  connect(searchButton_, &QPushButton::clicked, this,
          &FindInFilesPanel::startSearch);
  connect(results_, &QListWidget::itemActivated, this,
          [this](QListWidgetItem *item) {
            if (!item)
              return;
            emit matchActivated(item->data(Qt::UserRole).toString(),
                                item->data(Qt::UserRole + 1).toInt(),
                                item->data(Qt::UserRole + 2).toInt());
          });
}

FindInFilesPanel::~FindInFilesPanel() { cancelSearch(); }

void FindInFilesPanel::setRoot(const QString &root) {
  cancelSearch();
  root_ = root;
  results_->clear();
  status_->clear();
}

void FindInFilesPanel::focusQuery() {
  query_->setFocus();
  query_->selectAll();
}

void FindInFilesPanel::cancelSearch() {
  if (cancelled_)
    cancelled_->store(true);
  if (worker_) {
    worker_->wait();
    delete worker_;
    worker_ = nullptr;
  }
}

void FindInFilesPanel::startSearch() {
  cancelSearch();
  results_->clear();

  const QString needle = query_->text();
  if (root_.isEmpty() || needle.isEmpty()) {
    status_->clear();
    return;
  }

  const bool matchCase = caseSensitive_->isChecked();
  const bool wholeWord = wholeWord_->isChecked();
  const QString root = root_;
  status_->setText(tr("Searching…"));

  auto cancelled = std::make_shared<std::atomic_bool>(false);
  cancelled_ = cancelled;

  worker_ = QThread::create([this, root, needle, matchCase, wholeWord,
                             cancelled] {
    const QStringList files = collectWorkspaceFiles(root);
    const QDir rootDir(root);
    QList<SearchMatch> matches;
    int matchedFiles = 0;
    const Qt::CaseSensitivity cs =
        matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;

    for (const QString &rel : files) {
      if (cancelled->load() || matches.size() >= kMaxSearchMatches)
        break;
      const QString absPath = rootDir.filePath(rel);
      QFile file(absPath);
      if (file.size() > kMaxSearchFileBytes ||
          !file.open(QIODevice::ReadOnly))
        continue;
      const QByteArray sample = file.peek(1024);
      if (sample.contains('\0'))
        continue;
      const QString content = QString::fromUtf8(file.readAll());
      const QStringList lines = content.split('\n');
      bool fileMatched = false;
      for (int lineIdx = 0;
           lineIdx < lines.size() && matches.size() < kMaxSearchMatches;
           ++lineIdx) {
        const QString &line = lines.at(lineIdx);
        qsizetype pos = 0;
        while ((pos = line.indexOf(needle, pos, cs)) >= 0) {
          if (!wholeWord || isWholeWordMatch(line, pos, needle.size())) {
            const int column =
                static_cast<int>(line.left(pos).toUtf8().size());
            matches.append({absPath, rel, lineIdx, column, line.trimmed()});
            fileMatched = true;
            break;
          }
          pos += needle.size();
        }
      }
      if (fileMatched)
        ++matchedFiles;
    }

    QMetaObject::invokeMethod(
        this,
        [this, matches = std::move(matches), matchedFiles, cancelled] {
          if (cancelled->load())
            return;
          for (const SearchMatch &m : matches) {
            const QString label = QStringLiteral("%1:%2: %3")
                                      .arg(m.relativePath)
                                      .arg(m.line + 1)
                                      .arg(m.preview);
            auto *item = new QListWidgetItem(label, results_);
            item->setData(Qt::UserRole, m.absolutePath);
            item->setData(Qt::UserRole + 1, m.line);
            item->setData(Qt::UserRole + 2, m.column);
          }
          status_->setText(
              matches.size() >= kMaxSearchMatches
                  ? tr("%1+ matches in %2 files")
                        .arg(matches.size())
                        .arg(matchedFiles)
                  : tr("%1 matches in %2 files")
                        .arg(matches.size())
                        .arg(matchedFiles));
        },
        Qt::QueuedConnection);
  });
  worker_->start();
}

} // namespace nebula
