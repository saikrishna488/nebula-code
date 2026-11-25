#include "EditorView.h"

#include <ILexer.h>
#include <Lexilla.h>
#include <SciLexer.h>
#include <ScintillaEditBase.h>

#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <array>

namespace nebula {
namespace {

constexpr int lineNumberMargin = 0;
constexpr int foldMargin = 2;
constexpr qsizetype syntaxLimit = 8 * 1024 * 1024;

constexpr const char *cppKeywords =
    "alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t "
    "class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield "
    "decltype default delete do double dynamic_cast else enum explicit export extern false final float for "
    "friend goto if import inline int long module mutable namespace new noexcept not not_eq nullptr operator "
    "or or_eq override private protected public register reinterpret_cast requires return short signed sizeof "
    "static static_assert static_cast struct switch template this thread_local throw true try typedef typeid "
    "typename union unsigned using virtual void volatile wchar_t while xor xor_eq";

uptr_t rgb(const QColor &color)
{
    return color.red() | (color.green() << 8) | (color.blue() << 16);
}

EditorTheme builtInTheme(bool dark)
{
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(12);
    if (dark) {
        return {QColor("#111723"), QColor("#e5eaf7"), QColor("#8290a6"), QColor("#314970"),
                QColor("#1a2333"), QColor("#bfadff"), QColor("#a1ddb9"), QColor("#8e9db4"),
                QColor("#f6bc91"), QColor("#91c8ff"), font};
    }
    return {QColor("#fafbfe"), QColor("#2a3448"), QColor("#59677c"), QColor("#c9dbfa"),
            QColor("#eef2f9"), QColor("#5634a2"), QColor("#296648"), QColor("#5f6c82"),
            QColor("#85481c"), QColor("#1d548e"), font};
}

void setForegrounds(ScintillaEditBase *editor, const QColor &color, std::initializer_list<int> styles)
{
    for (const int style : styles)
        editor->send(SCI_STYLESETFORE, style, rgb(color));
}

} // namespace

class Minimap final : public QWidget {
public:
    Minimap(ScintillaEditBase *editor, QWidget *parent)
        : QWidget(parent), editor_(editor)
    {
        setFixedWidth(90);
        setAccessibleName(tr("Document minimap"));
        setCursor(Qt::PointingHandCursor);

        rebuildTimer_.setSingleShot(true);
        rebuildTimer_.setInterval(80);
        connect(&rebuildTimer_, &QTimer::timeout, this, [this] {
            rebuild();
            update();
        });
        connect(editor_, &ScintillaEditBase::verticalScrolled, this, [this] { update(); });
        connect(editor_, &ScintillaEditBase::modified, this, [this](Scintilla::ModificationFlags flags) {
            if (static_cast<int>(flags) & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT | SC_MOD_CHANGESTYLE))
                invalidate();
        });
    }

    void setTheme(const EditorTheme &theme)
    {
        theme_ = theme;
        invalidate();
    }

    void invalidate()
    {
        dirty_ = true;
        if (isVisible() && !rebuildTimer_.isActive())
            rebuildTimer_.start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (cache_.size() != size() * devicePixelRatioF() || cache_.devicePixelRatio() != devicePixelRatioF()
            || (dirty_ && !rebuildTimer_.isActive())) {
            rebuild();
        }
        QPainter painter(this);
        painter.fillRect(rect(), theme_.currentLine);
        painter.drawPixmap(0, 0, cache_);
        if (editor_->send(SCI_GETLENGTH) == 0)
            return;
        const auto lineCount = std::max<sptr_t>(1, editor_->send(SCI_GETLINECOUNT));
        const auto firstVisible = editor_->send(SCI_GETFIRSTVISIBLELINE);
        const auto firstLine = std::clamp<sptr_t>(
            editor_->send(SCI_DOCLINEFROMVISIBLE, firstVisible), 0, lineCount - 1);
        const auto lastLine = std::clamp<sptr_t>(
            editor_->send(SCI_DOCLINEFROMVISIBLE, firstVisible + editor_->send(SCI_LINESONSCREEN) - 1),
            firstLine, lineCount - 1);
        const auto y = static_cast<int>(firstLine * overviewHeight_ / lineCount);
        const auto visibleHeight = static_cast<int>((std::max(firstLine, lastLine) - firstLine + 1)
                                                     * overviewHeight_ / lineCount);
        QColor viewport = theme_.selection;
        viewport.setAlpha(110);
        painter.fillRect(QRect(0, y, width(), std::max(3, visibleHeight)).intersected(rect()), viewport);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            jump(event->position().y());
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons().testFlag(Qt::LeftButton))
            jump(event->position().y());
    }

