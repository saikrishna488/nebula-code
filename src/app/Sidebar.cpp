#include "Sidebar.h"
#include "git/GitPanel.h"
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace nebula {
namespace {

QIcon makeHeaderIcon(bool folder, const QColor &color) {
  const qreal dpr = 2.0;
  QPixmap pixmap(QSize(16, 16) * dpr);
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);

  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(
      QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

  if (folder) {
    painter.drawPolyline(QPolygonF{{2, 4}, {6.5, 4}, {8, 5.5}, {14, 5.5},
                                   {14, 12.5}, {2, 12.5}, {2, 4}});
  } else {
    painter.drawPolyline(
        QPolygonF{{3, 2}, {9.5, 2}, {13, 5.5}, {13, 14}, {3, 14}, {3, 2}});
    painter.drawPolyline(QPolygonF{{9.5, 2}, {9.5, 5.5}, {13, 5.5}});
  }

  // Draw a small '+' badge in the bottom-right / center.
  painter.drawLine(QPointF(6.5, 9.5), QPointF(10.5, 9.5));
  painter.drawLine(QPointF(8.5, 7.5), QPointF(8.5, 11.5));
  return QIcon(pixmap);
}

bool isValidEntryName(const QString &name) {
  return !name.isEmpty() && name != "." && name != ".." &&
         !name.contains('/') && !name.contains('\\');
}

void revealInFileManager(const QString &path, bool isDir) {
#ifdef Q_OS_MACOS
  Q_UNUSED(isDir);
  QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#else
  const QString target = isDir ? path : QFileInfo(path).absolutePath();
  QDesktopServices::openUrl(QUrl::fromLocalFile(target));
#endif
}

} // namespace

Sidebar::Sidebar(const QString &root,
                 std::function<void(const QString &)> openFile,
                 std::function<void()> openFolder, QWidget *parent)
    : QDockWidget(tr("Explorer"), parent), openFile_(std::move(openFile)) {
  setObjectName("WorkspaceSidebar");
  setAllowedAreas(Qt::LeftDockWidgetArea);
  setFeatures(QDockWidget::NoDockWidgetFeatures);
  auto *emptyTitle = new QWidget;
  emptyTitle->setFixedHeight(0);
  setTitleBarWidget(emptyTitle);
  auto *body = new QWidget;
  body->setObjectName("sidebarBody");
  body->setAutoFillBackground(true);
  auto *layout = new QVBoxLayout(body);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  heading_ = new QLabel(tr("EXPLORER"));
  heading_->setMargin(0);
  heading_->setContentsMargins(16, 0, 8, 0);
  heading_->setFixedHeight(36);
  auto headerFont = heading_->font();
  headerFont.setPointSize(10);
  headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.7);
  heading_->setFont(headerFont);
  layout->addWidget(heading_);
  pages_ = new QStackedWidget;
  layout->addWidget(pages_, 1);
  auto *explorer = new QWidget;
  auto *files = new QVBoxLayout(explorer);
  files->setContentsMargins(0, 0, 0, 0);
  files->setSpacing(0);
  emptyExplorer_ = new QWidget;
  auto *emptyFiles = new QVBoxLayout(emptyExplorer_);
  emptyFiles->setContentsMargins(14, 6, 14, 0);
  auto *open = new QPushButton(tr("Open Folder…"));
  open->setObjectName("sidebarOpenFolder");
  emptyFiles->addWidget(open);
  emptyFiles->addStretch();
  files->addWidget(emptyExplorer_, 1);
  connect(open, &QPushButton::clicked, this, openFolder);

  folderHeader_ = new QWidget;
  folderHeader_->setObjectName("sidebarFolderHeader");
  folderHeader_->setFixedHeight(26);
  auto *folderBar = new QHBoxLayout(folderHeader_);
  folderBar->setContentsMargins(12, 0, 6, 0);
  folderBar->setSpacing(2);

  folderName_ = new QLabel;
  folderName_->setTextFormat(Qt::PlainText);
  auto folderFont = folderName_->font();
  folderFont.setBold(true);
  folderName_->setFont(folderFont);
  folderBar->addWidget(folderName_, 1);

  newFileButton_ = new QToolButton;
  newFileButton_->setObjectName("sidebarNewFile");
  newFileButton_->setAutoRaise(true);
  newFileButton_->setFixedSize(22, 22);
  newFileButton_->setIconSize(QSize(16, 16));
  newFileButton_->setToolTip(tr("New File…"));
  newFileButton_->setAccessibleName(tr("New File"));
  folderBar->addWidget(newFileButton_);

  newFolderButton_ = new QToolButton;
  newFolderButton_->setObjectName("sidebarNewFolder");
  newFolderButton_->setAutoRaise(true);
  newFolderButton_->setFixedSize(22, 22);
  newFolderButton_->setIconSize(QSize(16, 16));
  newFolderButton_->setToolTip(tr("New Folder…"));
  newFolderButton_->setAccessibleName(tr("New Folder"));
  folderBar->addWidget(newFolderButton_);

  folderHeader_->hide();
  files->addWidget(folderHeader_);

  connect(newFileButton_, &QToolButton::clicked, this,
          [this] { createEntry(selectedDirectory(), false); });
  connect(newFolderButton_, &QToolButton::clicked, this,
          [this] { createEntry(selectedDirectory(), true); });

  tree_ = new QTreeView;
  tree_->setObjectName("workspaceFiles");
  tree_->setAccessibleName(tr("Workspace files"));
  tree_->setFrameShape(QFrame::NoFrame);
  tree_->setIndentation(14);
  tree_->setUniformRowHeights(true);
  tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  tree_->setHeaderHidden(true);
  tree_->setContextMenuPolicy(Qt::CustomContextMenu);
  tree_->hide();
  files->addWidget(tree_, 1);
  connect(tree_, &QTreeView::activated, this,
          [this](const QModelIndex &index) {
            auto *model = qobject_cast<QFileSystemModel *>(tree_->model());
            if (model && index.isValid() && index.model() == model &&
                !model->isDir(index))
              openFile_(model->filePath(index));
          });
  connect(tree_, &QTreeView::customContextMenuRequested, this,
          &Sidebar::showContextMenu);
  pages_->addWidget(explorer);
  git_ = new GitPanel;
  pages_->addWidget(git_);
  setWidget(body);
  updateActionIcons(palette().windowText().color());
  setWorkspace(root);
}

