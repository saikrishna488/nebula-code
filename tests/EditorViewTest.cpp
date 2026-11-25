#include "editor/EditorView.h"

#include <SciLexer.h>
#include <ScintillaEditBase.h>

#include <QImage>
#include <QSignalSpy>
#include <QTest>

#include <memory>

using nebula::EditorView;

class EditorViewTest final : public QObject {
    Q_OBJECT

private slots:
    void utf8AndLineEndings();
    void sharedDocumentAndMultiCaretUndo();
    void syntaxAndFolding();
    void markdownSyntax();
    void findAndReplace();
    void largeFileRemainsEditable();
    void shortDocumentMinimap();
};

void EditorViewTest::utf8AndLineEndings()
{
    EditorView view;
    auto *scintilla = view.findChild<ScintillaEditBase *>();
    view.setTextUtf8(QByteArray("é漢\r\nline\r\n"));
    QCOMPARE(view.textUtf8(), QByteArray("é漢\r\nline\r\n"));
    QCOMPARE(scintilla->send(SCI_GETEOLMODE), sptr_t{SC_EOL_CRLF});
    QVERIFY(!view.isModified());
    view.goToLineColumn(0, 2);
    QCOMPARE(view.cursorPosition(), qsizetype{5});
    QTest::keyClick(scintilla, Qt::Key_Return);
    QCOMPARE(view.textUtf8(), QByteArray("é漢\r\n\r\nline\r\n"));
}

void EditorViewTest::sharedDocumentAndMultiCaretUndo()
{
    auto first = std::make_unique<EditorView>();
    EditorView second;
    first->setTextUtf8("word word\n");
    second.shareDocumentFrom(*first);
    QCOMPARE(second.textUtf8(), first->textUtf8());
    second.setCursorPosition(9);
    QCOMPARE(first->cursorPosition(), qsizetype{0});

    auto *scintilla = first->findChild<ScintillaEditBase *>();
    first->addNextOccurrence();
    first->addNextOccurrence();
    QCOMPARE(scintilla->send(SCI_GETSELECTIONS), sptr_t{2});
    QSignalSpy splitChanged(&second, &EditorView::textChanged);
    QTest::keyClicks(scintilla, "x");
    QCOMPARE(second.textUtf8(), QByteArray("x x\n"));
    QVERIFY(!splitChanged.isEmpty());
    QVERIFY(second.isModified());

    first.reset();
    second.undo();
    QCOMPARE(second.textUtf8(), QByteArray("word word\n"));
    QVERIFY(!second.isModified());
    second.redo();
    QCOMPARE(second.textUtf8(), QByteArray("x x\n"));
}

void EditorViewTest::syntaxAndFolding()
{
    EditorView view;
    view.setLanguageForPath(QStringLiteral("example.cpp"));
    view.setTextUtf8("int main() {\n    return 42;\n}\n");
    auto *scintilla = view.findChild<ScintillaEditBase *>();
    QSignalSpy textChanged(&view, &EditorView::textChanged);
    scintilla->send(SCI_COLOURISE, 0, -1);
    QCOMPARE(scintilla->send(SCI_GETSTYLEAT, 0), sptr_t{SCE_C_WORD});
    QCOMPARE(scintilla->send(SCI_GETSTYLEAT, view.textUtf8().indexOf("42")), sptr_t{SCE_C_NUMBER});
    QVERIFY(scintilla->send(SCI_GETFOLDLEVEL, 0) & SC_FOLDLEVELHEADERFLAG);
    view.toggleFold();
    QCOMPARE(scintilla->send(SCI_GETLINEVISIBLE, 1), sptr_t{0});
    view.toggleFold();
    QCOMPARE(scintilla->send(SCI_GETLINEVISIBLE, 1), sptr_t{1});
    view.setLanguageForPath(QStringLiteral("example.txt"));
    QCOMPARE(scintilla->send(SCI_GETSTYLEAT, 0), sptr_t{0});
    QVERIFY(textChanged.isEmpty());
}

