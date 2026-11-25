#pragma once

#include <QDockWidget>

class QMainWindow;

namespace nebula {

class TerminalDock final : public QDockWidget
{
    Q_OBJECT

public:
    explicit TerminalDock(QMainWindow *parent);
};

} // namespace nebula