void Sidebar::setWorkspace(const QString &root) {
  if (root_ == root)
    return;

  root_ = root;
  auto *previousModel = tree_->model();
  auto *previousSelection = tree_->selectionModel();
  if (root.isEmpty()) {
    tree_->setModel(nullptr);
  } else {
    auto *model = new QFileSystemModel(tree_);
    tree_->setModel(model);
    for (int col = 1; col < model->columnCount(); ++col)
      tree_->hideColumn(col);
    tree_->setRootIndex(model->setRootPath(root));
  }
  if (previousSelection != tree_->selectionModel())
    delete previousSelection;
  delete previousModel;

  const QString name = QFileInfo(root).fileName();
  folderName_->setText(name.isEmpty() ? root : name);
  folderName_->setToolTip(root);
  emptyExplorer_->setVisible(root.isEmpty());
  folderHeader_->setVisible(!root.isEmpty());
  tree_->setVisible(!root.isEmpty());
  pages_->setCurrentIndex(0);
  heading_->setText(tr("EXPLORER"));
  // Source control resolves the new repository when its page is opened.
  git_->setWorkspace({});
}

bool Sidebar::showsSourceControl() const {
  return pages_->currentWidget() == git_;
}

void Sidebar::togglePage(bool sourceControl) {
  if (isVisible() && sourceControl == showsSourceControl()) {
    hide();
    return;
  }
  pages_->setCurrentIndex(sourceControl ? 1 : 0);
  heading_->setText(sourceControl ? tr("SOURCE CONTROL") : tr("EXPLORER"));
  if (sourceControl)
    git_->setWorkspace(root_);
  show();
}

