#include "app/Workbench.h"
#include "app/Theme.h"
#include "editor/EditorView.h"
#include "terminal/TerminalSession.h"
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QTreeView>
#include <QPushButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QSpinBox>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

using namespace nebula;
class WorkbenchTest : public QObject {
  Q_OBJECT
private slots:
  void displaysCredits() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("NEBULA_CONFIG_DIR", dir.filePath("config").toUtf8());
    Workbench window(dir.path());
    auto *action = window.findChild<QAction *>("help.credits");
    QVERIFY(action);
    QString text;
    QTimer::singleShot(0, &window, [&] {
      auto *dialog = window.findChild<QMessageBox *>();
      if (dialog) {
        text = dialog->text();
        dialog->accept();
      }
    });
    action->trigger();
    QVERIFY(text.contains("@saikrishna488"));
    QVERIFY(text.contains("Codex (GPT-6.2 Sol)"));
  }

  void clipboardShortcutsFollowFocus() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("NEBULA_CONFIG_DIR", dir.filePath("config").toUtf8());
    QString error;
    QVERIFY(writeJson(storagePath("settings.json"), {{"shell", "/bin/sh"}},
                      error));
    Workbench window(dir.path());
    window.show();
    window.activateWindow();
    auto *editor = window.findChild<EditorView *>();
    QVERIFY(editor);
    editor->setTextUtf8("Copy from the editor");
    editor->selectAll();
    editor->setFocus();
    QTRY_COMPARE(QApplication::focusWidget(), editor->focusProxy());
    QTest::keySequence(editor->focusProxy(), QKeySequence::Copy);
    QCOMPARE(QApplication::clipboard()->text(), QString("Copy from the editor"));

    window.findChild<QAction *>("view.terminal")->trigger();
    auto *dock = window.findChild<QDockWidget *>("Terminal");
    auto *terminal = dock->findChild<TerminalSession *>();
    QVERIFY(terminal);
    auto *copy = window.findChild<QAction *>("edit.copy");
    QSignalSpy copied(copy, &QAction::triggered);
    const auto copyFromTerminal = [&](QWidget *activeWindow, int count) {
      activeWindow->activateWindow();
      terminal->setFocus();
      QTRY_VERIFY(terminal->isAncestorOf(QApplication::focusWidget()));
      QVERIFY(copy->isEnabled());
      QWidget *focus = QApplication::focusWidget();
      QTest::keySequence(focus, QKeySequence::Copy);
      QCOMPARE(copied.count(), count);
    };
    copyFromTerminal(&window, 1);
    dock->setFloating(true);
    copyFromTerminal(dock, 2);
  }

  void sidebarAppearanceAndTerminal() {
    QTemporaryDir dir;
    qputenv("NEBULA_CONFIG_DIR", dir.filePath("config").toUtf8());
    QString error;
    QVERIFY(
        writeJson(storagePath("settings.json"), {{"shell", "/bin/sh"}}, error));
    Workbench window(dir.path());
    window.show();
    QCoreApplication::processEvents();
    auto *sidebar = window.findChild<QDockWidget *>("WorkspaceSidebar");
    QVERIFY(sidebar && sidebar->isVisible());
    auto *rail = window.findChild<QToolBar *>("nebulaActivityBar");
    QVERIFY(rail && rail->isVisible());
    QVERIFY(window.toolBarArea(rail) == Qt::LeftToolBarArea);
    QCOMPARE(sidebar->palette().window().color(), QColor("#f3f3f3"));
    auto clickRail = [&](const QString &name) {
      for (auto *button : rail->findChildren<QToolButton *>())
        if (button->accessibleName() == name) {
          button->click();
          return true;
        }
      return false;
    };
    QVERIFY(clickRail("Explorer"));
    QVERIFY(sidebar->isHidden());
    QVERIFY(clickRail("Source Control"));
    QVERIFY(sidebar->isVisible());
    QVERIFY(clickRail("Explorer"));
    QVERIFY(sidebar->isVisible());
    Workbench second(dir.path());
    auto setDark = [](bool save) {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      QVERIFY(dialog);
      auto *theme = dialog->findChild<QComboBox *>();
      theme->setCurrentIndex(theme->findData("dark"));
      dialog->findChild<QSpinBox *>()->setValue(15);
      dialog->findChild<QCheckBox *>()->setChecked(false);
      if (save)
        dialog->accept();
      else
        dialog->reject();
    };
    QTimer::singleShot(0, &window, [setDark] { setDark(false); });
    QVERIFY(clickRail("Settings"));
    QVERIFY(!readJson(storagePath("settings.json")).contains("theme"));
    QTimer::singleShot(0, &window, [setDark] { setDark(true); });
    QVERIFY(clickRail("Settings"));
    const auto stored = readJson(storagePath("settings.json"));
    QCOMPARE(stored["theme"].toString(), "dark");
    QCOMPARE(stored["shell"].toString(), "/bin/sh");
    QVERIFY(!stored["minimap"].toBool(true));
    QCOMPARE(stored["themeOverrides"]
                 .toObject()["dark"]
                 .toObject()["pointSize"]
                 .toInt(),
             15);
    QCOMPARE(second.palette().base().color(), QColor("#1f1f1f"));
    QVERIFY(clickRail("Terminal"));
    auto *dock = window.findChild<QDockWidget *>("Terminal");
    QVERIFY(dock && dock->isVisible());
    QVERIFY(window.dockWidgetArea(dock) == Qt::BottomDockWidgetArea);
    QPointer<TerminalSession> shell = dock->findChild<TerminalSession *>();
    QVERIFY(shell);
    dock->setFloating(true);
    QVERIFY(dock->isFloating());
    for (auto *action : dock->findChildren<QAction *>())
      if (action->text() == "Move to Right")
        action->trigger();
    QVERIFY(!dock->isFloating());
    QVERIFY(window.dockWidgetArea(dock) == Qt::RightDockWidgetArea);
    QCOMPARE(dock->widget(), shell.data());
    dock->close();
    QTRY_VERIFY(shell.isNull());
  }

  void sessionAndConflicts() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("NEBULA_CONFIG_DIR", dir.filePath("config").toUtf8());
    QCoreApplication::setOrganizationName("NebulaFixture");
    QCoreApplication::setApplicationName(dir.path().section('/', -1));
    auto path = dir.filePath("sample.txt");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("abc\r\n");
    f.close();
    auto *window = new Workbench(dir.path());
    window->show();
    QVERIFY(window->openFile(path));
    window->findChild<QAction *>("view.split")->trigger();
    auto *tabs = window->findChild<QTabWidget *>();
    auto *tab = static_cast<DocumentTab *>(tabs->currentWidget());
    QVERIFY(tab->secondary);
    tab->primary->replaceRange(0, 3, "updated");
    QCOMPARE(tab->secondary->textUtf8(), "updated\r\n");
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("other\n");
    f.close();
    QTimer::singleShot(0, window, [] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
        box->done(QMessageBox::Cancel);
    });
    window->findChild<QAction *>("file.save")->trigger();
    QVERIFY(tab->primary->isModified());
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), "other\n");
    f.close();
    tab->primary->undo();
    window->findChild<QAction *>("file.reload")->trigger();
    QCOMPARE(tab->secondary->textUtf8(), "other\n");
    tab->primary->setCursorPosition(1);
    tab->secondary->setCursorPosition(3);
    QVERIFY(window->close());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    window = new Workbench(dir.path());
    tabs = window->findChild<QTabWidget *>();
    QCOMPARE(tabs->count(), 1);
    tab = static_cast<DocumentTab *>(tabs->currentWidget());
    QVERIFY(tab->secondary);
    QCOMPARE(tab->file.path, normalizedPath(path));
    QCOMPARE(tab->primary->cursorPosition(), 1);
    QCOMPARE(tab->secondary->cursorPosition(), 3);
    delete window;
  }

  void openFolderFromEmptySidebarReusesWindow() {
    QTemporaryDir dir;
    qputenv("NEBULA_CONFIG_DIR", dir.filePath("config").toUtf8());
    Workbench window(QString{});
    window.show();
    auto *open = window.findChild<QPushButton *>("sidebarOpenFolder");
    QVERIFY(open && open->isVisible());
    QTimer::singleShot(0, &window, [&dir] {
      auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
      QVERIFY(dialog);
      dialog->setDirectory(dir.path());
      QMetaObject::invokeMethod(dialog, "accept");
    });
    open->click();
    auto *tree = window.findChild<QTreeView *>("workspaceFiles");
    QVERIFY(tree && tree->isVisible());
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    QVERIFY(model);
    QCOMPARE(normalizedPath(model->filePath(tree->rootIndex())), normalizedPath(dir.path()));
    QVERIFY(!open->isVisible());
  }

  void recentFolderSwitchKeepsWindowAndRestoresSessions() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("NEBULA_CONFIG_DIR", dir.filePath("config").toUtf8());
    const auto firstRoot = dir.filePath("first");
    const auto secondRoot = dir.filePath("second");
    QVERIFY(QDir().mkpath(firstRoot));
    QVERIFY(QDir().mkpath(secondRoot));
    const auto firstPath = firstRoot + "/sample.txt";
    const auto secondPath = secondRoot + "/sample.txt";
    for (const auto &path : {firstPath, secondPath}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write("sample\n"), 7);
    }
    {
      Workbench recent(secondRoot);
      recent.setAttribute(Qt::WA_DeleteOnClose, false);
      QVERIFY(recent.openFile(secondPath));
      auto *tabs = recent.findChild<QTabWidget *>();
      auto *tab = static_cast<DocumentTab *>(tabs->currentWidget());
      tab->primary->setCursorPosition(4);
      QVERIFY(recent.close());
    }

    Workbench window(firstRoot);
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    window.show();
    QVERIFY(window.openFile(firstPath));
    auto *tabs = window.findChild<QTabWidget *>();
    auto *tab = static_cast<DocumentTab *>(tabs->currentWidget());
    tab->primary->replaceRange(0, 6, "edited");
    QTimer::singleShot(0, &window, [] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
        box->done(QMessageBox::Cancel);
    });
    QVERIFY(!window.switchWorkspace(secondRoot));
    QCOMPARE(tabs->currentWidget(), tab);
    QVERIFY(tab->primary->isModified());
    QCOMPARE(tab->primary->textUtf8(), "edited\n");
    tab->primary->undo();
    QVERIFY(!tab->primary->isModified());
    window.findChild<QAction *>("view.split")->trigger();
    QVERIFY(tab->secondary);
    tab->primary->setCursorPosition(1);
    tab->secondary->setCursorPosition(3);

    auto *recent = window.findChild<QAction *>("workspace.recent");
    QVERIFY(recent);
    QTimer::singleShot(0, &window, [] {
      if (auto *dialog =
              qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
        dialog->accept();
    });
    recent->trigger();
    QVERIFY(window.isVisible());
    tabs = window.findChild<QTabWidget *>();
    QCOMPARE(tabs->count(), 1);
    tab = static_cast<DocumentTab *>(tabs->currentWidget());
    QCOMPARE(tab->file.path, normalizedPath(secondPath));
    QCOMPARE(tab->primary->cursorPosition(), 4);
    for (auto *widget : QApplication::topLevelWidgets()) {
      if (widget->isVisible() && dynamic_cast<Workbench *>(widget))
        QCOMPARE(widget, &window);
    }

    QVERIFY(window.switchWorkspace(firstRoot));
    tabs = window.findChild<QTabWidget *>();
    QCOMPARE(tabs->count(), 1);
    tab = static_cast<DocumentTab *>(tabs->currentWidget());
    QCOMPARE(tab->file.path, normalizedPath(firstPath));
    QVERIFY(tab->secondary);
    QCOMPARE(tab->primary->cursorPosition(), 1);
    QCOMPARE(tab->secondary->cursorPosition(), 3);
  }
};
QTEST_MAIN(WorkbenchTest)
#include "WorkbenchTest.moc"
