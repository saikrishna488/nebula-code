#pragma once

#include "LspClient.h"
#include <QWidget>

class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace nebula {

class LspPanel : public QWidget {
    Q_OBJECT

public:
    explicit LspPanel(QWidget *parent = nullptr);

    void showDiagnostics(const QString &path, const QList<LspDiagnostic> &diagnostics);
    void showLocations(const QString &title, const QList<LspLocation> &locations);
    void clear();

signals:
    void locationActivated(const QString &path, int line, int character);

private:
    QTreeWidgetItem *addLocation(QTreeWidgetItem *parent, const QStringList &labels,
                                const QString &path, LspPosition position);
    QTabWidget *tabs_;
    QTreeWidget *diagnostics_;
    QTreeWidget *results_;
    QHash<QString, QTreeWidgetItem *> diagnosticFiles_;
};

}