void Sidebar::setColors(const QColor &panel, const QColor &foreground,
                        const QColor &accent) {
  auto p = parentWidget()->palette();
  for (auto role : {QPalette::Window, QPalette::Base, QPalette::AlternateBase,
                    QPalette::Button})
    p.setColor(role, panel);
  for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
    p.setColor(role, foreground);
  setPalette(p);
  widget()->setPalette(p);
  updateActionIcons(foreground);
  const QString buttonStyle =
      QStringLiteral("QToolButton { border: 0; border-radius: 3px; }"
                     "QToolButton:hover { background: rgba(%1, %2, %3, 24); }"
                     "QToolButton:pressed { background: rgba(%1, %2, %3, 42); }")
          .arg(foreground.red())
          .arg(foreground.green())
          .arg(foreground.blue());
  newFileButton_->setStyleSheet(buttonStyle);
  newFolderButton_->setStyleSheet(buttonStyle);
  if (auto *tree = findChild<QTreeView *>("workspaceFiles")) {
    tree->setStyleSheet(QStringLiteral(
      "QTreeView { background-color: %1; color: %2; }"
      "QTreeView::item { height: 22px; }"
      "QTreeView::item:selected { background: %3; color: %4; }")
      .arg(panel.name(), foreground.name(), p.color(QPalette::Highlight).name(),
           p.color(QPalette::HighlightedText).name()));
  }
  if (auto *open = findChild<QPushButton *>("sidebarOpenFolder"))
    open->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: white; border: 0; "
                       "padding: 6px; border-radius: 3px; } QPushButton:focus "
                       "{ border: 1px solid palette(text); }")
            .arg(accent.name()));
}

void Sidebar::updateActionIcons(const QColor &foreground) {
  newFileButton_->setIcon(makeHeaderIcon(false, foreground));
  newFolderButton_->setIcon(makeHeaderIcon(true, foreground));
}

QString Sidebar::selectedDirectory() const {
  auto *model = qobject_cast<QFileSystemModel *>(tree_->model());
  if (!model)
    return root_;
  const QModelIndex index = tree_->currentIndex();
  if (!index.isValid() || index.model() != model)
    return root_;
  const QString path = model->filePath(index);
  return model->isDir(index) ? path : QFileInfo(path).absolutePath();
}

void Sidebar::createEntry(const QString &parentDir, bool directory) {
  if (root_.isEmpty() || parentDir.isEmpty())
    return;
  bool ok = false;
  const QString title = directory ? tr("New Folder") : tr("New File");
  const QString label = directory ? tr("Folder name:") : tr("File name:");
  const QString name =
      QInputDialog::getText(this, title, label, QLineEdit::Normal, {}, &ok)
          .trimmed();
  if (!ok)
    return;
  if (!isValidEntryName(name)) {
    QMessageBox::warning(this, title,
                         tr("Enter a valid name without path separators."));
    return;
  }
  const QString targetPath = QDir(parentDir).filePath(name);
  if (QFileInfo::exists(targetPath)) {
    QMessageBox::warning(this, title,
                         tr("\"%1\" already exists.").arg(name));
    return;
  }
  if (directory) {
    if (!QDir(parentDir).mkdir(name)) {
      QMessageBox::warning(this, title,
                           tr("Could not create folder \"%1\".").arg(name));
      return;
    }
  } else {
    QFile file(targetPath);
    if (!file.open(QIODevice::WriteOnly)) {
      QMessageBox::warning(
          this, title,
          tr("Could not create file \"%1\": %2").arg(name, file.errorString()));
      return;
    }
  }
  if (auto *model = qobject_cast<QFileSystemModel *>(tree_->model())) {
    const QModelIndex parentIndex = model->index(parentDir);
    if (parentIndex.isValid())
      tree_->expand(parentIndex);
    const QModelIndex index = model->index(targetPath);
    if (index.isValid()) {
      tree_->setCurrentIndex(index);
      tree_->scrollTo(index);
    }
  }
  if (!directory)
    openFile_(targetPath);
}

