#include "TerminalDock.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QToolButton>

namespace nebula {
namespace {

class DockGrip final : public QWidget
{
public:
    explicit DockGrip(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedWidth(style()->pixelMetric(QStyle::PM_ToolBarHandleExtent));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStyleOption option;
        option.initFrom(this);
        option.state |= QStyle::State_Horizontal;
        QPainter painter(this);
        style()->drawPrimitive(QStyle::PE_IndicatorToolBarHandle, &option, &painter, this);
    }
};

} // namespace

TerminalDock::TerminalDock(QMainWindow *parent)
    : QDockWidget(tr("Terminal"), parent)
{
    setObjectName(QStringLiteral("Terminal"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea |
                    Qt::BottomDockWidgetArea);
    setFeatures(DockWidgetClosable | DockWidgetMovable | DockWidgetFloatable);

    auto *titleBar = new QWidget(this);
    titleBar->setCursor(Qt::OpenHandCursor);
    titleBar->setToolTip(tr("Drag the terminal to move or detach it"));

    auto *title = new QLabel(windowTitle(), titleBar);
    title->setAttribute(Qt::WA_TransparentForMouseEvents);

    auto *placement = new QToolButton(titleBar);
    placement->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    placement->setAccessibleName(tr("Terminal placement"));
    placement->setToolTip(tr("Terminal placement"));
    placement->setCursor(Qt::ArrowCursor);
    placement->setAutoRaise(true);
    placement->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(placement);
    placement->setMenu(menu);

    auto *floatAction = menu->addAction(tr("Detach Terminal"), this, [this] {
        setFloating(!isFloating());
    });
    connect(this, &QDockWidget::topLevelChanged, floatAction, [this, floatAction](bool floating) {
        floatAction->setText(floating ? tr("Dock Terminal") : tr("Detach Terminal"));
    });
    menu->addSeparator();
    const auto addPlacement = [this, parent, menu](const QString &label, Qt::DockWidgetArea area) {
        menu->addAction(label, this, [this, parent, area] {
            setFloating(false);
            parent->addDockWidget(area, this);
        });
    };
    addPlacement(tr("Move to Bottom"), Qt::BottomDockWidgetArea);
    addPlacement(tr("Move to Left"), Qt::LeftDockWidgetArea);
    addPlacement(tr("Move to Right"), Qt::RightDockWidgetArea);

    auto *close = new QToolButton(titleBar);
    close->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    close->setAccessibleName(tr("Close Terminal"));
    close->setToolTip(tr("Close Terminal"));
    close->setCursor(Qt::ArrowCursor);
    close->setAutoRaise(true);
    connect(close, &QToolButton::clicked, this, &QWidget::close);

    auto *layout = new QHBoxLayout(titleBar);
    layout->setContentsMargins(4, 0, 2, 0);
    layout->setSpacing(4);
    layout->addWidget(new DockGrip(titleBar));
    layout->addWidget(title);
    layout->addStretch();
    layout->addWidget(placement);
    layout->addWidget(close);
    setTitleBarWidget(titleBar);
}

} // namespace nebula