private:
    void rebuild()
    {
        const qreal scale = devicePixelRatioF();
        cache_ = QPixmap((size() * scale));
        cache_.setDevicePixelRatio(scale);
        cache_.fill(theme_.currentLine);
        QPainter painter(&cache_);
        const auto lineCount = std::max<sptr_t>(1, editor_->send(SCI_GETLINECOUNT));
        const auto rows = std::min({lineCount, static_cast<sptr_t>(std::max(1, height() / 2)),
                                    static_cast<sptr_t>(400)});
        overviewHeight_ = static_cast<int>(rows * 2);

        std::array<QColor, 256> colors;
        for (unsigned int style = 0; style < colors.size(); ++style) {
            const auto color = editor_->send(SCI_STYLEGETFORE, style);
            colors[style] = QColor(color & 0xff, (color >> 8) & 0xff, (color >> 16) & 0xff, 205);
        }

        for (sptr_t row = 0; row < rows; ++row) {
            const auto line = row * lineCount / rows;
            const auto start = editor_->send(SCI_POSITIONFROMLINE, line);
            const auto end = std::min(editor_->send(SCI_GETLINEENDPOSITION, line),
                                      start + (width() - 10) * 4);
            int x = 5;
            for (auto position = start; position < end && x < width() - 5; ++position) {
                const auto character = editor_->send(SCI_GETCHARAT, position);
                if (character == '\t') {
                    x += 4;
                } else if ((character & 0xc0) != 0x80) {
                    if (character != ' ') {
                        const auto style = editor_->send(SCI_GETSTYLEINDEXAT, position);
                        painter.fillRect(x, static_cast<int>(row * 2), 1, 1, colors[style & 0xff]);
                    }
                    ++x;
                }
            }
        }

        dirty_ = false;
    }

    void jump(qreal y)
    {
        const auto lineCount = std::max<sptr_t>(1, editor_->send(SCI_GETLINECOUNT));
        const auto target = std::clamp<sptr_t>(
            static_cast<sptr_t>(y * lineCount / std::max(1, overviewHeight_)), 0, lineCount - 1);
        auto visibleLine = editor_->send(SCI_VISIBLEFROMDOCLINE, target);
        visibleLine -= editor_->send(SCI_LINESONSCREEN) / 2;
        editor_->send(SCI_SETFIRSTVISIBLELINE, std::max<sptr_t>(0, visibleLine));
    }

    ScintillaEditBase *editor_;
    EditorTheme theme_;
    QTimer rebuildTimer_;
    QPixmap cache_;
    int overviewHeight_ = 0;
    bool dirty_ = true;
};

EditorView::EditorView(QWidget *parent)
    : QWidget(parent), editor_(new ScintillaEditBase(this)), minimap_(new Minimap(editor_, this))
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(editor_, 1);
    layout->addWidget(minimap_);
    setFocusProxy(editor_);

    editor_->send(SCI_SETCODEPAGE, SC_CP_UTF8);
    editor_->send(SCI_SETEOLMODE, SC_EOL_LF);
    editor_->send(SCI_SETUSETABS, false);
    editor_->send(SCI_SETTABWIDTH, 4);
    editor_->send(SCI_SETMULTIPLESELECTION, true);
    editor_->send(SCI_SETADDITIONALSELECTIONTYPING, true);
    editor_->send(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);
    editor_->send(SCI_SETSCROLLWIDTHTRACKING, true);
    editor_->send(SCI_SETSCROLLWIDTH, 1);
    editor_->send(SCI_SETMARGINTYPEN, lineNumberMargin, SC_MARGIN_NUMBER);
    editor_->send(SCI_SETMARGINWIDTHN, 1, 0);
    editor_->send(SCI_SETMARGINTYPEN, foldMargin, SC_MARGIN_SYMBOL);
    editor_->send(SCI_SETMARGINMASKN, foldMargin, SC_MASK_FOLDERS);
    editor_->send(SCI_SETMARGINSENSITIVEN, foldMargin, true);
    editor_->send(SCI_SETAUTOMATICFOLD, SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE);
    editor_->send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_ARROWDOWN);
    editor_->send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_ARROW);
    editor_->send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_EMPTY);
    editor_->send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_EMPTY);
    editor_->send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_ARROW);
    editor_->send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_ARROWDOWN);
    editor_->send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_EMPTY);

    connect(editor_, &ScintillaEditBase::savePointChanged, this, &EditorView::modificationChanged);
    connect(editor_, &ScintillaEditBase::modified, this, [this](Scintilla::ModificationFlags flags) {
        if (static_cast<int>(flags) & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))
            emit textChanged();
    });
    connect(editor_, &ScintillaEditBase::linesAdded, this, [this] { updateLineNumberWidth(); });
    connect(editor_, &ScintillaEditBase::zoom, this, [this] { updateLineNumberWidth(); });
    connect(editor_, &ScintillaEditBase::updateUi, this, [this](Scintilla::Update flags) {
        if (static_cast<int>(flags) & (SC_UPDATE_SELECTION | SC_UPDATE_CONTENT))
            emit cursorChanged();
    });
    connect(editor_, &ScintillaEditBase::focusChanged, this, [this](bool focused) {
        minimap_->update();
        if (focused)
            emit editorFocused();
    });

    setDarkTheme(true);
}

