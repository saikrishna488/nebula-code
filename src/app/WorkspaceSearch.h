#pragma once

#include <QString>
#include <QWidget>
#include <atomic>
#include <memory>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace nebula {

QString quickOpenFile(QWidget *parent, const QString &root);

class FindInFilesPanel final : public QWidget {
  Q_OBJECT

public:
  explicit FindInFilesPanel(QWidget *parent = nullptr);
  ~FindInFilesPanel() override;

  void setRoot(const QString &root);
  void focusQuery();
  void startSearch();

signals:
  void matchActivated(const QString &path, int line, int byteColumn);

private:
  void cancelSearch();

  QString root_;
  QLineEdit *query_ = nullptr;
  QCheckBox *caseSensitive_ = nullptr;
  QCheckBox *wholeWord_ = nullptr;
  QPushButton *searchButton_ = nullptr;
  QLabel *status_ = nullptr;
  QListWidget *results_ = nullptr;
  std::shared_ptr<std::atomic_bool> cancelled_;
};

} // namespace nebula
