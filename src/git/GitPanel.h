#pragma once

#include "GitRepository.h"

#include <QWidget>

class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace nebula {

class GitPanel : public QWidget {
    Q_OBJECT

public:
    explicit GitPanel(QWidget *parent = nullptr);
    void setWorkspace(const QString &root);

private:
    const GitEntry *selectedEntry() const;
    void updateStatus(const QList<GitEntry> &entries);
    void updateControls();

    GitRepository *repository_;
    QLabel *status_;
    QLabel *error_;
    QTreeWidget *files_;
    QPlainTextEdit *diff_;
    QLineEdit *message_;
    QPushButton *refresh_;
    QPushButton *stage_;
    QPushButton *unstage_;
    QPushButton *stagedDiff_;
    QPushButton *unstagedDiff_;
    QPushButton *commit_;
    QTimer *statusRefreshTimer_;
    QList<GitEntry> entries_;
};

}
