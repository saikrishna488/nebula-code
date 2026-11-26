#include "Workbench.h"
#include "ActivityBar.h"
#include "AppearanceDialog.h"
#include "Sidebar.h"
#include "Theme.h"
#include "WorkspaceSearch.h"
#include "editor/EditorView.h"
#include "lsp/LspClient.h"
#include "lsp/LspPanel.h"
#include "terminal/TerminalDock.h"
#include "terminal/TerminalSession.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>

namespace nebula {
namespace {
EditorTheme colors(const QJsonObject &obj) {
  const auto color = [&](const char *key) {
    return QColor(obj[key].toString());
  };
  return {color("background"),
          color("foreground"),
          color("muted"),
          color("selection"),
          color("currentLine"),
          color("keyword"),
          color("string"),
          color("comment"),
          color("number"),
          color("preprocessor"),
          QFont(obj["font"].toString(), obj["pointSize"].toInt())};
}
QString titleFor(const DocumentTab *tab) {
  return tab->file.path.isEmpty() ? QObject::tr("Untitled")
                                  : QFileInfo(tab->file.path).fileName();
}
} // namespace

Workbench::Workbench(const QString &root) : Workbench(root, true) {}

Workbench::Workbench(const QString &root, bool restoreWorkspace)
    : root_(root.isEmpty() ? QString() : normalizedPath(root)),
      settings_(readJson(storagePath("settings.json"))),
      commands_(
          this,
          readJson(storagePath("keybindings.json"))["bindings"].toObject()),
      rememberWorkspace_(restoreWorkspace) {
  setAttribute(Qt::WA_DeleteOnClose);
  resize(1040, 700);
  theme_ = settings_["theme"].toString("light");
  minimap_ = settings_["minimap"].toBool(true);
  tabs_ = new QTabWidget;
  tabs_->setDocumentMode(true);
  tabs_->setMovable(true);
  tabs_->setTabsClosable(true);
  auto *tabBar = tabs_->tabBar();
  tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(tabBar, &QTabBar::customContextMenuRequested, this,
          &Workbench::showTabMenu);
  editorArea_ = new QSplitter;
  editorArea_->setChildrenCollapsible(false);
  editorArea_->addWidget(tabs_);
  setCentralWidget(editorArea_);
  const auto dock = [this](const QString &title, Qt::DockWidgetArea area,
                           QWidget *content) {
    auto *d = new QDockWidget(title, this);
    d->setObjectName(title);
    if (content)
      d->setWidget(content);
    addDockWidget(area, d);
    return d;
  };
  sidebar_ = new Sidebar(
      root_, [this](const QString &path) { openFile(path); },
      [this] { findChild<QAction *>("workspace.open")->trigger(); }, this);
  addDockWidget(Qt::LeftDockWidgetArea, sidebar_);
  setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
  resizeDocks({sidebar_}, {248}, Qt::Horizontal);
  activity_ = new ActivityBar(this);
  addToolBar(Qt::LeftToolBarArea, activity_);
  language_ = new LspPanel;
  languageDock_ =
      dock(tr("Language Results"), Qt::BottomDockWidgetArea, language_);
  languageDock_->hide();
  terminalDock_ = new TerminalDock(this);
  addDockWidget(Qt::BottomDockWidgetArea, terminalDock_);
  terminalDock_->hide();
  terminalDock_->installEventFilter(this);
  findDock_ = dock(tr("Find and Replace"), Qt::BottomDockWidgetArea, nullptr);
  findDock_->hide();
  findInFiles_ = new FindInFilesPanel;
  findInFiles_->setRoot(root_);
  searchDock_ = dock(tr("Find in Files"), Qt::BottomDockWidgetArea, findInFiles_);
  searchDock_->hide();
  connect(findInFiles_, &FindInFilesPanel::matchActivated, this,
          [this](const QString &path, int line, int column) {
            if (!openFile(path) || !current())
              return;
            current()->active->goToLineColumn(line, column);
            current()->active->setFocus();
          });
  lsp_ = new LspClient(this);
  if (!settings_["clangd"].toString().isEmpty())
    lsp_->setExecutable(settings_["clangd"].toString());
  lsp_->setWorkspace(root_);
  connect(lsp_, &LspClient::errorOccurred, this, &Workbench::report);
  connect(lsp_, &LspClient::diagnosticsReady, language_,
          &LspPanel::showDiagnostics);
  connect(lsp_, &LspClient::locationsReady, this,
          [this](const QString &title, const QList<LspLocation> &items) {
            language_->showLocations(title, items);
            languageDock_->show();
            if (items.size() == 1) {
              const auto &loc = items.first();
              emit language_->locationActivated(loc.path, loc.range.start.line,
                                                loc.range.start.character);
            }
          });
  connect(language_, &LspPanel::locationActivated, this,
          [this](const QString &path, int line, int character) {
            if (!openFile(path))
              return;
            auto *view = current()->active;
            const QString text = QString::fromUtf8(view->textUtf8());
            const int offset =
                lspOffset(text, {line, character}, lsp_->positionEncoding());
            view->setCursorPosition(text.left(offset).toUtf8().size());
            view->setFocus();
          });
  connect(lsp_, &LspClient::completionReady, this,
          [this](const QString &path, int offset,
                 const QList<LspCompletion> &items) {
            auto *tab = current();
            if (!tab || tab->file.path != path || items.isEmpty())
              return;
            QPointer<EditorView> view = tab->active;
            const auto bytes = view->textUtf8();
            const QString text = QString::fromUtf8(bytes);
            if (QString::fromUtf8(bytes.left(view->cursorPosition())).size() !=
                offset)
              return;
            auto *menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            for (const auto &item : items.mid(0, 100)) {
              menu->addAction(item.label, this, [view, bytes, text, item] {
                if (view && view->textUtf8() == bytes)
                  view->replaceRange(
                      text.left(item.startOffset).toUtf8().size(),
                      text.left(item.endOffset).toUtf8().size(),
                      item.text.toUtf8());
              });
            }
            menu->popup(view->mapToGlobal(view->rect().center()));
          });
  setupCommands();
  for (const auto *id : {"view.explorer", "view.git", "edit.find",
                         "view.terminal", "workbench.settings"})
    activity_->bind(id, findChild<QAction *>(id));
  auto refreshActivity = [this] {
    activity_->setActiveView(sidebar_->isHidden() ? QString{}
                             : sidebar_->showsSourceControl()
                                 ? "view.git"
                                 : "view.explorer");
  };
  connect(sidebar_, &QDockWidget::visibilityChanged, this, refreshActivity);
  refreshActivity();
  connect(tabs_, &QTabWidget::tabCloseRequested, this, &Workbench::closeTab);
  connect(tabs_, &QTabWidget::currentChanged, this, [this] {
    pinnedFocused_ = false;
    if (pinnedTab_ && tabs_->currentWidget() == pinnedTab_)
      pinnedTab_->active = pinnedTab_->primary;
    updateTitle(current());
    commands_.refresh();
  });
  connect(qApp, &QApplication::focusChanged, this,
          [this] { commands_.refresh(); });
  if (restoreWorkspace) {
    restoreSession(true);
    if (!tabs_->count())
      createTab({});
  }
  applyTheme();
  updateTitle(current());
  commands_.refresh();
}

Workbench *Workbench::openWorkspace(const QString &path) {
  const QString root = path.isEmpty() ? QString{} : normalizedPath(path);
  for (auto *widget : QApplication::topLevelWidgets()) {
    auto *window = dynamic_cast<Workbench *>(widget);
    if (window && window->rememberWorkspace_ && window->root_ == root &&
        window->isVisible()) {
      if (window->isMinimized())
        window->showNormal();
      window->raise();
      window->activateWindow();
      return window;
    }
  }
  auto *window = new Workbench(root);
  window->show();
  return window;
}

bool Workbench::switchWorkspace(const QString &path) {
  if (path.isEmpty())
    return false;
  const QFileInfo folder(path);
  if (!folder.isDir() || !folder.isReadable()) {
    report(tr("Cannot open folder %1").arg(path));
    return false;
  }
  const QString nextRoot = normalizedPath(path);
  if (nextRoot == root_)
    return true;

  if (terminal_ && QMessageBox::question(
                       this, tr("Switch Folder"),
                       tr("Close this workspace’s terminal and switch to %1?")
                           .arg(folder.fileName()),
                       QMessageBox::Yes | QMessageBox::Cancel,
                       QMessageBox::Cancel) != QMessageBox::Yes)
    return false;
  if (!canCloseTabs())
    return false;

  rememberSession();
  const bool reopenTerminal = terminal_ && terminalDock_->isVisible();
  if (terminal_)
    terminalDock_->close();
  unpinSplit();
  while (tabs_->count()) {
    auto *tab = tabs_->widget(0);
    tabs_->removeTab(0);
    delete tab;
  }

  root_ = nextRoot;
  rememberWorkspace_ = true;
  lsp_->setWorkspace(root_);
  language_->clear();
  findInFiles_->setRoot(root_);
  searchDock_->hide();
  sidebar_->setWorkspace(root_);
  activity_->setActiveView(sidebar_->isHidden() ? QString{} : "view.explorer");
  restoreSession(false);
  if (!current())
    createTab({});
  updateTitle(current());
  applyTheme();
  commands_.refresh();
  if (reopenTerminal)
    toggleTerminal();
  return true;
}

Workbench::~Workbench() {
  disconnect(qApp, nullptr, this, nullptr);
  for (auto *child : findChildren<QObject *>())
    disconnect(child, nullptr, this, nullptr);
}

bool Workbench::eventFilter(QObject *object, QEvent *event) {
  if (object == terminalDock_ && event->type() == QEvent::Close && terminal_) {
    disconnect(terminal_, nullptr, this, nullptr);
    terminal_->deleteLater();
    terminal_ = nullptr;
  }
  return QMainWindow::eventFilter(object, event);
}

DocumentTab *Workbench::current() const {
  return pinnedFocused_ ? pinnedTab_
                        : static_cast<DocumentTab *>(tabs_->currentWidget());
}
void Workbench::report(const QString &message) {
  statusBar()->showMessage(message, 12000);
}

DocumentTab *Workbench::createTab(FileState file) {
  auto *tab = new DocumentTab;
  tab->setChildrenCollapsible(false);
  tab->file = std::move(file);
  tab->primary = new EditorView;
  tab->primary->setLanguageForPath(tab->file.path);
  tab->primary->setTextUtf8(tab->file.bytes);
  tab->file.bytes.clear();
  tab->primary->setSavePoint();
  tab->active = tab->primary;
  tab->addWidget(tab->primary);
  tab->sync = new QTimer(tab);
  tab->sync->setSingleShot(true);
  tab->sync->setInterval(120);
  attachTab(tab);
  return tab;
}
void Workbench::attachTab(DocumentTab *tab) {
  connect(tab->sync, &QTimer::timeout, this, [this, tab] {
    lsp_->changeDocument(tab->file.path,
                         QString::fromUtf8(tab->primary->textUtf8()));
  });
  connect(tab->primary, &EditorView::textChanged, this, [this, tab] {
    if (tab->languageService) {
      lsp_->invalidateDocument(tab->file.path);
      tab->sync->start();
    }
  });
  bindView(tab, tab->primary);
  if (tab->secondary)
    bindView(tab, tab->secondary);
  tabs_->setCurrentIndex(tabs_->addTab(tab, titleFor(tab)));
  tab->active->setFocus();
  updateTitle(tab);
  updateLanguage(tab);
}
void Workbench::updateLanguage(DocumentTab *tab) {
  tab->languageService = !root_.isEmpty() &&
                         supportsClangDocument(tab->file.path) &&
                         QFileInfo(tab->file.path).size() <= 2 * 1024 * 1024;
  if (tab->languageService)
    lsp_->openDocument(tab->file.path,
                       QString::fromUtf8(tab->primary->textUtf8()));
}
void Workbench::bindView(DocumentTab *tab, EditorView *view) {
  view->setTheme(colors(currentTheme()));
  view->setMinimapVisible(minimap_);
  view->setTextWrapEnabled(settings_["textWrap"].toBool(true));
  connect(view, &EditorView::editorFocused, this, [this, tab, view] {
    pinnedFocused_ = tab == pinnedTab_ && view == tab->secondary;
    tab->active = view;
    updateTitle(tab);
    commands_.refresh();
  });
  connect(view, &EditorView::modificationChanged, this,
          [this, tab] { updateTitle(tab); });
  connect(view, &EditorView::cursorChanged, this, [this, view] {
    if (current() && current()->active == view)
      statusBar()->showMessage(tr("Line %1   Column %2")
                                   .arg(view->cursorLine() + 1)
                                   .arg(view->cursorColumn() + 1));
  });
}
void Workbench::toggleSplit(DocumentTab *tab) {
  if (!tab)
    return;
  if (tab->secondary) {
    const bool wasPinned = tab == pinnedTab_;
    if (wasPinned)
      unpinSplit();
    tab->active = tab->primary;
    delete tab->secondary;
    tab->secondary = nullptr;
    if (wasPinned)
      tab = static_cast<DocumentTab *>(tabs_->currentWidget());
  } else {
    splitTab(tab, false);
    return;
  }
  tab->active->setFocus();
}
void Workbench::splitTab(DocumentTab *tab, bool left) {
  if (!tab->secondary) {
    tab->secondary = new EditorView;
    tab->secondary->shareDocumentFrom(*tab->primary);
    bindView(tab, tab->secondary);
    tab->insertWidget(left ? 0 : 1, tab->secondary);
    tab->setSizes({1, 1});
  } else {
    tab->insertWidget(left ? 0 : 1, tab->secondary);
  }
  tab->active = tab->secondary;
  tab->active->setFocus();
}
void Workbench::pinSplit(DocumentTab *tab, bool left) {
  if (pinnedTab_ != tab) {
    unpinSplit();
    splitTab(tab, left);
    pinnedTab_ = tab;
    pinnedPane_ = new QTabWidget;
    pinnedPane_->setDocumentMode(true);
    pinnedPane_->setTabsClosable(true);
    pinnedPane_->tabBar()->setStyleSheet(tabs_->tabBar()->styleSheet());
    pinnedPane_->addTab(tab->secondary, titleFor(tab));
    connect(pinnedPane_, &QTabWidget::tabCloseRequested, this,
            [this, tab] { toggleSplit(tab); });
  }
  pinnedLeft_ = left;
  editorArea_->insertWidget(left ? 0 : 1, pinnedPane_);
  editorArea_->setSizes({1, 1});
  tab->secondary->show();
  tab->secondary->setFocus();
  pinnedFocused_ = tab->secondary->hasFocus();
  updateTitle(tab);
  updateTitle(current());
  commands_.refresh();
}
void Workbench::unpinSplit() {
  if (!pinnedTab_)
    return;
  auto *tab = pinnedTab_;
  pinnedTab_ = nullptr;
  pinnedFocused_ = false;
  pinnedPane_->removeTab(0);
  tab->insertWidget(pinnedLeft_ ? 0 : 1, tab->secondary);
  pinnedPane_->hide();
  pinnedPane_->setParent(this);
  pinnedPane_->deleteLater();
  pinnedPane_ = nullptr;
  tab->setSizes({1, 1});
  tab->secondary->show();
}
void Workbench::showTabMenu(const QPoint &position) {
  auto *bar = tabs_->tabBar();
  const int index = bar->tabAt(position);
  if (index < 0)
    return;
  tabs_->setCurrentIndex(index);
  auto *tab = static_cast<DocumentTab *>(tabs_->widget(index));
  QMenu menu(bar);
  menu.addAction(tr("Split Left"), this, [this, tab] { pinSplit(tab, true); });
  menu.addAction(tr("Split Right"), this, [this, tab] { pinSplit(tab, false); });
  menu.addAction(tr("Move to New Window"), this,
                 [this, tab] { moveTabToNewWindow(tab); });
  menu.addSeparator();
  menu.addAction(tr("Close Tab"), this,
                 [this, tab] { closeTab(tabs_->indexOf(tab)); });
  menu.exec(bar->mapToGlobal(position));
}
void Workbench::moveTabToNewWindow(DocumentTab *tab) {
  if (tab == pinnedTab_)
    unpinSplit();
  auto *window = new Workbench(root_, false);
  tab->sync->stop();
  if (tab->languageService)
    lsp_->closeDocument(tab->file.path);
  disconnect(tab->sync, nullptr, this, nullptr);
  disconnect(tab->primary, nullptr, this, nullptr);
  if (tab->secondary)
    disconnect(tab->secondary, nullptr, this, nullptr);
  tabs_->removeTab(tabs_->indexOf(tab));
  if (!tabs_->count())
    createTab({});
  window->attachTab(tab);
  window->show();
  window->activateWindow();
}
bool Workbench::openFile(const QString &path) {
  const QString normalized = normalizedPath(path);
  for (int i = 0; i < tabs_->count(); ++i) {
    auto *tab = static_cast<DocumentTab *>(tabs_->widget(i));
    if (tab->file.path == normalized) {
      tabs_->setCurrentIndex(i);
      pinnedFocused_ = false;
      if (tab == pinnedTab_)
        tab->active = tab->primary;
      tab->active->setFocus();
      return true;
    }
  }
  FileState file;
  QString error;
  if (!loadFile(normalized, file, error)) {
    report(tr("Cannot open %1 — %2").arg(path, error));
    return false;
  }
  auto *empty = static_cast<DocumentTab *>(tabs_->currentWidget());
  const bool replaceBlank = empty && empty->file.path.isEmpty() &&
                            !empty->primary->isModified() && !empty->secondary;
  createTab(std::move(file));
  if (replaceBlank)
    closeTab(tabs_->indexOf(empty));
  return true;
}
void Workbench::updateTitle(DocumentTab *tab) {
  if (!tab)
    return;
  QString title = titleFor(tab);
  if (tab->primary->isModified())
    title += " •";
  int index = tabs_->indexOf(tab);
  if (index >= 0) {
    tabs_->setTabText(index, title);
    tabs_->setTabToolTip(index, tab->file.path);
  }
  if (tab == pinnedTab_) {
    pinnedPane_->setTabText(0, title);
    pinnedPane_->setTabToolTip(0, tab->file.path);
  }
  if (tab == current())
    setWindowTitle(
        title + " — " +
        (root_.isEmpty() ? tr("Nebula Code") : QFileInfo(root_).fileName()));
}

bool Workbench::save(DocumentTab *tab) {
  if (!tab)
    return false;
  auto file = tab->file;
  if (file.path.isEmpty()) {
    const QString target =
        QFileDialog::getSaveFileName(this, tr("Save File"), root_);
    if (target.isEmpty())
      return false;
    file.path = normalizedPath(target);
    for (int i = 0; i < tabs_->count(); ++i) {
      if (static_cast<DocumentTab *>(tabs_->widget(i))->file.path ==
          file.path) {
        report(tr("This file is open in another tab."));
        return false;
      }
    }
  }
  const auto bytes = tab->primary->textUtf8();
  QString error;
  QByteArray expected = file.diskHash, hash;
  auto result = saveFile(file, bytes, expected, hash, error);
  if (result == SaveResult::ChangedOnDisk) {
    QFile disk(file.path);
    expected.clear();
    if (disk.exists()) {
      if (!disk.open(QIODevice::ReadOnly)) {
        report(disk.errorString());
        return false;
      }
      QCryptographicHash h(QCryptographicHash::Sha256);
      if (!h.addData(&disk)) {
        report(disk.errorString());
        return false;
      }
      expected = h.result();
    }
    if (QMessageBox::warning(
            this, tr("File changed on disk"),
            tr("Replace the current contents of %1 with your edits?")
                .arg(file.path),
            QMessageBox::Save | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Save)
      return false;
    result = saveFile(file, bytes, expected, hash, error);
  }
  if (result != SaveResult::Saved) {
    report(error.isEmpty() ? tr("The file changed again. Edits are still open.")
                           : error);
    return false;
  }
  const bool newPath = tab->file.path != file.path;
  tab->file = file;
  tab->file.diskHash = hash;
  tab->primary->setSavePoint();
  if (newPath) {
    tab->primary->setLanguageForPath(file.path);
    if (tab->secondary)
      tab->secondary->setLanguageForPath(file.path);
    updateLanguage(tab);
  }
  updateTitle(tab);
  report(tr("Saved %1").arg(file.path));
  return true;
}
bool Workbench::canClose(DocumentTab *tab) {
  if (!tab->primary->isModified())
    return true;
  const auto answer = QMessageBox::question(
      this, tr("Unsaved changes"), tr("Save changes to %1?").arg(titleFor(tab)),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
      QMessageBox::Save);
  return answer == QMessageBox::Discard ||
         (answer == QMessageBox::Save && save(tab));
}
void Workbench::closeTab(int index) {
  if (index < 0)
    return;
  auto *tab = static_cast<DocumentTab *>(tabs_->widget(index));
  if (!canClose(tab))
    return;
  if (tab == pinnedTab_)
    unpinSplit();
  if (tab->languageService)
    lsp_->closeDocument(tab->file.path);
  tabs_->removeTab(index);
  delete tab;
  if (!tabs_->count())
    createTab({});
}

void Workbench::setupCommands() {
  auto *file = menuBar()->addMenu(tr("File"));
  auto *edit = menuBar()->addMenu(tr("Edit"));
  auto *view = menuBar()->addMenu(tr("View"));
  auto *tools = menuBar()->addMenu(tr("Tools"));
  auto *help = menuBar()->addMenu(tr("Help"));
  auto add = [&](QMenu *menu, const QString &id, const QString &title,
                 QKeySequence keys, std::function<void()> fn,
                 std::function<bool()> enabled = {}) {
    menu->addAction(
        commands_.add(id, title, keys, std::move(fn), std::move(enabled)));
  };
  auto hasTab = [this] { return current() != nullptr; };
  add(help, "help.credits", tr("Credits"), {}, [this] {
    QMessageBox credits(QMessageBox::NoIcon, tr("Nebula Code Credits"),
                        tr("Project owner — @saikrishna488\n"
                           "Development assistance — Codex (GPT-6.2 Sol)"),
                        QMessageBox::Ok, this);
    credits.setTextFormat(Qt::PlainText);
    credits.setTextInteractionFlags(Qt::TextSelectableByMouse |
                                    Qt::TextSelectableByKeyboard);
    credits.exec();
  });
  auto editorFocus = [this] {
    auto *w = QApplication::focusWidget();
    return current() && w &&
           (current()->active == w || current()->active->isAncestorOf(w));
  };
  add(file, "file.new", tr("New File"), QKeySequence::New,
      [this] { createTab({}); });
  add(file, "file.open", tr("Open File…"), QKeySequence::Open, [this] {
    auto path = QFileDialog::getOpenFileName(this, tr("Open File"), root_);
    if (!path.isEmpty())
      openFile(path);
  });
  add(file, "workspace.open", tr("Open Folder…"), QKeySequence("Ctrl+Shift+O"),
      [this] {
        auto path =
            QFileDialog::getExistingDirectory(this, tr("Open Folder"), root_);
        if (!path.isEmpty())
          switchWorkspace(path);
      });
  add(file, "workspace.recent", tr("Open Recent Folder…"), {}, [this] {
    QStringList choices;
    for (const auto &value :
         readJson(storagePath("recent.json"))["folders"].toArray()) {
      if (QFileInfo(value.toString()).isDir())
        choices << value.toString();
    }
    if (choices.isEmpty()) {
      report(tr("No recent folders"));
      return;
    }
    auto path = QInputDialog::getItem(this, tr("Open Recent Folder"),
                                      tr("Folder"), choices, 0, false);
    if (!path.isEmpty())
      switchWorkspace(path);
  });
  add(
      file, "file.save", tr("Save"), QKeySequence::Save,
      [this] { save(current()); }, hasTab);
  add(
      file, "file.reload", tr("Reload from Disk"), {},
      [this] {
        auto *tab = current();
        if (tab->primary->isModified() &&
            QMessageBox::question(this, tr("Reload file"),
                                  tr("Discard edits and reload from disk?"),
                                  QMessageBox::Discard | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Discard)
          return;
        FileState file;
        QString error;
        if (!loadFile(tab->file.path, file, error)) {
          report(error);
          return;
        }
        tab->primary->setTextUtf8(file.bytes);
        file.bytes.clear();
        tab->file = std::move(file);
        tab->primary->setSavePoint();
      },
      [this] { return current() && !current()->file.path.isEmpty(); });
  add(
      file, "file.close", tr("Close Tab"), QKeySequence::Close,
      [this] { closeTab(tabs_->indexOf(current())); }, hasTab);
  auto action = [&](const char *id, QString name, QKeySequence key,
                    void (EditorView::*fn)()) {
    add(
        edit, id, name, key, [this, fn] { (current()->active->*fn)(); },
        editorFocus);
  };
  action("edit.undo", tr("Undo"), QKeySequence::Undo, &EditorView::undo);
  action("edit.redo", tr("Redo"), QKeySequence::Redo, &EditorView::redo);
  action("edit.cut", tr("Cut"), QKeySequence::Cut, &EditorView::cut);
  const auto terminalFocus = [this] {
    auto *w = QApplication::focusWidget();
    return terminal_ && w && (w == terminal_ || terminal_->isAncestorOf(w));
  };
  const auto clipboardFocus = [editorFocus, terminalFocus] {
    return editorFocus() || terminalFocus();
  };
  add(
      edit, "edit.copy", tr("Copy"), QKeySequence::Copy,
      [this, terminalFocus] {
        if (terminalFocus())
          terminal_->copy();
        else
          current()->active->copy();
      },
      clipboardFocus);
  add(
      edit, "edit.paste", tr("Paste"), QKeySequence::Paste,
      [this, terminalFocus] {
        if (terminalFocus())
          terminal_->paste();
        else
          current()->active->paste();
      },
      clipboardFocus);
  action("edit.selectAll", tr("Select All"), QKeySequence::SelectAll,
         &EditorView::selectAll);
  action("edit.nextOccurrence", tr("Add Next Occurrence"),
         QKeySequence("Ctrl+D"), &EditorView::addNextOccurrence);
  action("edit.fold", tr("Toggle Fold"), QKeySequence("Ctrl+Alt+["),
         &EditorView::toggleFold);
  add(
      file, "workspace.quickOpen", tr("Quick Open…"), QKeySequence("Ctrl+P"),
      [this] {
        const QString path = quickOpenFile(this, root_);
        if (!path.isEmpty())
          openFile(path);
      },
      [this] { return !root_.isEmpty(); });
  add(
      edit, "edit.find", tr("Find and Replace"), QKeySequence::Find,
      [this] { showFind(); }, hasTab);
  add(
      edit, "edit.gotoLine", tr("Go to Line…"), QKeySequence("Ctrl+G"),
      [this] { goToLine(); }, hasTab);
  add(
      edit, "workspace.findInFiles", tr("Find in Files…"),
      QKeySequence("Ctrl+Shift+F"), [this] { showFindInFiles(); },
      [this] { return !root_.isEmpty(); });
  add(view, "view.palette", tr("Command Palette…"),
      QKeySequence("Ctrl+Shift+P"), [this] { commands_.palette(); });
  add(
      view, "view.split", tr("Toggle Split Editor"), QKeySequence("Ctrl+\\"),
      [this] { toggleSplit(current()); }, hasTab);
  add(view, "view.explorer", tr("Toggle Explorer"), QKeySequence("Ctrl+B"),
      [this] {
        sidebar_->togglePage(false);
        activity_->setActiveView(sidebar_->isHidden() ? QString{}
                                                      : "view.explorer");
      });
  add(view, "view.minimap", tr("Toggle Minimap"), {}, [this] {
    updateAppearance(theme_, !minimap_, settings_["themeOverrides"].toObject());
  });
  auto *textWrap = commands_.add("view.textWrap", tr("Text Wrap"), {}, [this] {
    updateTextWrap(!settings_["textWrap"].toBool(true));
  });
  textWrap->setCheckable(true);
  textWrap->setChecked(settings_["textWrap"].toBool(true));
  view->addAction(textWrap);
  add(view, "view.theme", tr("Choose Theme…"), {}, [this] { showSettings(); });
  add(edit, "workbench.settings", tr("Settings…"), QKeySequence("Ctrl+,"),
      [this] { showSettings(); });
  findChild<QAction *>("workbench.settings")
      ->setMenuRole(QAction::PreferencesRole);
  add(view, "view.terminal", tr("Toggle Terminal"), QKeySequence("Ctrl+`"),
      [this] { toggleTerminal(); });
  add(
      view, "view.git", tr("Toggle Source Control"), {},
      [this] {
        sidebar_->togglePage(true);
        activity_->setActiveView(sidebar_->isHidden() ? QString{} : "view.git");
      },
      [this] { return !root_.isEmpty(); });
  add(view, "view.results", tr("Toggle Language Results"), {},
      [this] { languageDock_->setVisible(!languageDock_->isVisible()); });
  auto language = [this] { return current() && current()->languageService; };
  add(
      tools, "language.complete", tr("Complete at Cursor"),
      QKeySequence("Ctrl+Space"),
      [this] { requestLanguage(LanguageAction::Completion); }, language);
  add(
      tools, "language.definition", tr("Go to Definition"), QKeySequence("F12"),
      [this] { requestLanguage(LanguageAction::Definition); }, language);
  add(
      tools, "language.references", tr("Find References"),
      QKeySequence("Shift+F12"),
      [this] { requestLanguage(LanguageAction::References); }, language);
  add(
      tools, "language.restart", tr("Restart Language Server"), {},
      [this] { lsp_->restart(); }, language);
}

void Workbench::requestLanguage(LanguageAction operation) {
  auto *tab = current();
  auto *view = tab->active;
  const auto bytes = view->textUtf8();
  tab->sync->stop();
  lsp_->changeDocument(tab->file.path, QString::fromUtf8(bytes));
  const int offset =
      QString::fromUtf8(bytes.left(view->cursorPosition())).size();
  if (operation == LanguageAction::Completion)
    lsp_->requestCompletion(tab->file.path, offset);
  else if (operation == LanguageAction::Definition)
    lsp_->requestDefinition(tab->file.path, offset);
  else
    lsp_->requestReferences(tab->file.path, offset);
}
void Workbench::showFind() {
  if (!findDock_->widget()) {
    auto *bar = new QWidget;
    auto *row = new QHBoxLayout(bar);
    auto *find = new QLineEdit;
    auto *replace = new QLineEdit;
    auto *matchCase = new QCheckBox(tr("Match Case"));
    auto *wholeWord = new QCheckBox(tr("Whole Word"));
    find->setPlaceholderText(tr("Find"));
    replace->setPlaceholderText(tr("Replace with"));
    matchCase->setObjectName(QStringLiteral("findMatchCase"));
    wholeWord->setObjectName(QStringLiteral("findWholeWord"));
    row->addWidget(find);
    row->addWidget(replace);
    row->addWidget(matchCase);
    row->addWidget(wholeWord);
    auto button = [&](const QString &name, std::function<void()> fn) {
      auto *b = new QPushButton(name);
      row->addWidget(b);
      connect(b, &QPushButton::clicked, this, fn);
    };
    auto search = [this, find, matchCase, wholeWord](bool backward) {
      if (current() &&
          !current()->active->findNext(find->text(), backward,
                                       matchCase->isChecked(),
                                       wholeWord->isChecked()))
        report(tr("No match"));
    };
    connect(find, &QLineEdit::returnPressed, this, [search] { search(false); });
    button(tr("Previous"), [search] { search(true); });
    button(tr("Next"), [search] { search(false); });
    button(tr("Replace"), [this, find, replace, matchCase, wholeWord] {
      if (current())
        current()->active->replaceCurrent(find->text(), replace->text(),
                                          matchCase->isChecked(),
                                          wholeWord->isChecked());
    });
    button(tr("Replace All"), [this, find, replace, matchCase, wholeWord] {
      if (current())
        report(tr("%1 replacements")
                   .arg(current()->active->replaceAll(
                       find->text(), replace->text(), matchCase->isChecked(),
                       wholeWord->isChecked())));
    });
    findDock_->setWidget(bar);
  }
  findDock_->show();
  auto *field = findDock_->findChild<QLineEdit *>();
  field->setFocus();
  field->selectAll();
}
void Workbench::showFindInFiles() {
  searchDock_->show();
  searchDock_->raise();
  findInFiles_->focusQuery();
}
void Workbench::goToLine() {
  if (!current())
    return;
  auto *view = current()->active;
  const QString initial = QStringLiteral("%1:%2")
                              .arg(view->cursorLine() + 1)
                              .arg(view->cursorColumn() + 1);
  bool ok = false;
  const QString input = QInputDialog::getText(
      this, tr("Go to Line"), tr("Line or line:column:"), QLineEdit::Normal,
      initial, &ok);
  if (!ok || input.trimmed().isEmpty())
    return;
  const QStringList parts = input.trimmed().split(':');
  bool lineOk = false;
  const qsizetype line = parts.value(0).toLongLong(&lineOk);
  if (!lineOk || line < 1)
    return;
  bool colOk = false;
  const qsizetype column =
      parts.size() > 1 ? parts.value(1).toLongLong(&colOk) : 1;
  view->goToLineColumn(line - 1, colOk && column >= 1 ? column - 1 : 0);
  view->setFocus();
}
void Workbench::toggleTerminal() {
  if (terminalDock_->isVisible()) {
    terminalDock_->hide();
    return;
  }
  if (!terminal_) {
    terminal_ = new TerminalSession(root_.isEmpty() ? QDir::homePath() : root_,
                                    terminalDock_);
    terminal_->setDarkTheme(currentTheme()["dark"].toBool());
    terminalDock_->setWidget(terminal_);
    connect(terminal_, &TerminalSession::finished, this, [this] {
      terminalDock_->hide();
      terminal_->deleteLater();
      terminal_ = nullptr;
    });
    if (!terminal_->start(settings_["shell"].toString())) {
      report(terminal_->errorString());
      delete terminal_;
      terminal_ = nullptr;
      return;
    }
  }
  terminalDock_->show();
  terminal_->setFocus();
  commands_.refresh();
}
QJsonObject Workbench::currentTheme() const {
  return loadTheme(theme_, settings_["themeOverrides"].toObject());
}

void Workbench::showSettings() {
  AppearanceDialog dialog(theme_, minimap_,
                          settings_["themeOverrides"].toObject(), this);
  dialog.setPalette(palette());
  if (dialog.exec() == QDialog::Accepted)
    updateAppearance(dialog.themeName(), dialog.minimapVisible(),
                     dialog.themeOverrides());
}

void Workbench::updateTextWrap(bool enabled) {
  const QString path = storagePath("settings.json");
  auto updated = readJson(path);
  updated["textWrap"] = enabled;
  QString error;
  if (!writeJson(path, updated, error)) {
    findChild<QAction *>("view.textWrap")->setChecked(
        settings_["textWrap"].toBool(true));
    QMessageBox::warning(this, tr("Cannot Save Settings"), error);
    return;
  }
  for (auto *widget : QApplication::topLevelWidgets()) {
    if (auto *window = dynamic_cast<Workbench *>(widget)) {
      window->settings_["textWrap"] = enabled;
      window->findChild<QAction *>("view.textWrap")->setChecked(enabled);
      for (auto *editor : window->editorArea_->findChildren<EditorView *>())
        editor->setTextWrapEnabled(enabled);
    }
  }
}

void Workbench::updateAppearance(const QString &theme, bool minimap,
                                 const QJsonObject &overrides) {
  const QString path = storagePath("settings.json");
  auto updated = readJson(path);
  updated["theme"] = theme;
  updated["minimap"] = minimap;
  updated["themeOverrides"] = overrides;
  QString error;
  if (!writeJson(path, updated, error)) {
    QMessageBox::warning(this, tr("Cannot Save Settings"), error);
    return;
  }
  for (auto *widget : QApplication::topLevelWidgets()) {
    if (auto *window = dynamic_cast<Workbench *>(widget)) {
      window->settings_ = updated;
      window->theme_ = theme;
      window->minimap_ = minimap;
      window->applyTheme();
    }
  }
}
void Workbench::applyTheme() {
  auto obj = currentTheme();
  auto theme = colors(obj);
  QPalette p = palette();
  QColor panel(obj["panel"].toString());
  for (auto role :
       {QPalette::Window, QPalette::Button, QPalette::AlternateBase})
    p.setColor(role, panel);
  for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
                    QPalette::HighlightedText})
    p.setColor(role, theme.foreground);
  p.setColor(QPalette::Base, theme.background);
  p.setColor(QPalette::Highlight, theme.selection);
  p.setColor(QPalette::PlaceholderText, theme.muted);
  const QColor sidebarText =
      panel.lightnessF() > 0.52 ? QColor("#333333") : QColor("#eeeeee");
  const QColor accent =
      obj["dark"].toBool() ? QColor("#1683cf") : QColor("#0068b5");
  p.setColor(QPalette::WindowText, sidebarText);
  p.setColor(QPalette::ButtonText, sidebarText);
  setPalette(p);
  sidebar_->setColors(panel, sidebarText, accent);
  activity_->setColors(panel, sidebarText, accent);
  tabs_->tabBar()->setStyleSheet(
      QStringLiteral("QTabBar::tab { background: %1; color: %2; padding: 6px "
                     "10px; border-top: 2px solid transparent; }"
                     "QTabBar::tab:selected { background: %3; color: %4; "
                     "border-top-color: %5; }")
          .arg(panel.name(), sidebarText.name(), theme.background.name(),
               theme.foreground.name(), accent.name()));
  if (pinnedPane_)
    pinnedPane_->tabBar()->setStyleSheet(tabs_->tabBar()->styleSheet());

  for (auto *v : editorArea_->findChildren<EditorView *>()) {
    v->setTheme(theme);
    v->setMinimapVisible(minimap_);
  }
  if (terminal_)
    terminal_->setDarkTheme(obj["dark"].toBool());
}
void Workbench::restoreSession(bool restoreWindowGeometry) {
  auto data = readJson(sessionPath(root_));
  const auto terminalArea = Qt::DockWidgetArea(
      data["terminalArea"].toInt(Qt::BottomDockWidgetArea));
  switch (terminalArea) {
  case Qt::TopDockWidgetArea:
  case Qt::RightDockWidgetArea:
  case Qt::BottomDockWidgetArea:
  case Qt::LeftDockWidgetArea:
    addDockWidget(terminalArea, terminalDock_);
    break;
  default:
    addDockWidget(Qt::BottomDockWidgetArea, terminalDock_);
    break;
  }
  for (const auto &entry : data["tabs"].toArray()) {
    const auto obj = entry.toObject();
    const auto path = obj["path"].toString();
    if (path.isEmpty() || !openFile(path))
      continue;
    auto *tab = static_cast<DocumentTab *>(tabs_->currentWidget());
    tab->primary->setCursorPosition(obj["cursor"].toInteger());
    if (obj["split"].toBool()) {
      if (obj["pinned"].toBool())
        pinSplit(tab, obj["splitLeft"].toBool());
      else
        splitTab(tab, obj["splitLeft"].toBool());
      tab->secondary->setCursorPosition(obj["secondCursor"].toInteger());
      auto *splitter = tab == pinnedTab_ ? editorArea_ : tab;
      splitter->restoreState(
          QByteArray::fromBase64(obj["sizes"].toString().toLatin1()));
      if (!obj["focusSecond"].toBool())
        tab->active = tab->primary;
    }
  }
  const auto selected = data["active"].toString();
  for (int i = 0; i < tabs_->count(); ++i) {
    auto *tab = static_cast<DocumentTab *>(tabs_->widget(i));
    if (tab->file.path == selected) {
      tabs_->setCurrentIndex(i);
      auto *view = tab == pinnedTab_ ? tab->primary : tab->active;
      view->setFocus();
      break;
    }
  }
  if (pinnedTab_ && data["pinnedFocused"].toBool())
    pinnedTab_->secondary->setFocus();
  if (restoreWindowGeometry)
    restoreGeometry(
        QByteArray::fromBase64(data["geometry"].toString().toLatin1()));
}
void Workbench::rememberSession() {
  if (!rememberWorkspace_)
    return;
  QJsonArray files;
  for (int i = 0; i < tabs_->count(); ++i) {
    auto *tab = static_cast<DocumentTab *>(tabs_->widget(i));
    if (tab->file.path.isEmpty())
      continue;
    files.append(QJsonObject{
        {"path", tab->file.path},
        {"cursor", qint64(tab->primary->cursorPosition())},
        {"split", tab->secondary != nullptr},
        {"pinned", tab == pinnedTab_},
        {"splitLeft", tab == pinnedTab_
                          ? pinnedLeft_
                          : tab->secondary && tab->indexOf(tab->secondary) == 0},
        {"secondCursor",
         qint64(tab->secondary ? tab->secondary->cursorPosition() : 0)},
        {"focusSecond", tab->secondary && tab->active == tab->secondary},
        {"sizes", QString::fromLatin1((tab == pinnedTab_ ? editorArea_ : tab)
                                  ->saveState().toBase64())}});
  }
  auto *selected = static_cast<DocumentTab *>(tabs_->currentWidget());
  QString error;
  if (!writeJson(sessionPath(root_),
                 {{"tabs", files},
                  {"terminalArea", int(dockWidgetArea(terminalDock_))},
                  {"active", selected ? selected->file.path : QString()},
                  {"pinnedFocused", pinnedFocused_},
                  {"geometry", QString::fromLatin1(saveGeometry().toBase64())}},
                 error))
    report(error);
  if (!root_.isEmpty()) {
    auto recent = readJson(storagePath("recent.json"))["folders"].toArray();
    QJsonArray folders{root_};
    for (const auto &v : recent)
      if (v.toString() != root_ && folders.size() < 12)
        folders.append(v);
    if (!writeJson(storagePath("recent.json"), {{"folders", folders}}, error))
      report(error);
  }
}
bool Workbench::canCloseTabs() {
  for (int i = 0; i < tabs_->count(); ++i) {
    if (!canClose(static_cast<DocumentTab *>(tabs_->widget(i))))
      return false;
  }
  return true;
}

void Workbench::closeEvent(QCloseEvent *event) {
  if (!canCloseTabs()) {
    event->ignore();
    return;
  }
  rememberSession();
  QMainWindow::closeEvent(event);
}
} // namespace nebula