void Sidebar::renameEntry(const QString &path) {
  if (path.isEmpty() || path == root_)
    return;
  const QFileInfo info(path);
  bool ok = false;
  const QString newName =
      QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                            QLineEdit::Normal, info.fileName(), &ok)
          .trimmed();
  if (!ok || newName == info.fileName())
    return;
  if (!isValidEntryName(newName)) {
    QMessageBox::warning(this, tr("Rename"),
                         tr("Enter a valid name without path separators."));
    return;
  }
  const QString newPath = QDir(info.absolutePath()).filePath(newName);
  if (QFileInfo::exists(newPath)) {
    QMessageBox::warning(this, tr("Rename"),
                         tr("\"%1\" already exists.").arg(newName));
    return;
  }
  if (!QDir().rename(path, newPath)) {
    QMessageBox::warning(this, tr("Rename"),
                         tr("Could not rename \"%1\".").arg(info.fileName()));
    return;
  }
  if (auto *model = qobject_cast<QFileSystemModel *>(tree_->model())) {
    const QModelIndex index = model->index(newPath);
    if (index.isValid()) {
      tree_->setCurrentIndex(index);
      tree_->scrollTo(index);
    }
  }
  emit pathRenamed(path, newPath);
}

void Sidebar::deleteEntry(const QString &path) {
  if (path.isEmpty() || path == root_)
    return;
  const QFileInfo info(path);
  if (QMessageBox::question(
          this, tr("Delete"),
          tr("Move \"%1\" to Trash?").arg(info.fileName()),
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
      QMessageBox::Yes) {
    return;
  }
  if (!QFile::moveToTrash(path)) {
    QMessageBox::warning(
        this, tr("Delete"),
        tr("Could not move \"%1\" to Trash.").arg(info.fileName()));
    return;
  }
  emit pathDeleted(path);
}

void Sidebar::showContextMenu(const QPoint &pos) {
  auto *model = qobject_cast<QFileSystemModel *>(tree_->model());
  if (!model || root_.isEmpty())
    return;
  const QModelIndex index = tree_->indexAt(pos);
  const bool isItem = index.isValid() && index.model() == model;
  const QString path = isItem ? model->filePath(index) : root_;
  const bool isDir = !isItem || model->isDir(index);
  const QString targetDir = isDir ? path : QFileInfo(path).absolutePath();
  if (isItem)
    tree_->setCurrentIndex(index);

  QMenu menu(tree_);
  if (isItem && !isDir) {
    menu.addAction(tr("Open"), this, [this, path] { openFile_(path); });
    menu.addSeparator();
  }
  menu.addAction(tr("New File…"), this,
                 [this, targetDir] { createEntry(targetDir, false); });
  menu.addAction(tr("New Folder…"), this,
                 [this, targetDir] { createEntry(targetDir, true); });
  if (isItem) {
    menu.addSeparator();
    menu.addAction(tr("Rename…"), this, [this, path] { renameEntry(path); });
    menu.addAction(tr("Delete"), this, [this, path] { deleteEntry(path); });
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this,
                   [path] { QApplication::clipboard()->setText(path); });
#ifdef Q_OS_MACOS
    const QString revealLabel = tr("Reveal in Finder");
#else
    const QString revealLabel = tr("Open Containing Folder");
#endif
    menu.addAction(revealLabel, this,
                   [path, isDir] { revealInFileManager(path, isDir); });
  }
  menu.exec(tree_->viewport()->mapToGlobal(pos));
}
} // namespace nebula
