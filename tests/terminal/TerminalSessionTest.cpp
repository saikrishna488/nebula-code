#include "terminal/TerminalSession.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <qtermwidget.h>

#include <cerrno>
#include <csignal>

using nebula::TerminalSession;

class TerminalSessionTest final : public QObject
{
    Q_OBJECT

private slots:
    void rejectsInvalidLaunch();
    void runsInteractiveShellInWorkspace();
    void editsFileInFullScreenVim();
    void reapsShellWhenDestroyed();
};

void TerminalSessionTest::rejectsInvalidLaunch()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalSession missingDirectory(QDir(directory.path()).filePath(QStringLiteral("missing")));
    QVERIFY(!missingDirectory.start());
    QVERIFY(!missingDirectory.errorString().isEmpty());

    TerminalSession missingProgram(directory.path());
    QVERIFY(!missingProgram.start(QStringLiteral("/no-such-shell")));
    QVERIFY(!missingProgram.errorString().isEmpty());
}

void TerminalSessionTest::runsInteractiveShellInWorkspace()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray output;
    TerminalSession session(directory.path());
    session.resize(700, 300);
    session.show();
    session.setFocus();
    auto *widget = session.findChild<QTermWidget *>();
    QVERIFY(widget);
    QCOMPARE(widget->historySize(), 5000);
    QVERIFY(QTermWidget::availableColorSchemes().contains(QStringLiteral("BlackOnWhite")));
    QVERIFY(QTermWidget::availableColorSchemes().contains(QStringLiteral("WhiteOnBlack")));
    QVERIFY(QTermWidget::availableKeyBindings().contains(QStringLiteral("default")));
    connect(widget, &QTermWidget::receivedData, &session,
        [&output](const QString &data) { output += data.toLatin1(); });
    QSignalSpy finished(&session, &TerminalSession::finished);

    QVERIFY2(session.start(QStringLiteral("/bin/bash"),
                          {QStringLiteral("--noprofile"), QStringLiteral("--norc"),
                           QStringLiteral("-i")}), qPrintable(session.errorString()));
    widget->sendText(QStringLiteral("set +H; stty -echo; printf 'READY_%s\\n' yes\n"));
    QTRY_VERIFY_WITH_TIMEOUT(output.contains("READY_yes"), 4000);
    widget->sendText(QStringLiteral("test -t 0 && test -t 1 && printf 'PTY_%s\\n' yes; pwd -P\n"));
    QTRY_VERIFY(output.contains("PTY_yes"));
    QTRY_VERIFY(output.contains(QDir(directory.path()).canonicalPath().toUtf8()));

    const int originalColumns = widget->screenColumnsCount();
    session.resize(840, 400);
    QTRY_VERIFY(widget->screenColumnsCount() > originalColumns);
    const QByteArray dimensions = QByteArray::number(widget->screenLinesCount()) + ' '
                                + QByteArray::number(widget->screenColumnsCount());
    widget->sendText(QStringLiteral("stty size\n"));
    QTRY_VERIFY(output.contains(dimensions));

    QApplication::clipboard()->setText(QStringLiteral("printf 'UNICODE_%s\\n' 'naïve 世界'"));
    session.paste();
    widget->sendText(QStringLiteral("\n"));
    QTRY_VERIFY(output.contains(QByteArray("UNICODE_naïve 世界")));

    widget->sendText(QStringLiteral("sleep 60 & worker=$!; "
                                    "sh -c 'printf \"FOREGROUND_%s\\n\" ready; exec sleep 60'\n"));
    QTRY_VERIFY2_WITH_TIMEOUT(output.contains("FOREGROUND_ready"), output.constData(), 4000);
#ifdef Q_OS_MACOS
    constexpr auto terminalControlModifier = Qt::MetaModifier;
#else
    constexpr auto terminalControlModifier = Qt::ControlModifier;
#endif
    QTest::keyClick(widget->focusProxy(), Qt::Key_C, terminalControlModifier);
    widget->sendText(QStringLiteral("printf 'INTERRUPT_%s\\n' \"$?\"; "
                                    "kill -0 \"$worker\" && printf 'JOB_%s\\n' active; "
                                    "kill \"$worker\"; wait \"$worker\"; exit 0\n"));
    QTRY_VERIFY2_WITH_TIMEOUT(output.contains("INTERRUPT_130"), output.constData(), 4000);
    QTRY_VERIFY(output.contains("JOB_active"));
    QTRY_COMPARE(finished.count(), 1);
}

void TerminalSessionTest::editsFileInFullScreenVim()
{
    const QString vim = QStandardPaths::findExecutable(QStringLiteral("vim"));
    if (vim.isEmpty())
        QSKIP("vim is unavailable");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray output;
    TerminalSession session(directory.path());
    session.resize(720, 360);
    session.show();
    auto *widget = session.findChild<QTermWidget *>();
    QVERIFY(widget);
    connect(widget, &QTermWidget::receivedData, &session,
        [&output](const QString &data) { output += data.toLatin1(); });
    QSignalSpy finished(&session, &TerminalSession::finished);

    QVERIFY2(session.start(vim, {QStringLiteral("-Nu"), QStringLiteral("NONE"),
                                 QStringLiteral("-n"), QStringLiteral("-i"),
                                 QStringLiteral("NONE"), QStringLiteral("-c"),
                                 QStringLiteral("echo 'NEBULA_VIM_READY'"),
                                 QStringLiteral("terminal.txt")}),
             qPrintable(session.errorString()));
    QTRY_VERIFY2_WITH_TIMEOUT(output.contains("NEBULA_VIM_READY"), output.constData(), 4000);
    QWidget *display = widget->focusProxy();
    QTest::keyClicks(display, "iNebula terminal");
    QTest::keyClick(display, Qt::Key_Escape);
    QTest::keyClicks(display, ":wq");
    QTest::keyClick(display, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 4000);
    QFile saved(QDir(directory.path()).filePath(QStringLiteral("terminal.txt")));
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), QByteArray("Nebula terminal\n"));
}

void TerminalSessionTest::reapsShellWhenDestroyed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    pid_t pid;
    {
        TerminalSession session(directory.path());
        QVERIFY2(session.start(QStringLiteral("/bin/sh")), qPrintable(session.errorString()));
        pid = session.findChild<QTermWidget *>()->getShellPID();
        QVERIFY(pid > 0);
    }
    QTRY_VERIFY(kill(pid, 0) == -1 && errno == ESRCH);
}

QTEST_MAIN(TerminalSessionTest)
#include "TerminalSessionTest.moc"
