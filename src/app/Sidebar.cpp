#include "Sidebar.h"
#include "git/GitPanel.h"
#include <QColor>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeView>
#include <QVBoxLayout>

namespace nebula {
Sidebar::Sidebar(const QString &root,
                 std::function<void(const QString &)> openFile,
                 std::function<void()> openFolder, QWidget *parent)
    : QDockWidget(tr("Explorer"), parent) {
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

  folderName_ = new QLabel;
  folderName_->setTextFormat(Qt::PlainText);
  folderName_->setContentsMargins(12, 0, 6, 0);
  folderName_->setFixedHeight(26);
  auto folderFont = folderName_->font();
  folderFont.setBold(true);
  folderName_->setFont(folderFont);
  folderName_->hide();
  files->addWidget(folderName_);

  tree_ = new QTreeView;
  tree_->setObjectName("workspaceFiles");
  tree_->setAccessibleName(tr("Workspace files"));
  tree_->setFrameShape(QFrame::NoFrame);
  tree_->setIndentation(14);
  tree_->setUniformRowHeights(true);
  tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  tree_->setHeaderHidden(true);
  tree_->hide();
  files->addWidget(tree_, 1);
  connect(tree_, &QTreeView::activated, this,
          [this, openFile](const QModelIndex &index) {
            auto *model = qobject_cast<QFileSystemModel *>(tree_->model());
            if (model && index.isValid() && index.model() == model &&
                !model->isDir(index))
              openFile(model->filePath(index));
          });
  pages_->addWidget(explorer);
  git_ = new GitPanel;
  pages_->addWidget(git_);
  setWidget(body);
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
  folderName_->setVisible(!root.isEmpty());
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
} // namespace nebula
