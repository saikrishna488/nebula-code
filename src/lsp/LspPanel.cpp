#include "LspPanel.h"

#include <QFileInfo>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace nebula {

namespace {

enum ItemRole { PathRole = Qt::UserRole, LineRole, CharacterRole };

QString diagnosticKind(int severity)
{
    switch (severity) {
    case 1: return QObject::tr("Error");
    case 2: return QObject::tr("Warning");
    case 3: return QObject::tr("Information");
    case 4: return QObject::tr("Hint");
    default: return QObject::tr("Diagnostic");
    }
}

}

LspPanel::LspPanel(QWidget *parent)
    : QWidget(parent), tabs_(new QTabWidget(this)), diagnostics_(new QTreeWidget(this)), results_(new QTreeWidget(this))
{
    tabs_->addTab(diagnostics_, tr("Diagnostics"));
    tabs_->addTab(results_, tr("Locations"));
    diagnostics_->setHeaderLabels({tr("File or Diagnostic"), tr("Line"), tr("Message")});
    results_->setHeaderLabels({tr("File"), tr("Line"), tr("Path")});
    results_->setRootIsDecorated(false);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tabs_);

    const auto activate = [this](QTreeWidgetItem *item) {
        const auto path = item->data(0, PathRole).toString();
        if (!path.isEmpty())
            emit locationActivated(path, item->data(0, LineRole).toInt(),
                                  item->data(0, CharacterRole).toInt());
    };
    connect(diagnostics_, &QTreeWidget::itemActivated, this, activate);
    connect(results_, &QTreeWidget::itemActivated, this, activate);
}

void LspPanel::showDiagnostics(const QString &path, const QList<LspDiagnostic> &diagnostics)
{
    delete diagnosticFiles_.take(path);
    if (diagnostics.isEmpty())
        return;
    auto *file = new QTreeWidgetItem(diagnostics_, {QFileInfo(path).fileName(), {}, path});
    diagnosticFiles_.insert(path, file);
    for (const auto &item : diagnostics) {
        addLocation(file, {diagnosticKind(item.severity), QString::number(item.range.start.line + 1),
                           item.message},
                    path, item.range.start);
    }
    file->setExpanded(true);
}

void LspPanel::showLocations(const QString &title, const QList<LspLocation> &locations)
{
    results_->clear();
    const int index = tabs_->indexOf(results_);
    tabs_->setTabText(index, title);
    tabs_->setCurrentIndex(index);
    if (locations.isEmpty())
        new QTreeWidgetItem(results_, {tr("No locations found.")});
    for (const auto &item : locations) {
        addLocation(nullptr, {QFileInfo(item.path).fileName(), QString::number(item.range.start.line + 1),
                              item.path},
                    item.path, item.range.start);
    }
}

void LspPanel::clear()
{
    diagnosticFiles_.clear();
    diagnostics_->clear();
    results_->clear();
}

QTreeWidgetItem *LspPanel::addLocation(QTreeWidgetItem *parent, const QStringList &labels,
                                      const QString &path, LspPosition position)
{
    auto *item = parent ? new QTreeWidgetItem(parent, labels) : new QTreeWidgetItem(results_, labels);
    item->setData(0, PathRole, path);
    item->setData(0, LineRole, position.line);
    item->setData(0, CharacterRole, position.character);
    item->setToolTip(2, labels.value(2));
    return item;
}

}