QByteArray EditorView::textUtf8() const
{
    const auto length = editor_->send(SCI_GETLENGTH);
    if (length == 0)
        return {};
    return QByteArray(reinterpret_cast<const char *>(editor_->send(SCI_GETCHARACTERPOINTER)), length);
}

void EditorView::setTextUtf8(const QByteArray &text)
{
    int mode = SC_EOL_LF;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == '\n')
            break;
        if (text[i] == '\r') {
            mode = i + 1 < text.size() && text[i + 1] == '\n' ? SC_EOL_CRLF : SC_EOL_CR;
            break;
        }
    }
    editor_->send(SCI_SETEOLMODE, mode);

    editor_->send(SCI_SETUNDOCOLLECTION, false);
    editor_->send(SCI_CLEARALL);
    editor_->sends(SCI_ADDTEXT, text.size(), text.constData());
    editor_->send(SCI_SETUNDOCOLLECTION, true);
    editor_->send(SCI_EMPTYUNDOBUFFER);
    editor_->send(SCI_GOTOPOS, 0);
    setSavePoint();
    setLanguageForPath(path_);
}

void EditorView::setLanguageForPath(const QString &path)
{
    path_ = path;
    const QFileInfo file(path);
    const auto suffix = file.suffix().toLower();
    const auto name = file.fileName().toLower();
    const char *language = nullptr;
    int languageId = SCLEX_CONTAINER;
    if (QStringList{QStringLiteral("c"), QStringLiteral("h"), QStringLiteral("cc"), QStringLiteral("cpp"),
                    QStringLiteral("cxx"), QStringLiteral("hpp"), QStringLiteral("hh"), QStringLiteral("hxx"),
                    QStringLiteral("ipp"), QStringLiteral("inl"), QStringLiteral("m"), QStringLiteral("mm")}.contains(suffix)) {
        language = "cpp";
        languageId = SCLEX_CPP;
    } else if (suffix == QStringLiteral("py") || suffix == QStringLiteral("pyw")) {
        language = "python";
        languageId = SCLEX_PYTHON;
    } else if (suffix == QStringLiteral("json") || suffix == QStringLiteral("jsonc")) {
        language = "json";
        languageId = SCLEX_JSON;
    } else if (suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown")) {
        language = "markdown";
        languageId = SCLEX_MARKDOWN;
    } else if (suffix == QStringLiteral("sh") || suffix == QStringLiteral("bash") || suffix == QStringLiteral("zsh")) {
        language = "bash";
        languageId = SCLEX_BASH;
    } else if (suffix == QStringLiteral("cmake") || name == QStringLiteral("cmakelists.txt")) {
        language = "cmake";
        languageId = SCLEX_CMAKE;
    }

    if (editor_->send(SCI_GETLENGTH) > syntaxLimit) {
        language = nullptr;
        languageId = SCLEX_CONTAINER;
    }
    if (editor_->send(SCI_GETLEXER) == languageId) {
        setTheme(theme_);
        return;
    }

    auto *lexer = language ? CreateLexer(language) : nullptr;
    editor_->send(SCI_CLEARDOCUMENTSTYLE);
    editor_->send(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(lexer));
    if (lexer) {
        editor_->send(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold"), reinterpret_cast<sptr_t>("1"));
        const auto lexerId = editor_->send(SCI_GETLEXER);
        if (lexerId == SCLEX_CPP) {
            editor_->sends(SCI_SETKEYWORDS, 0, cppKeywords);
            editor_->send(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold.compact"), reinterpret_cast<sptr_t>("0"));
        } else if (lexerId == SCLEX_PYTHON) {
            editor_->sends(SCI_SETKEYWORDS, 0,
                           "False None True and as assert async await break case class continue def del elif else "
                           "except finally for from global if import in is lambda match nonlocal not or pass raise "
                           "return try while with yield");
        } else if (lexerId == SCLEX_JSON) {
            editor_->sends(SCI_SETKEYWORDS, 0, "false null true");
        } else if (lexerId == SCLEX_BASH) {
            editor_->sends(SCI_SETKEYWORDS, 0,
                           "if then elif else fi time for while until do done in case esac coproc select function "
                           "break continue return exit export readonly local declare typeset source unset set");
        } else if (lexerId == SCLEX_CMAKE) {
            editor_->sends(SCI_SETKEYWORDS, 0,
                           "add_compile_definitions add_compile_options add_custom_command add_custom_target "
                           "add_definitions add_executable add_library add_subdirectory add_test cmake_minimum_required "
                           "configure_file enable_testing execute_process file find_library find_package find_path "
                           "find_program get_property include install list message option project set set_property "
                           "string target_compile_definitions target_compile_features target_compile_options "
                           "target_include_directories target_link_libraries target_sources unset");
        }
    }
    setTheme(theme_);
}

void EditorView::shareDocumentFrom(EditorView &source)
{
    const auto document = source.editor_->send(SCI_GETDOCPOINTER);
    if (editor_->send(SCI_GETDOCPOINTER) == document)
        return;
    const bool wasModified = isModified();
    editor_->send(SCI_SETDOCPOINTER, 0, document);
    path_ = source.path_;
    setTheme(theme_);
    if (isModified() != wasModified)
        emit modificationChanged(isModified());
}

void EditorView::setDarkTheme(bool dark)
{
    setTheme(builtInTheme(dark));
}

void EditorView::setTheme(const EditorTheme &theme)
{
    theme_ = theme;
    editor_->send(SCI_STYLESETFORE, STYLE_DEFAULT, rgb(theme.foreground));
    editor_->send(SCI_STYLESETBACK, STYLE_DEFAULT, rgb(theme.background));
    editor_->sends(SCI_STYLESETFONT, STYLE_DEFAULT, theme.font.family().toUtf8().constData());
    editor_->send(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, qRound(theme.font.pointSizeF() * SC_FONT_SIZE_MULTIPLIER));
    editor_->send(SCI_STYLECLEARALL);

    setForegrounds(editor_, theme.muted, {STYLE_LINENUMBER, STYLE_FOLDDISPLAYTEXT});
    editor_->send(SCI_STYLESETBACK, STYLE_LINENUMBER, rgb(theme.background));
    editor_->send(SCI_SETFOLDMARGINCOLOUR, true, rgb(theme.background));
    editor_->send(SCI_SETFOLDMARGINHICOLOUR, true, rgb(theme.background));
    for (int marker = SC_MARKNUM_FOLDEREND; marker <= SC_MARKNUM_FOLDEROPEN; ++marker) {
        editor_->send(SCI_MARKERSETFORE, marker, rgb(theme.background));
        editor_->send(SCI_MARKERSETBACK, marker, rgb(theme.muted));
    }
    editor_->send(SCI_SETCARETFORE, rgb(theme.foreground));
    editor_->send(SCI_SETADDITIONALCARETFORE, rgb(theme.keyword));
    editor_->send(SCI_SETCARETLINEVISIBLE, true);
    editor_->send(SCI_SETCARETLINEBACK, rgb(theme.currentLine));
    editor_->send(SCI_SETSELBACK, true, rgb(theme.selection));
    editor_->send(SCI_SETADDITIONALSELBACK, rgb(theme.selection));

    const auto lexerId = editor_->send(SCI_GETLEXER);
    const bool supportsFolding = lexerId > SCLEX_NULL && lexerId != SCLEX_MARKDOWN;
    editor_->send(SCI_SETMARGINWIDTHN, foldMargin, supportsFolding ? 14 : 0);
    const bool styleOverview = lexerId > SCLEX_NULL && !minimap_->isHidden();
    editor_->send(SCI_SETIDLESTYLING, styleOverview ? SC_IDLESTYLING_ALL : SC_IDLESTYLING_NONE);
    switch (lexerId) {
    case SCLEX_CPP:
        setForegrounds(editor_, theme.keyword, {SCE_C_WORD, SCE_C_WORD2});
        setForegrounds(editor_, theme.string,
                       {SCE_C_STRING, SCE_C_CHARACTER, SCE_C_VERBATIM, SCE_C_STRINGRAW, SCE_C_ESCAPESEQUENCE});
        setForegrounds(editor_, theme.comment,
                       {SCE_C_COMMENT, SCE_C_COMMENTLINE, SCE_C_COMMENTDOC, SCE_C_COMMENTLINEDOC,
                        SCE_C_COMMENTDOCKEYWORD, SCE_C_PREPROCESSORCOMMENT, SCE_C_PREPROCESSORCOMMENTDOC});
        setForegrounds(editor_, theme.number, {SCE_C_NUMBER});
        setForegrounds(editor_, theme.preprocessor, {SCE_C_PREPROCESSOR, SCE_C_UUID});
        break;
    case SCLEX_PYTHON:
        setForegrounds(editor_, theme.keyword, {SCE_P_WORD, SCE_P_WORD2});
        setForegrounds(editor_, theme.string,
                       {SCE_P_STRING, SCE_P_CHARACTER, SCE_P_TRIPLE, SCE_P_TRIPLEDOUBLE, SCE_P_FSTRING,
                        SCE_P_FCHARACTER, SCE_P_FTRIPLE, SCE_P_FTRIPLEDOUBLE});
        setForegrounds(editor_, theme.comment, {SCE_P_COMMENTLINE, SCE_P_COMMENTBLOCK});
        setForegrounds(editor_, theme.number, {SCE_P_NUMBER});
        setForegrounds(editor_, theme.preprocessor, {SCE_P_CLASSNAME, SCE_P_DEFNAME, SCE_P_DECORATOR});
        break;
    case SCLEX_JSON:
        setForegrounds(editor_, theme.keyword, {SCE_JSON_KEYWORD, SCE_JSON_LDKEYWORD});
        setForegrounds(editor_, theme.string, {SCE_JSON_STRING, SCE_JSON_ESCAPESEQUENCE});
        setForegrounds(editor_, theme.comment, {SCE_JSON_LINECOMMENT, SCE_JSON_BLOCKCOMMENT});
        setForegrounds(editor_, theme.number, {SCE_JSON_NUMBER});
        setForegrounds(editor_, theme.preprocessor, {SCE_JSON_PROPERTYNAME});
        break;
    case SCLEX_MARKDOWN:
        setForegrounds(editor_, theme.keyword,
                       {SCE_MARKDOWN_HEADER1, SCE_MARKDOWN_HEADER2, SCE_MARKDOWN_HEADER3,
                        SCE_MARKDOWN_HEADER4, SCE_MARKDOWN_HEADER5, SCE_MARKDOWN_HEADER6,
                        SCE_MARKDOWN_STRONG1, SCE_MARKDOWN_STRONG2, SCE_MARKDOWN_EM1, SCE_MARKDOWN_EM2});
        setForegrounds(editor_, theme.string, {SCE_MARKDOWN_CODE, SCE_MARKDOWN_CODE2, SCE_MARKDOWN_CODEBK});
        setForegrounds(editor_, theme.preprocessor,
                       {SCE_MARKDOWN_LINK, SCE_MARKDOWN_OLIST_ITEM, SCE_MARKDOWN_ULIST_ITEM,
                        SCE_MARKDOWN_FRONT_MARK, SCE_MARKDOWN_FRONT_KEY});
        setForegrounds(editor_, theme.comment, {SCE_MARKDOWN_BLOCKQUOTE, SCE_MARKDOWN_HRULE});
        break;
    case SCLEX_BASH:
        setForegrounds(editor_, theme.keyword, {SCE_SH_WORD});
        setForegrounds(editor_, theme.string, {SCE_SH_STRING, SCE_SH_CHARACTER, SCE_SH_HERE_Q});
        setForegrounds(editor_, theme.comment, {SCE_SH_COMMENTLINE});
        setForegrounds(editor_, theme.number, {SCE_SH_NUMBER});
        setForegrounds(editor_, theme.preprocessor, {SCE_SH_SCALAR, SCE_SH_PARAM, SCE_SH_BACKTICKS});
        break;
    case SCLEX_CMAKE:
        setForegrounds(editor_, theme.keyword,
                       {SCE_CMAKE_COMMANDS, SCE_CMAKE_WHILEDEF, SCE_CMAKE_FOREACHDEF,
                        SCE_CMAKE_IFDEFINEDEF, SCE_CMAKE_MACRODEF});
        setForegrounds(editor_, theme.string, {SCE_CMAKE_STRINGDQ, SCE_CMAKE_STRINGLQ, SCE_CMAKE_STRINGRQ});
        setForegrounds(editor_, theme.comment, {SCE_CMAKE_COMMENT});
        setForegrounds(editor_, theme.number, {SCE_CMAKE_NUMBER});
        setForegrounds(editor_, theme.preprocessor, {SCE_CMAKE_VARIABLE, SCE_CMAKE_STRINGVAR});
        break;
    default:
        break;
    }

    updateLineNumberWidth();
    minimap_->setTheme(theme_);
}

void EditorView::setMinimapVisible(bool visible)
{
    minimap_->setVisible(visible);
    editor_->send(SCI_SETIDLESTYLING, visible && editor_->send(SCI_GETLEXER) > SCLEX_NULL
                  ? SC_IDLESTYLING_ALL : SC_IDLESTYLING_NONE);
    if (visible)
        minimap_->invalidate();
}

void EditorView::addNextOccurrence()
{
    editor_->send(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE);
    editor_->send(SCI_TARGETWHOLEDOCUMENT);
    editor_->send(SCI_MULTIPLESELECTADDNEXT);
}

void EditorView::toggleFold()
{
    auto line = cursorLine();
    if ((editor_->send(SCI_GETFOLDLEVEL, line) & SC_FOLDLEVELHEADERFLAG) == 0)
        line = editor_->send(SCI_GETFOLDPARENT, line);
    if (line >= 0)
        editor_->send(SCI_TOGGLEFOLD, line);
}

bool EditorView::findNext(const QString &text, bool backwards, bool caseSensitive)
{
    if (text.isEmpty())
        return false;
    const QByteArray needle = text.toUtf8();
    const auto first = backwards ? editor_->send(SCI_GETLENGTH) : 0;
    const auto last = backwards ? 0 : editor_->send(SCI_GETLENGTH);
    const auto from = editor_->send(backwards ? SCI_GETSELECTIONSTART : SCI_GETSELECTIONEND);
    editor_->send(SCI_SETSEARCHFLAGS, caseSensitive ? SCFIND_MATCHCASE : 0);
    editor_->send(SCI_SETTARGETRANGE, from, last);
    if (editor_->sends(SCI_SEARCHINTARGET, needle.size(), needle.constData()) < 0) {
        editor_->send(SCI_SETTARGETRANGE, first, last);
        if (editor_->sends(SCI_SEARCHINTARGET, needle.size(), needle.constData()) < 0)
            return false;
    }
    const auto start = editor_->send(SCI_GETTARGETSTART);
    const auto end = editor_->send(SCI_GETTARGETEND);
    editor_->send(SCI_ENSUREVISIBLE, editor_->send(SCI_LINEFROMPOSITION, start));
    editor_->send(SCI_ENSUREVISIBLE, editor_->send(SCI_LINEFROMPOSITION, end));
    editor_->send(SCI_SETSEL, start, end);
    return true;
}

bool EditorView::replaceCurrent(const QString &text, const QString &replacement, bool caseSensitive)
{
    if (text.isEmpty() || editor_->send(SCI_GETREADONLY))
        return false;
    const QByteArray needle = text.toUtf8();
    const auto start = editor_->send(SCI_GETSELECTIONSTART);
    const auto end = editor_->send(SCI_GETSELECTIONEND);
    editor_->send(SCI_SETSEARCHFLAGS, caseSensitive ? SCFIND_MATCHCASE : 0);
    editor_->send(SCI_SETTARGETRANGE, start, end);
    if (editor_->sends(SCI_SEARCHINTARGET, needle.size(), needle.constData()) != start
        || editor_->send(SCI_GETTARGETEND) != end) {
        return false;
    }
    const QByteArray bytes = replacement.toUtf8();
    editor_->sends(SCI_REPLACETARGET, bytes.size(), bytes.constData());
    editor_->send(SCI_SETSEL, start, start + bytes.size());
    return true;
}

qsizetype EditorView::replaceAll(const QString &text, const QString &replacement, bool caseSensitive)
{
    if (text.isEmpty() || editor_->send(SCI_GETREADONLY))
        return 0;
    const QByteArray needle = text.toUtf8();
    const QByteArray bytes = replacement.toUtf8();
    qsizetype replaced = 0;
    editor_->send(SCI_SETSEARCHFLAGS, caseSensitive ? SCFIND_MATCHCASE : 0);
    editor_->send(SCI_TARGETWHOLEDOCUMENT);
    editor_->send(SCI_BEGINUNDOACTION);
    while (editor_->sends(SCI_SEARCHINTARGET, needle.size(), needle.constData()) >= 0) {
        editor_->sends(SCI_REPLACETARGET, bytes.size(), bytes.constData());
        ++replaced;
        editor_->send(SCI_SETTARGETRANGE, editor_->send(SCI_GETTARGETEND), editor_->send(SCI_GETLENGTH));
    }
    editor_->send(SCI_ENDUNDOACTION);
    return replaced;
}

void EditorView::replaceRange(qsizetype startByte, qsizetype endByte, const QByteArray &text)
{
    const auto length = editor_->send(SCI_GETLENGTH);
    if (startByte < 0 || startByte > endByte || endByte > length || editor_->send(SCI_GETREADONLY))
        return;
    editor_->send(SCI_SETTARGETRANGE, startByte, endByte);
    editor_->sends(SCI_REPLACETARGET, text.size(), text.constData());
    setCursorPosition(startByte + text.size());
}

qsizetype EditorView::cursorPosition() const
{
    return editor_->send(SCI_GETCURRENTPOS);
}

qsizetype EditorView::cursorLine() const
{
    return editor_->send(SCI_LINEFROMPOSITION, cursorPosition());
}

qsizetype EditorView::cursorColumn() const
{
    return editor_->send(SCI_GETCOLUMN, cursorPosition());
}

void EditorView::setCursorPosition(qsizetype position)
{
    position = std::clamp<qsizetype>(position, 0, editor_->send(SCI_GETLENGTH));
    editor_->send(SCI_ENSUREVISIBLE, editor_->send(SCI_LINEFROMPOSITION, position));
    editor_->send(SCI_GOTOPOS, position);
}

void EditorView::goToLineColumn(qsizetype line, qsizetype column)
{
    line = std::clamp<qsizetype>(line, 0, editor_->send(SCI_GETLINECOUNT) - 1);
    setCursorPosition(editor_->send(SCI_FINDCOLUMN, line, std::max<qsizetype>(0, column)));
}

void EditorView::undo()
{
    editor_->send(SCI_UNDO);
}

void EditorView::redo()
{
    editor_->send(SCI_REDO);
}

void EditorView::cut()
{
    editor_->send(SCI_CUT);
}

void EditorView::copy()
{
    editor_->send(SCI_COPY);
}

void EditorView::paste()
{
    editor_->send(SCI_PASTE);
}

void EditorView::selectAll()
{
    editor_->send(SCI_SELECTALL);
}

void EditorView::setSavePoint()
{
    editor_->send(SCI_SETSAVEPOINT);
}

bool EditorView::isModified() const
{
    return editor_->send(SCI_GETMODIFY);
}

void EditorView::updateLineNumberWidth()
{
    const auto digits = QByteArray::number(static_cast<qlonglong>(editor_->send(SCI_GETLINECOUNT))).size();
    const QByteArray sample(std::max<qsizetype>(digits, 2), '9');
    const auto width = editor_->sends(SCI_TEXTWIDTH, STYLE_LINENUMBER, sample.constData()) + 14;
    if (editor_->send(SCI_GETMARGINWIDTHN, lineNumberMargin) != width)
        editor_->send(SCI_SETMARGINWIDTHN, lineNumberMargin, width);
}

} // namespace nebula
