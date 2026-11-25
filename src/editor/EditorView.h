#pragma once

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QString>
#include <QWidget>

class ScintillaEditBase;

namespace nebula {

struct EditorTheme {
    QColor background;
    QColor foreground;
    QColor muted;
    QColor selection;
    QColor currentLine;
    QColor keyword;
    QColor string;
    QColor comment;
    QColor number;
    QColor preprocessor;
    QFont font;
};

class Minimap;

class EditorView final : public QWidget {
    Q_OBJECT

public:
    explicit EditorView(QWidget *parent = nullptr);

    QByteArray textUtf8() const;
    void setTextUtf8(const QByteArray &text);
    void setLanguageForPath(const QString &path);
    void shareDocumentFrom(EditorView &source);
    void setDarkTheme(bool dark);
    void setTheme(const EditorTheme &theme);
    void setMinimapVisible(bool visible);
    void setTextWrapEnabled(bool enabled);
    void addNextOccurrence();
    void toggleFold();
    bool findNext(const QString &text, bool backwards = false, bool caseSensitive = false);
    bool replaceCurrent(const QString &text, const QString &replacement, bool caseSensitive = false);
    qsizetype replaceAll(const QString &text, const QString &replacement, bool caseSensitive = false);
    void replaceRange(qsizetype startByte, qsizetype endByte, const QByteArray &text);
    qsizetype cursorPosition() const;
    qsizetype cursorLine() const;
    qsizetype cursorColumn() const;
    void setCursorPosition(qsizetype position);
    void goToLineColumn(qsizetype line, qsizetype column);
    void undo();
    void redo();
    void cut();
    void copy();
    void paste();
    void selectAll();
    void setSavePoint();
    bool isModified() const;

signals:
    void modificationChanged(bool modified);
    void editorFocused();
    void cursorChanged();
    void textChanged();

private:
    void updateLineNumberWidth();

    ScintillaEditBase *editor_;
    Minimap *minimap_;
    EditorTheme theme_;
    QString path_;
};

} // namespace nebula
