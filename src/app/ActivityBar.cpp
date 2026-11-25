#include "app/ActivityBar.h"

#include <QAction>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QSizePolicy>
#include <QToolButton>
#include <cmath>

namespace nebula {

namespace {

enum class Icon { Explorer, Branch, Find, Terminal, Settings };

void drawIcon(QPainter &painter, Icon icon) {
  switch (icon) {
  case Icon::Explorer:
    painter.drawPolyline(QPolygonF{{3, 15}, {3, 3}, {13, 3}, {16, 6}});
    painter.drawPolyline(QPolygonF{{7, 7}, {16, 7}, {20, 11}, {20, 21},
                                  {7, 21}, {7, 7}});
    painter.drawPolyline(QPolygonF{{16, 7}, {16, 11}, {20, 11}});
    painter.drawLine(QPointF(10, 15), QPointF(17, 15));
    painter.drawLine(QPointF(10, 18), QPointF(17, 18));
    break;
  case Icon::Branch:
    painter.drawLine(QPointF(7, 7), QPointF(7, 17));
    painter.drawEllipse(QPointF(7, 5), 2, 2);
    painter.drawEllipse(QPointF(7, 19), 2, 2);
    painter.drawEllipse(QPointF(17, 5), 2, 2);
    {
      QPainterPath branch(QPointF(17, 7));
      branch.cubicTo(17, 14, 7, 10, 7, 16);
      painter.drawPath(branch);
    }
    break;
  case Icon::Find:
    painter.drawEllipse(QPointF(10, 10), 6, 6);
    painter.drawLine(QPointF(14.3, 14.3), QPointF(20, 20));
    break;
  case Icon::Terminal:
    painter.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2);
    painter.drawPolyline(QPolygonF{{7, 9}, {10, 12}, {7, 15}});
    painter.drawLine(QPointF(12, 15), QPointF(17, 15));
    break;
  case Icon::Settings: {
    constexpr qreal turn = 6.283185307179586;
    QPolygonF teeth;
    for (int tooth = 0; tooth < 8; ++tooth) {
      for (int edge = 0; edge < 4; ++edge) {
        const qreal angle = (tooth * 4 + edge) * turn / 32;
        const qreal radius = edge == 0 || edge == 3 ? 7.2 : 9.3;
        teeth << QPointF(12 + radius * std::cos(angle),
                         12 + radius * std::sin(angle));
      }
    }
    painter.drawPolygon(teeth);
    painter.drawEllipse(QPointF(12, 12), 3, 3);
    break;
  }
  }
}

} // namespace

class ActivityBar::Button : public QToolButton {
public:
  Button(Icon icon, const QString &label, QWidget *parent)
      : QToolButton(parent), icon_(icon) {
    setFixedSize(46, 46);
    setAutoRaise(true);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(label);
    setAccessibleName(label);
    setEnabled(false);
    connect(this, &QToolButton::clicked, this, [this] {
      if (command_)
        command_->trigger();
    });
  }

  void bind(QAction *command) {
    disconnect(changed_);
    disconnect(destroyed_);
    command_ = command;
    setEnabled(command && command->isEnabled());
    if (command) {
      changed_ = connect(command, &QAction::changed, this, [this] {
        setEnabled(command_ && command_->isEnabled());
      });
      destroyed_ = connect(command, &QObject::destroyed, this,
                           [this] { setEnabled(false); });
    }
  }

  void setActive(bool active) {
    active_ = active;
    update();
  }

  void setColors(const QColor &foreground, const QColor &accent) {
    foreground_ = foreground;
    accent_ = accent;
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    const QColor foreground =
        foreground_.isValid() ? foreground_ : palette().windowText().color();
    const QColor accent =
        accent_.isValid() ? accent_ : palette().highlight().color();

    if (isEnabled() && (underMouse() || isDown())) {
      QColor hover = foreground;
      hover.setAlpha(isDown() ? 40 : 22);
      painter.fillRect(rect(), hover);
    }
    if (active_)
      painter.fillRect(QRect(0, 0, 2, height()), accent);
    if (hasFocus()) {
      painter.setPen(accent);
      painter.drawRect(rect().adjusted(3, 3, -4, -4));
    }

    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(!isEnabled() ? 0.4 : active_ || underMouse() ? 1.0 : 0.75);
    painter.setPen(QPen(foreground, 1.5, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.translate((width() - 24) / 2.0, (height() - 24) / 2.0);
    drawIcon(painter, icon_);
  }

private:
  Icon icon_;
  QPointer<QAction> command_;
  QMetaObject::Connection changed_;
  QMetaObject::Connection destroyed_;
  QColor foreground_;
  QColor accent_;
  bool active_ = false;
};

ActivityBar::ActivityBar(QWidget *parent)
    : QToolBar(tr("Activity Bar"), parent) {
  setObjectName(QStringLiteral("nebulaActivityBar"));
  setAllowedAreas(Qt::LeftToolBarArea);
  setOrientation(Qt::Vertical);
  setMovable(false);
  setFloatable(false);
  setFixedWidth(46);

  const auto addButton = [this](const QString &id, Icon icon,
                                const QString &label) {
    auto *button = new Button(icon, label, this);
    buttons_.insert(id, button);
    addWidget(button);
  };

  addButton(QStringLiteral("view.explorer"), Icon::Explorer, tr("Explorer"));
  addButton(QStringLiteral("view.git"), Icon::Branch, tr("Source Control"));
  addButton(QStringLiteral("edit.find"), Icon::Find, tr("Find in Document"));
  auto *spacer = new QWidget(this);
  spacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
  addWidget(spacer);
  addButton(QStringLiteral("view.terminal"), Icon::Terminal, tr("Terminal"));
  addButton(QStringLiteral("workbench.settings"), Icon::Settings, tr("Settings"));
  setColors({}, {}, {});
}

void ActivityBar::bind(const QString &id, QAction *action) {
  if (auto *button = buttons_.value(id))
    button->bind(action);
}

void ActivityBar::setActiveView(const QString &id) {
  for (auto it = buttons_.cbegin(); it != buttons_.cend(); ++it)
    it.value()->setActive(it.key() == id);
}

void ActivityBar::setColors(QColor background, QColor foreground, QColor accent) {
  const auto color = background.isValid() ? background : palette().window().color();
  setStyleSheet(QStringLiteral("QToolBar#nebulaActivityBar {"
                               "background: %1; border: 0; padding: 0;"
                               "margin: 0; spacing: 0; }")
                    .arg(color.name()));
  for (auto *button : buttons_)
    button->setColors(foreground, accent);
}

} // namespace nebula
