#include "terminal/TerminalSession.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <qtermwidget.h>

#include <pwd.h>
#include <unistd.h>

namespace nebula {
namespace {

QString resolveShell(const QString &shell)
{
    if (!shell.isEmpty())
        return QStandardPaths::findExecutable(shell);

    const QString environmentShell = QStandardPaths::findExecutable(qEnvironmentVariable("SHELL"));
    if (!environmentShell.isEmpty())
        return environmentShell;

    if (const passwd *user = getpwuid(getuid())) {
        const QString loginShell = QStandardPaths::findExecutable(QFile::decodeName(user->pw_shell));
        if (!loginShell.isEmpty())
            return loginShell;
    }
    return QStandardPaths::findExecutable(QStringLiteral("/bin/sh"));
}

} // namespace

TerminalSession::TerminalSession(const QString &workingDirectory, QWidget *parent)
    : QWidget(parent),
      terminal_(new QTermWidget(0, this)),
      workingDirectory_(workingDirectory.isEmpty() ? QDir::homePath()
                                                   : QDir(workingDirectory).absolutePath())
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(terminal_);
    setFocusProxy(terminal_);
    terminal_->setTerminalSizeHint(false);
    terminal_->setHistorySize(5000);
    terminal_->setScrollBarPosition(QTermWidget::ScrollBarRight);
    terminal_->setFlowControlEnabled(false);
    setDarkTheme(true);
    connect(terminal_, &QTermWidget::finished, this, &TerminalSession::finished, Qt::QueuedConnection);
}

bool TerminalSession::start(const QString &shell, const QStringList &arguments)
{
    if (started_) {
        error_ = tr("This terminal session has already been started.");
        return false;
    }
    const QFileInfo directory(workingDirectory_);
    if (!directory.isDir() || !directory.isExecutable()) {
        error_ = tr("Cannot access the terminal directory “%1”.").arg(workingDirectory_);
        return false;
    }
    const QString executable = resolveShell(shell);
    if (executable.isEmpty()) {
        error_ = shell.isEmpty() ? tr("No executable shell was found.")
                                 : tr("Cannot run the shell “%1”.").arg(shell);
        return false;
    }

    QStringList environment{QStringLiteral("TERM=xterm-256color")};
    QStringList shellArguments = arguments;
#ifdef Q_OS_MACOS
    // Match Terminal.app's login-shell startup so GUI launches load the user's PATH.
    const QString shellName = QFileInfo(executable).fileName();
    if (shellArguments.isEmpty()
        && (shellName == QStringLiteral("zsh") || shellName == QStringLiteral("bash"))) {
        shellArguments.append(QStringLiteral("-l"));
    }
    if (qEnvironmentVariableIsEmpty("LC_ALL") && qEnvironmentVariableIsEmpty("LC_CTYPE")
        && qEnvironmentVariableIsEmpty("LANG")) {
        environment.append(QStringLiteral("LC_CTYPE=en_US.UTF-8"));
    }
#endif
    terminal_->setEnvironment(environment);
    terminal_->setWorkingDirectory(workingDirectory_);
    terminal_->setShellProgram(executable);
    terminal_->setArgs(shellArguments);
    terminal_->startShellProgram();
    if (terminal_->getShellPID() <= 0) {
        error_ = tr("Could not start “%1” in a pseudo-terminal.").arg(executable);
        return false;
    }
    started_ = true;
    error_.clear();
    return true;
}

QString TerminalSession::errorString() const
{
    return error_;
}

void TerminalSession::setDarkTheme(bool dark)
{
    terminal_->setColorScheme(dark ? QStringLiteral("WhiteOnBlack") : QStringLiteral("BlackOnWhite"));
}

void TerminalSession::copy()
{
    terminal_->copyClipboard();
}

void TerminalSession::paste()
{
    terminal_->pasteClipboard();
}

} // namespace nebula