void EditorViewTest::markdownSyntax()
{
    EditorView view;
    auto *scintilla = view.findChild<ScintillaEditBase *>();
    view.setLanguageForPath(QStringLiteral("README.MD"));
    view.setTextUtf8("# Heading\n\n**bold** with `code` and [link](https://example.com)\n");
    scintilla->send(SCI_COLOURISE, 0, -1);
    QCOMPARE(scintilla->send(SCI_GETLEXER), sptr_t{SCLEX_MARKDOWN});
    QCOMPARE(scintilla->send(SCI_GETSTYLEAT, 0), sptr_t{SCE_MARKDOWN_HEADER1});
    QCOMPARE(scintilla->send(SCI_GETSTYLEAT, view.textUtf8().indexOf("bold")), sptr_t{SCE_MARKDOWN_STRONG1});
    QCOMPARE(scintilla->send(SCI_GETSTYLEAT, view.textUtf8().indexOf("code")), sptr_t{SCE_MARKDOWN_CODE});
    QCOMPARE(scintilla->send(SCI_GETSTYLEAT, view.textUtf8().indexOf("link")), sptr_t{SCE_MARKDOWN_LINK});
    view.setLanguageForPath(QStringLiteral("guide.markdown"));
    QCOMPARE(scintilla->send(SCI_GETLEXER), sptr_t{SCLEX_MARKDOWN});
    view.setLanguageForPath(QStringLiteral("guide.txt"));
    QCOMPARE(scintilla->send(SCI_GETSTYLEAT, 0), sptr_t{SCE_MARKDOWN_DEFAULT});
}

void EditorViewTest::findAndReplace()
{
    EditorView view;
    view.setTextUtf8("é é É\n");
    QVERIFY(view.findNext(QStringLiteral("é")));
    QCOMPARE(view.cursorPosition(), qsizetype{2});
    QVERIFY(view.findNext(QStringLiteral("é"), true));
    QCOMPARE(view.cursorPosition(), qsizetype{8});
    QVERIFY(view.replaceCurrent(QStringLiteral("é"), QStringLiteral("日")));
    QCOMPARE(view.textUtf8(), QByteArray("é é 日\n"));
    QCOMPARE(view.replaceAll(QStringLiteral("é"), QStringLiteral("éé")), qsizetype{2});
    QCOMPARE(view.textUtf8(), QByteArray("éé éé 日\n"));
    view.undo();
    QCOMPARE(view.textUtf8(), QByteArray("é é 日\n"));
    view.replaceRange(0, 2, "x");
    QCOMPARE(view.textUtf8(), QByteArray("x é 日\n"));
}

void EditorViewTest::largeFileRemainsEditable()
{
    EditorView view;
    auto *scintilla = view.findChild<ScintillaEditBase *>();
    const QByteArray fixture = QByteArray("int value;\n").repeated(800000);
    view.setLanguageForPath(QStringLiteral("large.cpp"));
    view.setTextUtf8(fixture);
    QCOMPARE(scintilla->send(SCI_GETLEXER), sptr_t{SCLEX_CONTAINER});
    QCOMPARE(view.textUtf8(), fixture);
    view.replaceRange(4, 9, "other");
    QVERIFY(view.textUtf8().startsWith("int other;\n"));
    view.undo();
    QCOMPARE(view.textUtf8(), fixture);
}

void EditorViewTest::shortDocumentMinimap()
{
    EditorView view;
    view.resize(480, 320);
    view.setTextUtf8("short document");
    view.show();
    const auto image = view.grab().toImage();
    const auto scale = image.devicePixelRatio();
    const auto right = image.width() - qRound(8 * scale);
    QCOMPARE(image.pixelColor(right, qRound(40 * scale)), image.pixelColor(right, qRound(100 * scale)));
}

QTEST_MAIN(EditorViewTest)
#include "EditorViewTest.moc"
