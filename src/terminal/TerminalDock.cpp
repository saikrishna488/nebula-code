#include "TerminalDock.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QStyle>
#include <QToolButton>

namespace nebula {
TerminalDock::TerminalDock(QMainWindow *parent)
    : QDockWidget(tr("Terminal"), parent)
{
    setObjectName(QStringLiteral("Terminal"));
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setFeatures(DockWidgetClosable);

    auto *titleBar = new QWidget(this);
    auto *title = new QLabel(windowTitle(), titleBar);

    auto *placement = new QToolButton(titleBar);
    placement->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    placement->setAccessibleName(tr("Terminal placement"));
    placement->setToolTip(tr("Terminal placement"));
    placement->setAutoRaise(true);
    placement->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(placement);
    placement->setMenu(menu);

    const auto addPlacement = [this, parent, menu](const QString &label, Qt::DockWidgetArea area) {
        menu->addAction(label, this, [this, parent, area] {
            parent->addDockWidget(area, this);
        });
    };
    addPlacement(tr("Move to Top"), Qt::TopDockWidgetArea);
    addPlacement(tr("Move to Bottom"), Qt::BottomDockWidgetArea);
    addPlacement(tr("Move to Left"), Qt::LeftDockWidgetArea);
    addPlacement(tr("Move to Right"), Qt::RightDockWidgetArea);

    auto *close = new QToolButton(titleBar);
    close->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    close->setAccessibleName(tr("Close Terminal"));
    close->setToolTip(tr("Close Terminal"));
    close->setAutoRaise(true);
    connect(close, &QToolButton::clicked, this, &QWidget::close);

    auto *layout = new QHBoxLayout(titleBar);
    layout->setContentsMargins(4, 0, 2, 0);
    layout->setSpacing(4);
    layout->addWidget(title);
    layout->addStretch();
    layout->addWidget(placement);
    layout->addWidget(close);
    setTitleBarWidget(titleBar);
}

} // namespace nebula
