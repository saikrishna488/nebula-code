#pragma once

#include <QStringList>
#include <QWidget>

class QTermWidget;

namespace nebula {

class TerminalSession final : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalSession(const QString &workingDirectory, QWidget *parent = nullptr);

    bool start(const QString &shell = {}, const QStringList &arguments = {});
    QString errorString() const;
    void setDarkTheme(bool dark);
    void copy();
    void paste();

signals:
    void finished();

private:
    QTermWidget *terminal_;
    QString workingDirectory_;
    QString error_;
    bool started_ = false;
};

} // namespace nebula
