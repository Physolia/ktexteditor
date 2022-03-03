/*
    This file is part of the KDE libraries
    SPDX-FileCopyrightText: 2001-2004 Christoph Cullmann <cullmann@kde.org>
    SPDX-FileCopyrightText: 2001 Joseph Wenninger <jowenn@kde.org>
    SPDX-FileCopyrightText: 1999 Jochen Wilhelmy <digisnap@cs.tu-berlin.de>
    SPDX-FileCopyrightText: 2006 Hamish Rodda <rodda@kde.org>
    SPDX-FileCopyrightText: 2007 Mirko Stocker <me@misto.ch>
    SPDX-FileCopyrightText: 2009-2010 Michel Ludwig <michel.ludwig@kdemail.net>
    SPDX-FileCopyrightText: 2013 Gerald Senarclens de Grancy <oss@senarclens.eu>
    SPDX-FileCopyrightText: 2013 Andrey Matveyakin <a.matveyakin@gmail.com>

    SPDX-License-Identifier: LGPL-2.0-only
*/
// BEGIN includes
#include "katedocument.h"
#include "config.h"
#include "kateabstractinputmode.h"
#include "kateautoindent.h"
#include "katebuffer.h"
#include "kateconfig.h"
#include "katedialogs.h"
#include "kateglobal.h"
#include "katehighlight.h"
#include "kateindentdetecter.h"
#include "katemodemanager.h"
#include "katepartdebug.h"
#include "kateplaintextsearch.h"
#include "kateregexpsearch.h"
#include "katerenderer.h"
#include "katescriptmanager.h"
#include "kateswapfile.h"
#include "katesyntaxmanager.h"
#include "katetemplatehandler.h"
#include "kateundomanager.h"
#include "katevariableexpansionmanager.h"
#include "kateview.h"
#include "printing/kateprinter.h"
#include "spellcheck/ontheflycheck.h"
#include "spellcheck/prefixstore.h"
#include "spellcheck/spellcheck.h"

#if EDITORCONFIG_FOUND
#include "editorconfig.h"
#endif

#include <KTextEditor/Attribute>
#include <KTextEditor/DocumentCursor>

#include <KConfigGroup>
#include <KDirWatch>
#include <KFileItem>
#include <KIO/Job>
#include <KIO/JobUiDelegate>
#include <KJobWidgets>
#include <KMessageBox>
#include <KMountPoint>
#include <KNetworkMounts>
#include <KParts/OpenUrlArguments>
#include <KStandardAction>
#include <KStringHandler>
#include <KToggleAction>
#include <KXMLGUIFactory>

#include <kparts_version.h>

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QFile>
#include <QFileDialog>
#include <QMap>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTextCodec>
#include <QTextStream>

#include <cmath>

// END  includes

#if 0
#define EDIT_DEBUG qCDebug(LOG_KTE)
#else
#define EDIT_DEBUG                                                                                                                                             \
    if (0)                                                                                                                                                     \
    qCDebug(LOG_KTE)
#endif

template<class C, class E>
static int indexOf(const std::initializer_list<C> &list, const E &entry)
{
    auto it = std::find(list.begin(), list.end(), entry);
    return it == list.end() ? -1 : std::distance(list.begin(), it);
}

template<class C, class E>
static bool contains(const std::initializer_list<C> &list, const E &entry)
{
    return indexOf(list, entry) >= 0;
}

static inline QChar matchingStartBracket(const QChar c)
{
    switch (c.toLatin1()) {
    case '}':
        return QLatin1Char('{');
    case ']':
        return QLatin1Char('[');
    case ')':
        return QLatin1Char('(');
    }
    return QChar();
}

static inline QChar matchingEndBracket(const QChar c, bool withQuotes = true)
{
    switch (c.toLatin1()) {
    case '{':
        return QLatin1Char('}');
    case '[':
        return QLatin1Char(']');
    case '(':
        return QLatin1Char(')');
    case '\'':
        return withQuotes ? QLatin1Char('\'') : QChar();
    case '"':
        return withQuotes ? QLatin1Char('"') : QChar();
    }
    return QChar();
}

static inline QChar matchingBracket(const QChar c)
{
    QChar bracket = matchingStartBracket(c);
    if (bracket.isNull()) {
        bracket = matchingEndBracket(c, /*withQuotes=*/false);
    }
    return bracket;
}

static inline bool isStartBracket(const QChar c)
{
    return !matchingEndBracket(c, /*withQuotes=*/false).isNull();
}

static inline bool isEndBracket(const QChar c)
{
    return !matchingStartBracket(c).isNull();
}

static inline bool isBracket(const QChar c)
{
    return isStartBracket(c) || isEndBracket(c);
}

/**
 * normalize given url
 * @param url input url
 * @return normalized url
 */
static QUrl normalizeUrl(const QUrl &url)
{
    // only normalize local urls

    if (url.isEmpty() || !url.isLocalFile()
        || KNetworkMounts::self()->isOptionEnabledForPath(url.toLocalFile(), KNetworkMounts::StrongSideEffectsOptimizations)) {
        return url;
    }
    // don't normalize if not existing!
    // canonicalFilePath won't work!
    const QString normalizedUrl(QFileInfo(url.toLocalFile()).canonicalFilePath());
    if (normalizedUrl.isEmpty()) {
        return url;
    }

    // else: use canonicalFilePath to normalize
    return QUrl::fromLocalFile(normalizedUrl);
}

// BEGIN d'tor, c'tor
//
// KTextEditor::DocumentPrivate Constructor
//
KTextEditor::DocumentPrivate::DocumentPrivate(bool bSingleViewMode, bool bReadOnly, QWidget *parentWidget, QObject *parent)
    : KTextEditor::Document(this, parent)
    , m_bSingleViewMode(bSingleViewMode)
    , m_bReadOnly(bReadOnly)
    ,

    m_undoManager(new KateUndoManager(this))
    ,

    m_buffer(new KateBuffer(this))
    , m_indenter(new KateAutoIndent(this))
    ,

    m_docName(QStringLiteral("need init"))
    ,

    m_fileType(QStringLiteral("Normal"))
    ,

    m_config(new KateDocumentConfig(this))

{
    // setup component name
    const auto &aboutData = EditorPrivate::self()->aboutData();
    setComponentName(aboutData.componentName(), aboutData.displayName());

    // avoid spamming plasma and other window managers with progress dialogs
    // we show such stuff inline in the views!
    setProgressInfoEnabled(false);

    // register doc at factory
    KTextEditor::EditorPrivate::self()->registerDocument(this);

    // normal hl
    m_buffer->setHighlight(0);

    // swap file
    m_swapfile = (config()->swapFileMode() == KateDocumentConfig::DisableSwapFile) ? nullptr : new Kate::SwapFile(this);

    // some nice signals from the buffer
    connect(m_buffer, &KateBuffer::tagLines, this, &KTextEditor::DocumentPrivate::tagLines);

    // if the user changes the highlight with the dialog, notify the doc
    connect(KateHlManager::self(), &KateHlManager::changed, this, &KTextEditor::DocumentPrivate::internalHlChanged);

    // signals for mod on hd
    connect(KTextEditor::EditorPrivate::self()->dirWatch(), &KDirWatch::dirty, this, &KTextEditor::DocumentPrivate::slotModOnHdDirty);

    connect(KTextEditor::EditorPrivate::self()->dirWatch(), &KDirWatch::created, this, &KTextEditor::DocumentPrivate::slotModOnHdCreated);

    connect(KTextEditor::EditorPrivate::self()->dirWatch(), &KDirWatch::deleted, this, &KTextEditor::DocumentPrivate::slotModOnHdDeleted);

    // singleshot timer to handle updates of mod on hd state delayed
    m_modOnHdTimer.setSingleShot(true);
    m_modOnHdTimer.setInterval(200);
    connect(&m_modOnHdTimer, &QTimer::timeout, this, &KTextEditor::DocumentPrivate::slotDelayedHandleModOnHd);

    // Setup auto reload stuff
    m_autoReloadMode = new KToggleAction(i18n("Auto Reload Document"), this);
    m_autoReloadMode->setWhatsThis(i18n("Automatic reload the document when it was changed on disk"));
    connect(m_autoReloadMode, &KToggleAction::triggered, this, &DocumentPrivate::autoReloadToggled);
    // Prepare some reload amok protector...
    m_autoReloadThrottle.setSingleShot(true);
    //...but keep the value small in unit tests
    m_autoReloadThrottle.setInterval(KTextEditor::EditorPrivate::self()->unitTestMode() ? 50 : 3000);
    connect(&m_autoReloadThrottle, &QTimer::timeout, this, &DocumentPrivate::onModOnHdAutoReload);

    // load handling
    // this is needed to ensure we signal the user if a file is still loading
    // and to disallow him to edit in that time
    connect(this, &KTextEditor::DocumentPrivate::started, this, &KTextEditor::DocumentPrivate::slotStarted);
    connect(this, qOverload<>(&KTextEditor::DocumentPrivate::completed), this, &KTextEditor::DocumentPrivate::slotCompleted);
    connect(this, &KTextEditor::DocumentPrivate::canceled, this, &KTextEditor::DocumentPrivate::slotCanceled);

    // handle doc name updates
    connect(this, &KParts::ReadOnlyPart::urlChanged, this, &KTextEditor::DocumentPrivate::slotUrlChanged);
    updateDocName();

    // if single view mode, like in the konqui embedding, create a default view ;)
    // be lazy, only create it now, if any parentWidget is given, otherwise widget()
    // will create it on demand...
    if (m_bSingleViewMode && parentWidget) {
        KTextEditor::View *view = (KTextEditor::View *)createView(parentWidget);
        insertChildClient(view);
        view->setContextMenu(view->defaultContextMenu());
        setWidget(view);
    }

    connect(m_undoManager, &KateUndoManager::undoChanged, this, &KTextEditor::DocumentPrivate::undoChanged);
    connect(m_undoManager, &KateUndoManager::undoStart, this, &KTextEditor::DocumentPrivate::editingStarted);
    connect(m_undoManager, &KateUndoManager::undoEnd, this, &KTextEditor::DocumentPrivate::editingFinished);
    connect(m_undoManager, &KateUndoManager::redoStart, this, &KTextEditor::DocumentPrivate::editingStarted);
    connect(m_undoManager, &KateUndoManager::redoEnd, this, &KTextEditor::DocumentPrivate::editingFinished);

    connect(this, &KTextEditor::DocumentPrivate::sigQueryClose, this, &KTextEditor::DocumentPrivate::slotQueryClose_save);

    connect(this, &KTextEditor::DocumentPrivate::aboutToInvalidateMovingInterfaceContent, this, &KTextEditor::DocumentPrivate::clearEditingPosStack);
    onTheFlySpellCheckingEnabled(config()->onTheFlySpellCheck());

    // make sure correct defaults are set (indenter, ...)
    updateConfig();
}

//
// KTextEditor::DocumentPrivate Destructor
//
KTextEditor::DocumentPrivate::~DocumentPrivate()
{
    // we need to disconnect this as it triggers in destructor of KParts::ReadOnlyPart but we have already deleted
    // important stuff then
    disconnect(this, &KParts::ReadOnlyPart::urlChanged, this, &KTextEditor::DocumentPrivate::slotUrlChanged);

    // delete pending mod-on-hd message, if applicable
    delete m_modOnHdHandler;

    // we are about to delete cursors/ranges/...
    Q_EMIT aboutToDeleteMovingInterfaceContent(this);

    // kill it early, it has ranges!
    delete m_onTheFlyChecker;
    m_onTheFlyChecker = nullptr;

    clearDictionaryRanges();

    // Tell the world that we're about to close (== destruct)
    // Apps must receive this in a direct signal-slot connection, and prevent
    // any further use of interfaces once they return.
    Q_EMIT aboutToClose(this);

    // remove file from dirwatch
    deactivateDirWatch();

    // thanks for offering, KPart, but we're already self-destructing
    setAutoDeleteWidget(false);
    setAutoDeletePart(false);

    // clean up remaining views
    qDeleteAll(m_views.keys());
    m_views.clear();

    // clean up marks
    for (auto &mark : std::as_const(m_marks)) {
        delete mark;
    }
    m_marks.clear();

    // de-register document early from global collections
    // otherwise we might "use" them again during destruction in a half-valid state
    // see e.g. bug 422546 for similar issues with view
    // this is still early enough, as as long as m_config is valid, this document is still "OK"
    KTextEditor::EditorPrivate::self()->deregisterDocument(this);
}
// END

void KTextEditor::DocumentPrivate::saveEditingPositions(const KTextEditor::Cursor cursor)
{
    if (m_editingStackPosition != m_editingStack.size() - 1) {
        m_editingStack.resize(m_editingStackPosition);
    }

    // try to be clever: reuse existing cursors if possible
    QSharedPointer<KTextEditor::MovingCursor> mc;

    // we might pop last one: reuse that
    if (!m_editingStack.isEmpty() && cursor.line() == m_editingStack.top()->line()) {
        mc = m_editingStack.pop();
    }

    // we might expire oldest one, reuse that one, if not already one there
    // we prefer the other one for reuse, as already on the right line aka in the right block!
    const int editingStackSizeLimit = 32;
    if (m_editingStack.size() >= editingStackSizeLimit) {
        if (mc) {
            m_editingStack.removeFirst();
        } else {
            mc = m_editingStack.takeFirst();
        }
    }

    // new cursor needed? or adjust existing one?
    if (mc) {
        mc->setPosition(cursor);
    } else {
        mc = QSharedPointer<KTextEditor::MovingCursor>(newMovingCursor(cursor));
    }

    // add new one as top of stack
    m_editingStack.push(mc);
    m_editingStackPosition = m_editingStack.size() - 1;
}

KTextEditor::Cursor KTextEditor::DocumentPrivate::lastEditingPosition(EditingPositionKind nextOrPrev, KTextEditor::Cursor currentCursor)
{
    if (m_editingStack.isEmpty()) {
        return KTextEditor::Cursor::invalid();
    }
    auto targetPos = m_editingStack.at(m_editingStackPosition)->toCursor();
    if (targetPos == currentCursor) {
        if (nextOrPrev == Previous) {
            m_editingStackPosition--;
        } else {
            m_editingStackPosition++;
        }
        m_editingStackPosition = qBound(0, m_editingStackPosition, m_editingStack.size() - 1);
    }
    return m_editingStack.at(m_editingStackPosition)->toCursor();
}

void KTextEditor::DocumentPrivate::clearEditingPosStack()
{
    m_editingStack.clear();
    m_editingStackPosition = -1;
}

// on-demand view creation
QWidget *KTextEditor::DocumentPrivate::widget()
{
    // no singleViewMode -> no widget()...
    if (!singleViewMode()) {
        return nullptr;
    }

    // does a widget exist already? use it!
    if (KTextEditor::Document::widget()) {
        return KTextEditor::Document::widget();
    }

    // create and return one...
    KTextEditor::View *view = (KTextEditor::View *)createView(nullptr);
    insertChildClient(view);
    view->setContextMenu(view->defaultContextMenu());
    setWidget(view);
    return view;
}

// BEGIN KTextEditor::Document stuff

KTextEditor::View *KTextEditor::DocumentPrivate::createView(QWidget *parent, KTextEditor::MainWindow *mainWindow)
{
    KTextEditor::ViewPrivate *newView = new KTextEditor::ViewPrivate(this, parent, mainWindow);

    if (m_fileChangedDialogsActivated) {
        connect(newView, &KTextEditor::ViewPrivate::focusIn, this, &KTextEditor::DocumentPrivate::slotModifiedOnDisk);
    }

    Q_EMIT viewCreated(this, newView);

    // post existing messages to the new view, if no specific view is given
    const auto keys = m_messageHash.keys();
    for (KTextEditor::Message *message : keys) {
        if (!message->view()) {
            newView->postMessage(message, m_messageHash[message]);
        }
    }

    return newView;
}

KTextEditor::Range KTextEditor::DocumentPrivate::rangeOnLine(KTextEditor::Range range, int line) const
{
    const int col1 = toVirtualColumn(range.start());
    const int col2 = toVirtualColumn(range.end());
    return KTextEditor::Range(line, fromVirtualColumn(line, col1), line, fromVirtualColumn(line, col2));
}

void KTextEditor::DocumentPrivate::setMetaData(const KPluginMetaData &metaData)
{
    KParts::Part::setMetaData(metaData);
}

// BEGIN KTextEditor::EditInterface stuff

bool KTextEditor::DocumentPrivate::isEditingTransactionRunning() const
{
    return editSessionNumber > 0;
}

QString KTextEditor::DocumentPrivate::text() const
{
    return m_buffer->text();
}

QString KTextEditor::DocumentPrivate::text(const KTextEditor::Range &range, bool blockwise) const
{
    if (!range.isValid()) {
        qCWarning(LOG_KTE) << "Text requested for invalid range" << range;
        return QString();
    }

    QString s;

    if (range.start().line() == range.end().line()) {
        if (range.start().column() > range.end().column()) {
            return QString();
        }

        const QString lineText = m_buffer->lineText(range.start().line());

        if (lineText.isEmpty()) {
            return QString();
        }

        return lineText.mid(range.start().column(), range.end().column() - range.start().column());
    } else {
        for (int i = range.start().line(); (i <= range.end().line()) && (i < m_buffer->count()); ++i) {
            const QString lineText = m_buffer->lineText(i);

            if (!blockwise) {
                if (i == range.start().line()) {
                    s.append(lineText.mid(range.start().column(), lineText.length() - range.start().column()));
                } else if (i == range.end().line()) {
                    s.append(lineText.mid(0, range.end().column()));
                } else {
                    s.append(lineText);
                }
            } else {
                KTextEditor::Range subRange = rangeOnLine(range, i);
                s.append(lineText.mid(subRange.start().column(), subRange.columnWidth()));
            }

            if (i < range.end().line()) {
                s.append(QLatin1Char('\n'));
            }
        }
    }

    return s;
}

QChar KTextEditor::DocumentPrivate::characterAt(const KTextEditor::Cursor &position) const
{
    const QString lineText = m_buffer->lineText(position.line());
    return lineText.size() < position.column() ? lineText.at(position.column()) : QChar();
}

QString KTextEditor::DocumentPrivate::wordAt(const KTextEditor::Cursor &cursor) const
{
    return text(wordRangeAt(cursor));
}

KTextEditor::Range KTextEditor::DocumentPrivate::wordRangeAt(const KTextEditor::Cursor &cursor) const
{
    // get text line
    const int line = cursor.line();
    const QString lineText = m_buffer->lineText(line);
    if (lineText.isEmpty()) {
        return KTextEditor::Range::invalid();
    }

    // make sure the cursor is
    const int lineLenth = lineText.length();
    if (cursor.column() > lineLenth) {
        return KTextEditor::Range::invalid();
    }

    int start = cursor.column();
    int end = start;

    while (start > 0 && highlight()->isInWord(lineText.at(start - 1), m_buffer->attributeInLine(line, start - 1))) {
        start--;
    }
    while (end < lineLenth && highlight()->isInWord(lineText.at(end), m_buffer->attributeInLine(line, end))) {
        end++;
    }

    return KTextEditor::Range(line, start, line, end);
}

bool KTextEditor::DocumentPrivate::isValidTextPosition(const KTextEditor::Cursor &cursor) const
{
    const int ln = cursor.line();
    const int col = cursor.column();
    // cursor in document range?
    if (ln < 0 || col < 0 || ln >= lines() || col > lineLength(ln)) {
        return false;
    }

    const QString str = line(ln);
    Q_ASSERT(str.length() >= col);

    // cursor at end of line?
    const int len = lineLength(ln);
    if (col == 0 || col == len) {
        return true;
    }

    // cursor in the middle of a valid utf32-surrogate?
    return (!str.at(col).isLowSurrogate()) || (!str.at(col - 1).isHighSurrogate());
}

QStringList KTextEditor::DocumentPrivate::textLines(const KTextEditor::Range &range, bool blockwise) const
{
    QStringList ret;

    if (!range.isValid()) {
        qCWarning(LOG_KTE) << "Text requested for invalid range" << range;
        return ret;
    }

    if (blockwise && (range.start().column() > range.end().column())) {
        return ret;
    }

    if (range.start().line() == range.end().line()) {
        Q_ASSERT(range.start() <= range.end());

        const QString lineText = m_buffer->lineText(range.start().line());

        if (lineText.isEmpty()) {
            return ret;
        }

        ret << lineText.mid(range.start().column(), range.end().column() - range.start().column());
    } else {
        for (int i = range.start().line(); (i <= range.end().line()) && (i < m_buffer->count()); ++i) {
            const QString lineText = m_buffer->lineText(i);

            if (!blockwise) {
                if (i == range.start().line()) {
                    ret << lineText.mid(range.start().column(), lineText.length() - range.start().column());
                } else if (i == range.end().line()) {
                    ret << lineText.mid(0, range.end().column());
                } else {
                    ret << lineText;
                }
            } else {
                KTextEditor::Range subRange = rangeOnLine(range, i);
                ret << lineText.mid(subRange.start().column(), subRange.columnWidth());
            }
        }
    }

    return ret;
}

QString KTextEditor::DocumentPrivate::line(int line) const
{
    return m_buffer->lineText(line);
}

bool KTextEditor::DocumentPrivate::setText(const QString &s)
{
    if (!isReadWrite()) {
        return false;
    }

    std::vector<KTextEditor::Mark> msave;
    msave.reserve(m_marks.size());
    std::transform(m_marks.cbegin(), m_marks.cend(), std::back_inserter(msave), [](KTextEditor::Mark *mark) {
        return *mark;
    });

    editStart();

    // delete the text
    clear();

    // insert the new text
    insertText(KTextEditor::Cursor(), s);

    editEnd();

    for (KTextEditor::Mark mark : msave) {
        setMark(mark.line, mark.type);
    }

    return true;
}

bool KTextEditor::DocumentPrivate::setText(const QStringList &text)
{
    if (!isReadWrite()) {
        return false;
    }

    std::vector<KTextEditor::Mark> msave;
    msave.reserve(m_marks.size());
    std::transform(m_marks.cbegin(), m_marks.cend(), std::back_inserter(msave), [](KTextEditor::Mark *mark) {
        return *mark;
    });

    editStart();

    // delete the text
    clear();

    // insert the new text
    insertText(KTextEditor::Cursor::start(), text);

    editEnd();

    for (KTextEditor::Mark mark : msave) {
        setMark(mark.line, mark.type);
    }

    return true;
}

bool KTextEditor::DocumentPrivate::clear()
{
    if (!isReadWrite()) {
        return false;
    }

    for (auto view : std::as_const(m_views)) {
        view->clear();
        view->tagAll();
        view->update();
    }

    clearMarks();

    Q_EMIT aboutToInvalidateMovingInterfaceContent(this);
    m_buffer->invalidateRanges();

    Q_EMIT aboutToRemoveText(documentRange());

    return editRemoveLines(0, lastLine());
}

bool KTextEditor::DocumentPrivate::insertText(const KTextEditor::Cursor &position, const QString &text, bool block)
{
    if (!isReadWrite()) {
        return false;
    }

    if (text.isEmpty()) {
        return true;
    }

    editStart();

    int currentLine = position.line();
    int currentLineStart = 0;
    const int totalLength = text.length();
    int insertColumn = position.column();

    // pad with empty lines, if insert position is after last line
    if (position.line() > lines()) {
        int line = lines();
        while (line <= position.line()) {
            editInsertLine(line, QString());
            line++;
        }
    }

    // compute expanded column for block mode
    int positionColumnExpanded = insertColumn;
    const int tabWidth = config()->tabWidth();
    if (block) {
        const QString lineText = m_buffer->lineText(currentLine);
        positionColumnExpanded = Kate::TextLineData::toVirtualColumn(lineText, insertColumn, tabWidth);
    }

    int pos = 0;
    for (; pos < totalLength; pos++) {
        const QChar &ch = text.at(pos);

        if (ch == QLatin1Char('\n')) {
            // Only perform the text insert if there is text to insert
            if (currentLineStart < pos) {
                editInsertText(currentLine, insertColumn, text.mid(currentLineStart, pos - currentLineStart));
            }

            if (!block) {
                editWrapLine(currentLine, insertColumn + pos - currentLineStart);
                insertColumn = 0;
            }

            currentLine++;

            if (block) {
                if (currentLine == lastLine() + 1) {
                    editInsertLine(currentLine, QString());
                }
                const QString lineText = m_buffer->lineText(currentLine);
                insertColumn = positionColumnExpanded;
                if (!lineText.isEmpty()) {
                    insertColumn = Kate::TextLineData::fromVirtualColumn(lineText, insertColumn, tabWidth);
                }
            }

            currentLineStart = pos + 1;
        }
    }

    // Only perform the text insert if there is text to insert
    if (currentLineStart < pos) {
        editInsertText(currentLine, insertColumn, text.mid(currentLineStart, pos - currentLineStart));
    }

    editEnd();
    return true;
}

bool KTextEditor::DocumentPrivate::insertText(const KTextEditor::Cursor &position, const QStringList &textLines, bool block)
{
    if (!isReadWrite()) {
        return false;
    }

    // just reuse normal function
    return insertText(position, textLines.join(QLatin1Char('\n')), block);
}

bool KTextEditor::DocumentPrivate::removeText(const KTextEditor::Range &_range, bool block)
{
    KTextEditor::Range range = _range;

    if (!isReadWrite()) {
        return false;
    }

    // Should now be impossible to trigger with the new Range class
    Q_ASSERT(range.start().line() <= range.end().line());

    if (range.start().line() > lastLine()) {
        return false;
    }

    if (!block) {
        Q_EMIT aboutToRemoveText(range);
    }

    editStart();

    if (!block) {
        if (range.end().line() > lastLine()) {
            range.setEnd(KTextEditor::Cursor(lastLine() + 1, 0));
        }

        if (range.onSingleLine()) {
            editRemoveText(range.start().line(), range.start().column(), range.columnWidth());
        } else {
            int from = range.start().line();
            int to = range.end().line();

            // remove last line
            if (to <= lastLine()) {
                editRemoveText(to, 0, range.end().column());
            }

            // editRemoveLines() will be called on first line (to remove bookmark)
            if (range.start().column() == 0 && from > 0) {
                --from;
            }

            // remove middle lines
            editRemoveLines(from + 1, to - 1);

            // remove first line if not already removed by editRemoveLines()
            if (range.start().column() > 0 || range.start().line() == 0) {
                editRemoveText(from, range.start().column(), m_buffer->lineLength(from) - range.start().column());
                editUnWrapLine(from);
            }
        }
    } // if ( ! block )
    else {
        int startLine = qMax(0, range.start().line());
        int vc1 = toVirtualColumn(range.start());
        int vc2 = toVirtualColumn(range.end());
        for (int line = qMin(range.end().line(), lastLine()); line >= startLine; --line) {
            int col1 = fromVirtualColumn(line, vc1);
            int col2 = fromVirtualColumn(line, vc2);
            editRemoveText(line, qMin(col1, col2), qAbs(col2 - col1));
        }
    }

    editEnd();
    return true;
}

bool KTextEditor::DocumentPrivate::insertLine(int l, const QString &str)
{
    if (!isReadWrite()) {
        return false;
    }

    if (l < 0 || l > lines()) {
        return false;
    }

    return editInsertLine(l, str);
}

bool KTextEditor::DocumentPrivate::insertLines(int line, const QStringList &text)
{
    if (!isReadWrite()) {
        return false;
    }

    if (line < 0 || line > lines()) {
        return false;
    }

    bool success = true;
    for (const QString &string : text) {
        success &= editInsertLine(line++, string);
    }

    return success;
}

bool KTextEditor::DocumentPrivate::removeLine(int line)
{
    if (!isReadWrite()) {
        return false;
    }

    if (line < 0 || line > lastLine()) {
        return false;
    }

    return editRemoveLine(line);
}

int KTextEditor::DocumentPrivate::totalCharacters() const
{
    int l = 0;

    for (int i = 0; i < m_buffer->count(); ++i) {
        l += m_buffer->lineLength(i);
    }

    return l;
}

int KTextEditor::DocumentPrivate::lines() const
{
    return m_buffer->count();
}

int KTextEditor::DocumentPrivate::lineLength(int line) const
{
    return m_buffer->lineLength(line);
}

bool KTextEditor::DocumentPrivate::isLineModified(int line) const
{
    return m_buffer->isLineModified(line);
}

bool KTextEditor::DocumentPrivate::isLineSaved(int line) const
{
    return m_buffer->isLineSaved(line);
}

bool KTextEditor::DocumentPrivate::isLineTouched(int line) const
{
    return m_buffer->isLineTouched(line);
}
// END

// BEGIN KTextEditor::EditInterface internal stuff
//
// Starts an edit session with (or without) undo, update of view disabled during session
//
bool KTextEditor::DocumentPrivate::editStart()
{
    editSessionNumber++;

    if (editSessionNumber > 1) {
        return false;
    }

    editIsRunning = true;

    // no last change cursor at start
    m_editLastChangeStartCursor = KTextEditor::Cursor::invalid();

    m_undoManager->editStart();

    for (auto view : std::as_const(m_views)) {
        view->editStart();
    }

    m_buffer->editStart();
    return true;
}

//
// End edit session and update Views
//
bool KTextEditor::DocumentPrivate::editEnd()
{
    if (editSessionNumber == 0) {
        Q_ASSERT(0);
        return false;
    }

    // wrap the new/changed text, if something really changed!
    if (m_buffer->editChanged() && (editSessionNumber == 1)) {
        if (m_undoManager->isActive() && config()->wordWrap()) {
            wrapText(m_buffer->editTagStart(), m_buffer->editTagEnd());
        }
    }

    editSessionNumber--;

    if (editSessionNumber > 0) {
        return false;
    }

    // end buffer edit, will trigger hl update
    // this will cause some possible adjustment of tagline start/end
    m_buffer->editEnd();

    m_undoManager->editEnd();

    // edit end for all views !!!!!!!!!
    for (auto view : std::as_const(m_views)) {
        view->editEnd(m_buffer->editTagStart(), m_buffer->editTagEnd(), m_buffer->editTagFrom());
    }

    if (m_buffer->editChanged()) {
        setModified(true);
        Q_EMIT textChanged(this);
    }

    // remember last change position in the stack, if any
    // this avoid costly updates for longer editing transactions
    // before we did that on textInsert/Removed
    if (m_editLastChangeStartCursor.isValid()) {
        saveEditingPositions(m_editLastChangeStartCursor);
    }

    editIsRunning = false;
    return true;
}

void KTextEditor::DocumentPrivate::pushEditState()
{
    editStateStack.push(editSessionNumber);
}

void KTextEditor::DocumentPrivate::popEditState()
{
    if (editStateStack.isEmpty()) {
        return;
    }

    int count = editStateStack.pop() - editSessionNumber;
    while (count < 0) {
        ++count;
        editEnd();
    }
    while (count > 0) {
        --count;
        editStart();
    }
}

void KTextEditor::DocumentPrivate::inputMethodStart()
{
    m_undoManager->inputMethodStart();
}

void KTextEditor::DocumentPrivate::inputMethodEnd()
{
    m_undoManager->inputMethodEnd();
}

bool KTextEditor::DocumentPrivate::wrapText(int startLine, int endLine)
{
    if (startLine < 0 || endLine < 0) {
        return false;
    }

    if (!isReadWrite()) {
        return false;
    }

    int col = config()->wordWrapAt();

    if (col == 0) {
        return false;
    }

    editStart();

    for (int line = startLine; (line <= endLine) && (line < lines()); line++) {
        const QString lineText = kateTextLine(line);

        if (lineText.isEmpty()) {
            break;
        }

        // qCDebug(LOG_KTE) << "try wrap line: " << line;

        if (Kate::TextLineData::virtualLength(lineText, m_buffer->tabWidth()) > col) {
            const QString nextLine = kateTextLine(line + 1);

            // qCDebug(LOG_KTE) << "do wrap line: " << line;

            int eolPosition = lineText.length() - 1;

            // take tabs into account here, too
            int x = 0;
            const QString &t = lineText;
            int z2 = 0;
            for (; z2 < lineText.length(); z2++) {
                static const QChar tabChar(QLatin1Char('\t'));
                if (t.at(z2) == tabChar) {
                    x += m_buffer->tabWidth() - (x % m_buffer->tabWidth());
                } else {
                    x++;
                }

                if (x > col) {
                    break;
                }
            }

            const int colInChars = qMin(z2, lineText.length() - 1);
            int searchStart = colInChars;

            // If where we are wrapping is an end of line and is a space we don't
            // want to wrap there
            if (searchStart == eolPosition && t.at(searchStart).isSpace()) {
                searchStart--;
            }

            // Scan backwards looking for a place to break the line
            // We are not interested in breaking at the first char
            // of the line (if it is a space), but we are at the second
            // anders: if we can't find a space, try breaking on a word
            // boundary, using KateHighlight::canBreakAt().
            // This could be a priority (setting) in the hl/filetype/document
            int z = -1;
            int nw = -1; // alternative position, a non word character
            for (z = searchStart; z >= 0; z--) {
                if (t.at(z).isSpace()) {
                    break;
                }
                if ((nw < 0) && highlight()->canBreakAt(t.at(z), m_buffer->attributeInLine(line, z))) {
                    nw = z;
                }
            }

            if (z >= 0) {
                // So why don't we just remove the trailing space right away?
                // Well, the (view's) cursor may be directly in front of that space
                // (user typing text before the last word on the line), and if that
                // happens, the cursor would be moved to the next line, which is not
                // what we want (bug #106261)
                z++;
            } else {
                // There was no space to break at so break at a nonword character if
                // found, or at the wrapcolumn ( that needs be configurable )
                // Don't try and add any white space for the break
                if ((nw >= 0) && nw < colInChars) {
                    nw++; // break on the right side of the character
                }
                z = (nw >= 0) ? nw : colInChars;
            }

            if (!m_buffer->isLineAutoWrapped(line + 1)) {
                editWrapLine(line, z, true);
                editMarkLineAutoWrapped(line + 1, true);

                endLine++;
            } else {
                if ((nextLine.length() > 0) && !nextLine.at(0).isSpace() && ((lineText.length() < 1) || !lineText.at(lineText.length() - 1).isSpace())) {
                    editInsertText(line + 1, 0, QStringLiteral(" "));
                }

                bool newLineAdded = false;
                editWrapLine(line, z, false, &newLineAdded);

                editMarkLineAutoWrapped(line + 1, true);

                endLine++;
            }
        }
    }

    editEnd();

    return true;
}

bool KTextEditor::DocumentPrivate::wrapParagraph(int first, int last)
{
    if (first == last) {
        return wrapText(first, last);
    }

    if (first < 0 || last < first) {
        return false;
    }

    if (last >= lines() || first > last) {
        return false;
    }

    if (!isReadWrite()) {
        return false;
    }

    editStart();

    // Because we shrink and expand lines, we need to track the working set by powerful "MovingStuff"
    std::unique_ptr<KTextEditor::MovingRange> range(newMovingRange(KTextEditor::Range(first, 0, last, 0)));
    std::unique_ptr<KTextEditor::MovingCursor> curr(newMovingCursor(KTextEditor::Cursor(range->start())));

    // Scan the selected range for paragraphs, whereas each empty line trigger a new paragraph
    for (int line = first; line <= range->end().line(); ++line) {
        // Is our first line a somehow filled line?
        if (Kate::TextLineData::firstChar(m_buffer->lineText(first)) < 0) {
            // Fast forward to first non empty line
            ++first;
            curr->setPosition(curr->line() + 1, 0);
            continue;
        }

        // Is our current line a somehow filled line? If not, wrap the paragraph
        if (Kate::TextLineData::firstChar(m_buffer->lineText(line)) < 0) {
            curr->setPosition(line, 0); // Set on empty line
            joinLines(first, line - 1);
            // Don't wrap twice! That may cause a bad result
            if (!wordWrap()) {
                wrapText(first, first);
            }
            first = curr->line() + 1;
            line = first;
        }
    }

    // If there was no paragraph, we need to wrap now
    bool needWrap = (curr->line() != range->end().line());
    if (needWrap && Kate::TextLineData::firstChar(line(first)) != -1) {
        joinLines(first, range->end().line());
        // Don't wrap twice! That may cause a bad result
        if (!wordWrap()) {
            wrapText(first, first);
        }
    }

    editEnd();
    return true;
}

bool KTextEditor::DocumentPrivate::editInsertText(int line, int col, const QString &s)
{
    // verbose debug
    EDIT_DEBUG << "editInsertText" << line << col << s;

    if (line < 0 || col < 0) {
        return false;
    }

    if (!isReadWrite()) {
        return false;
    }

    int length = lineLength(line);

    if (length < 0) {
        return false;
    }

    // nothing to do, do nothing!
    if (s.isEmpty()) {
        return true;
    }

    editStart();

    QString s2 = s;
    int col2 = col;
    if (col2 > length) {
        s2 = QString(col2 - length, QLatin1Char(' ')) + s;
        col2 = length;
    }

    m_undoManager->slotTextInserted(line, col2, s2);

    // remember last change cursor
    m_editLastChangeStartCursor = KTextEditor::Cursor(line, col2);

    // insert text into line
    m_buffer->insertText(m_editLastChangeStartCursor, s2);

    Q_EMIT textInsertedRange(this, KTextEditor::Range(line, col2, line, col2 + s2.length()));

    editEnd();

    return true;
}

bool KTextEditor::DocumentPrivate::editRemoveText(int line, int col, int len)
{
    // verbose debug
    EDIT_DEBUG << "editRemoveText" << line << col << len;

    if (line < 0 || col < 0 || len < 0) {
        return false;
    }

    if (!isReadWrite()) {
        return false;
    }

    const QString l = plainKateTextLine(line);

    if (l.isEmpty()) {
        return false;
    }

    // nothing to do, do nothing!
    if (len == 0) {
        return true;
    }

    // wrong column
    if (col >= l.size()) {
        return false;
    }

    // don't try to remove what's not there
    len = qMin(len, l.size() - col);

    editStart();

    QString oldText = l.mid(col, len);

    m_undoManager->slotTextRemoved(line, col, oldText);

    // remember last change cursor
    m_editLastChangeStartCursor = KTextEditor::Cursor(line, col);

    // remove text from line
    m_buffer->removeText(KTextEditor::Range(m_editLastChangeStartCursor, KTextEditor::Cursor(line, col + len)));

    Q_EMIT textRemoved(this, KTextEditor::Range(line, col, line, col + len), oldText);

    editEnd();

    return true;
}

bool KTextEditor::DocumentPrivate::editMarkLineAutoWrapped(int line, bool autowrapped)
{
    // verbose debug
    EDIT_DEBUG << "editMarkLineAutoWrapped" << line << autowrapped;

    if (line < 0) {
        return false;
    }

    if (!isReadWrite()) {
        return false;
    }

    editStart();

    m_undoManager->slotMarkLineAutoWrapped(line, autowrapped);

    buffer().setLineAutoWrapped(line, autowrapped);

    editEnd();

    return true;
}

bool KTextEditor::DocumentPrivate::editWrapLine(int line, int col, bool newLine, bool *newLineAdded)
{
    // verbose debug
    EDIT_DEBUG << "editWrapLine" << line << col << newLine;

    if (line < 0 || col < 0) {
        return false;
    }

    if (!isReadWrite()) {
        return false;
    }

    int length = lineLength(line);

    if (length < 0) {
        return false;
    }

    editStart();

    const bool nextLineValid = lineLength(line + 1) >= 0;

    m_undoManager->slotLineWrapped(line, col, length - col, (!nextLineValid || newLine));

    if (!nextLineValid || newLine) {
        m_buffer->wrapLine(KTextEditor::Cursor(line, col));

        QVarLengthArray<KTextEditor::Mark *, 8> list;
        for (const auto &mark : std::as_const(m_marks)) {
            if (mark->line >= line) {
                if ((col == 0) || (mark->line > line)) {
                    list.push_back(mark);
                }
            }
        }

        for (const auto &mark : list) {
            m_marks.take(mark->line);
        }

        for (const auto &mark : list) {
            mark->line++;
            m_marks.insert(mark->line, mark);
        }

        if (!list.empty()) {
            Q_EMIT marksChanged(this);
        }

        // yes, we added a new line !
        if (newLineAdded) {
            (*newLineAdded) = true;
        }
    } else {
        m_buffer->wrapLine(KTextEditor::Cursor(line, col));
        m_buffer->unwrapLine(line + 2);

        // no, no new line added !
        if (newLineAdded) {
            (*newLineAdded) = false;
        }
    }

    // remember last change cursor
    m_editLastChangeStartCursor = KTextEditor::Cursor(line, col);

    Q_EMIT textInsertedRange(this, KTextEditor::Range(line, col, line + 1, 0));

    editEnd();

    return true;
}

bool KTextEditor::DocumentPrivate::editUnWrapLine(int line, bool removeLine, int length)
{
    // verbose debug
    EDIT_DEBUG << "editUnWrapLine" << line << removeLine << length;

    if (line < 0 || length < 0) {
        return false;
    }

    if (!isReadWrite()) {
        return false;
    }

    int col = lineLength(line);
    bool lineValid = col >= 0;
    bool nextLineValid = lineLength(line + 1) >= 0;

    if (!lineValid || !nextLineValid) {
        return false;
    }

    editStart();

    m_undoManager->slotLineUnWrapped(line, col, length, removeLine);

    if (removeLine) {
        m_buffer->unwrapLine(line + 1);
    } else {
        m_buffer->wrapLine(KTextEditor::Cursor(line + 1, length));
        m_buffer->unwrapLine(line + 1);
    }

    QVarLengthArray<KTextEditor::Mark *, 8> list;
    for (const auto &mark : std::as_const(m_marks)) {
        if (mark->line >= line + 1) {
            list.push_back(mark);
        }

        if (mark->line == line + 1) {
            auto m = m_marks.take(line);
            if (m) {
                mark->type |= m->type;
                delete m;
            }
        }
    }

    for (const auto &mark : list) {
        m_marks.take(mark->line);
    }

    for (const auto &mark : list) {
        mark->line--;
        m_marks.insert(mark->line, mark);
    }

    if (!list.isEmpty()) {
        Q_EMIT marksChanged(this);
    }

    // remember last change cursor
    m_editLastChangeStartCursor = KTextEditor::Cursor(line, col);

    Q_EMIT textRemoved(this, KTextEditor::Range(line, col, line + 1, 0), QStringLiteral("\n"));

    editEnd();

    return true;
}

bool KTextEditor::DocumentPrivate::editInsertLine(int line, const QString &s)
{
    // verbose debug
    EDIT_DEBUG << "editInsertLine" << line << s;

    if (line < 0) {
        return false;
    }

    if (!isReadWrite()) {
        return false;
    }

    if (line > lines()) {
        return false;
    }

    editStart();

    m_undoManager->slotLineInserted(line, s);

    // wrap line
    if (line > 0) {
        const QString previousLine = buffer().lineText(line - 1);
        m_buffer->wrapLine(KTextEditor::Cursor(line - 1, previousLine.size()));
    } else {
        m_buffer->wrapLine(KTextEditor::Cursor(0, 0));
    }

    // insert text
    m_buffer->insertText(KTextEditor::Cursor(line, 0), s);

    QVarLengthArray<KTextEditor::Mark *, 8> list;
    for (const auto &mark : std::as_const(m_marks)) {
        if (mark->line >= line) {
            list.push_back(mark);
        }
    }

    for (const auto &mark : list) {
        m_marks.take(mark->line);
    }

    for (const auto &mark : list) {
        mark->line++;
        m_marks.insert(mark->line, mark);
    }

    if (!list.isEmpty()) {
        Q_EMIT marksChanged(this);
    }

    KTextEditor::Range rangeInserted(line, 0, line, buffer().lineLength(line));

    if (line) {
        int prevLineLength = lineLength(line - 1);
        rangeInserted.setStart(KTextEditor::Cursor(line - 1, prevLineLength));
    } else {
        rangeInserted.setEnd(KTextEditor::Cursor(line + 1, 0));
    }

    // remember last change cursor
    m_editLastChangeStartCursor = rangeInserted.start();

    Q_EMIT textInsertedRange(this, rangeInserted);

    editEnd();

    return true;
}

bool KTextEditor::DocumentPrivate::editRemoveLine(int line)
{
    return editRemoveLines(line, line);
}

bool KTextEditor::DocumentPrivate::editRemoveLines(int from, int to)
{
    // verbose debug
    EDIT_DEBUG << "editRemoveLines" << from << to;

    if (to < from || from < 0 || to > lastLine()) {
        return false;
    }

    if (!isReadWrite()) {
        return false;
    }

    if (lines() == 1) {
        return editRemoveText(0, 0, lineLength(0));
    }

    editStart();
    QStringList oldText;

    // first remove text
    for (int line = to; line >= from; --line) {
        oldText.prepend(this->line(line));
        m_undoManager->slotLineRemoved(line, this->line(line));

        using namespace KTextEditor;
        const auto lineRange = Range(Cursor(line, 0), Cursor(line, m_buffer->lineLength(line)));
        m_buffer->removeText(lineRange);
    }

    // then collapse lines
    for (int line = to; line >= from; --line) {
        // unwrap all lines, prefer to unwrap line behind, skip to wrap line 0
        if (line + 1 < m_buffer->lines()) {
            m_buffer->unwrapLine(line + 1);
        } else if (line) {
            m_buffer->unwrapLine(line);
        }
    }

    QVarLengthArray<int, 8> rmark;
    QVarLengthArray<KTextEditor::Mark *, 8> list;

    for (KTextEditor::Mark *mark : std::as_const(m_marks)) {
        int line = mark->line;
        if (line > to) {
            list << mark;
        } else if (line >= from) {
            rmark << line;
        }
    }

    for (int line : rmark) {
        delete m_marks.take(line);
    }

    for (auto mark : list) {
        m_marks.take(mark->line);
    }

    for (auto mark : list) {
        mark->line -= to - from + 1;
        m_marks.insert(mark->line, mark);
    }

    if (!list.isEmpty()) {
        Q_EMIT marksChanged(this);
    }

    KTextEditor::Range rangeRemoved(from, 0, to + 1, 0);

    if (to == lastLine() + to - from + 1) {
        rangeRemoved.setEnd(KTextEditor::Cursor(to, oldText.last().length()));
        if (from > 0) {
            int prevLineLength = lineLength(from - 1);
            rangeRemoved.setStart(KTextEditor::Cursor(from - 1, prevLineLength));
        }
    }

    // remember last change cursor
    m_editLastChangeStartCursor = rangeRemoved.start();

    Q_EMIT textRemoved(this, rangeRemoved, oldText.join(QLatin1Char('\n')) + QLatin1Char('\n'));

    editEnd();

    return true;
}
// END

// BEGIN KTextEditor::UndoInterface stuff
uint KTextEditor::DocumentPrivate::undoCount() const
{
    return m_undoManager->undoCount();
}

uint KTextEditor::DocumentPrivate::redoCount() const
{
    return m_undoManager->redoCount();
}

void KTextEditor::DocumentPrivate::undo()
{
    m_undoManager->undo();
}

void KTextEditor::DocumentPrivate::redo()
{
    m_undoManager->redo();
}
// END

// BEGIN KTextEditor::SearchInterface stuff
QVector<KTextEditor::Range>
KTextEditor::DocumentPrivate::searchText(KTextEditor::Range range, const QString &pattern, const KTextEditor::SearchOptions options) const
{
    const bool escapeSequences = options.testFlag(KTextEditor::EscapeSequences);
    const bool regexMode = options.testFlag(KTextEditor::Regex);
    const bool backwards = options.testFlag(KTextEditor::Backwards);
    const bool wholeWords = options.testFlag(KTextEditor::WholeWords);
    const Qt::CaseSensitivity caseSensitivity = options.testFlag(KTextEditor::CaseInsensitive) ? Qt::CaseInsensitive : Qt::CaseSensitive;

    if (regexMode) {
        // regexp search
        // escape sequences are supported by definition
        QRegularExpression::PatternOptions patternOptions;
        if (caseSensitivity == Qt::CaseInsensitive) {
            patternOptions |= QRegularExpression::CaseInsensitiveOption;
        }
        KateRegExpSearch searcher(this);
        return searcher.search(pattern, range, backwards, patternOptions);
    }

    if (escapeSequences) {
        // escaped search
        KatePlainTextSearch searcher(this, caseSensitivity, wholeWords);
        KTextEditor::Range match = searcher.search(KateRegExpSearch::escapePlaintext(pattern), range, backwards);

        QVector<KTextEditor::Range> result;
        result.append(match);
        return result;
    }

    // plaintext search
    KatePlainTextSearch searcher(this, caseSensitivity, wholeWords);
    KTextEditor::Range match = searcher.search(pattern, range, backwards);

    QVector<KTextEditor::Range> result;
    result.append(match);
    return result;
}
// END

QWidget *KTextEditor::DocumentPrivate::dialogParent()
{
    QWidget *w = widget();

    if (!w) {
        w = activeView();

        if (!w) {
            w = QApplication::activeWindow();
        }
    }

    return w;
}

QUrl KTextEditor::DocumentPrivate::getSaveFileUrl(const QString &dialogTitle)
{
    // per default we use the url of the current document
    // if that is empty, we will try to get the url of the last used view, we assume some properly ordered views() list is around
    QUrl startUrl = url();
    if (auto mainWindow = KTextEditor::Editor::instance()->application()->activeMainWindow(); mainWindow && !startUrl.isValid()) {
        for (auto view : mainWindow->views()) {
            if (view->document()->url().isValid()) {
                // we remove the filename, otherwise we propose to save as some other already existing file, that is confusing
                startUrl = view->document()->url().adjusted(QUrl::RemoveFilename);
                break;
            }
        }
    }

    // spawn the dialog, dialogParent will take care of proper parent
    return QFileDialog::getSaveFileUrl(dialogParent(), dialogTitle, startUrl.isValid() ? startUrl : QUrl());
}

// BEGIN KTextEditor::HighlightingInterface stuff
bool KTextEditor::DocumentPrivate::setMode(const QString &name)
{
    return updateFileType(name);
}

KTextEditor::DefaultStyle KTextEditor::DocumentPrivate::defaultStyleAt(const KTextEditor::Cursor &position) const
{
    // TODO, FIXME KDE5: in surrogate, use 2 bytes before
    if (!isValidTextPosition(position)) {
        return dsNormal;
    }

    int ds = const_cast<KTextEditor::DocumentPrivate *>(this)->defStyleNum(position.line(), position.column());
    if (ds < 0 || ds > static_cast<int>(dsError)) {
        return dsNormal;
    }

    return static_cast<DefaultStyle>(ds);
}

QString KTextEditor::DocumentPrivate::mode() const
{
    return m_fileType;
}

QStringList KTextEditor::DocumentPrivate::modes() const
{
    QStringList m;

    const QList<KateFileType *> &modeList = KTextEditor::EditorPrivate::self()->modeManager()->list();
    m.reserve(modeList.size());
    for (KateFileType *type : modeList) {
        m << type->name;
    }

    return m;
}

bool KTextEditor::DocumentPrivate::setHighlightingMode(const QString &name)
{
    int mode = KateHlManager::self()->nameFind(name);
    if (mode == -1) {
        return false;
    }
    m_buffer->setHighlight(mode);
    return true;
}

QString KTextEditor::DocumentPrivate::highlightingMode() const
{
    return highlight()->name();
}

QStringList KTextEditor::DocumentPrivate::highlightingModes() const
{
    const auto modeList = KateHlManager::self()->modeList();
    QStringList hls;
    hls.reserve(modeList.size());
    for (const auto &hl : modeList) {
        hls << hl.name();
    }
    return hls;
}

QString KTextEditor::DocumentPrivate::highlightingModeSection(int index) const
{
    return KateHlManager::self()->modeList().at(index).section();
}

QString KTextEditor::DocumentPrivate::modeSection(int index) const
{
    return KTextEditor::EditorPrivate::self()->modeManager()->list().at(index)->section;
}

void KTextEditor::DocumentPrivate::bufferHlChanged()
{
    // update all views
    makeAttribs(false);

    // deactivate indenter if necessary
    m_indenter->checkRequiredStyle();

    Q_EMIT highlightingModeChanged(this);
}

void KTextEditor::DocumentPrivate::setDontChangeHlOnSave()
{
    m_hlSetByUser = true;
}

void KTextEditor::DocumentPrivate::bomSetByUser()
{
    m_bomSetByUser = true;
}
// END

// BEGIN KTextEditor::SessionConfigInterface and KTextEditor::ParameterizedSessionConfigInterface stuff
void KTextEditor::DocumentPrivate::readSessionConfig(const KConfigGroup &kconfig, const QSet<QString> &flags)
{
    if (!flags.contains(QStringLiteral("SkipEncoding"))) {
        // get the encoding
        QString tmpenc = kconfig.readEntry("Encoding");
        if (!tmpenc.isEmpty() && (tmpenc != encoding())) {
            setEncoding(tmpenc);
        }
    }

    if (!flags.contains(QStringLiteral("SkipUrl"))) {
        // restore the url
        QUrl url(kconfig.readEntry("URL"));

        // open the file if url valid
        if (!url.isEmpty() && url.isValid()) {
            openUrl(url);
        } else {
            completed(); // perhaps this should be emitted at the end of this function
        }
    } else {
        completed(); // perhaps this should be emitted at the end of this function
    }

    if (!flags.contains(QStringLiteral("SkipMode"))) {
        // restore the filetype
        // NOTE: if the session config file contains an invalid Mode
        // (for example, one that was deleted or renamed), do not apply it
        if (kconfig.hasKey("Mode")) {
            // restore if set by user, too!
            m_fileTypeSetByUser = kconfig.readEntry("Mode Set By User", false);
            if (m_fileTypeSetByUser) {
                updateFileType(kconfig.readEntry("Mode"));
            } else {
                // Not set by user:
                // - if it's not the default ("Normal") use the mode from the config file
                // - if it's "Normal", use m_fileType which was detected by the code in openFile()
                const QString modeFromCfg = kconfig.readEntry("Mode");
                updateFileType(modeFromCfg != QLatin1String("Normal") ? modeFromCfg : m_fileType);
            }
        }
    }

    if (!flags.contains(QStringLiteral("SkipHighlighting"))) {
        // restore the hl stuff
        if (kconfig.hasKey("Highlighting")) {
            const int mode = KateHlManager::self()->nameFind(kconfig.readEntry("Highlighting"));
            if (mode >= 0) {
                // restore if set by user, too! see bug 332605, otherwise we loose the hl later again on save
                m_hlSetByUser = kconfig.readEntry("Highlighting Set By User", false);

                if (m_hlSetByUser) {
                    m_buffer->setHighlight(mode);
                } else {
                    // Not set by user, only set highlighting if it's not 0, the default,
                    // otherwise leave it the same as the highlighting set by updateFileType()
                    // which has already been called by openFile()
                    if (mode > 0) {
                        m_buffer->setHighlight(mode);
                    }
                }
            }
        }
    }

    // indent mode
    config()->setIndentationMode(kconfig.readEntry("Indentation Mode", config()->indentationMode()));

    // Restore Bookmarks
    const QList<int> marks = kconfig.readEntry("Bookmarks", QList<int>());
    for (int i = 0; i < marks.count(); i++) {
        addMark(marks.at(i), KTextEditor::DocumentPrivate::markType01);
    }
}

void KTextEditor::DocumentPrivate::writeSessionConfig(KConfigGroup &kconfig, const QSet<QString> &flags)
{
    if (this->url().isLocalFile()) {
        const QString path = this->url().toLocalFile();
        if (path.startsWith(QDir::tempPath())) {
            return; // inside tmp resource, do not save
        }
    }

    if (!flags.contains(QStringLiteral("SkipUrl"))) {
        // save url
        kconfig.writeEntry("URL", this->url().toString());
    }

    if (!flags.contains(QStringLiteral("SkipEncoding"))) {
        // save encoding
        kconfig.writeEntry("Encoding", encoding());
    }

    if (!flags.contains(QStringLiteral("SkipMode"))) {
        // save file type
        kconfig.writeEntry("Mode", m_fileType);
        // save if set by user, too!
        kconfig.writeEntry("Mode Set By User", m_fileTypeSetByUser);
    }

    if (!flags.contains(QStringLiteral("SkipHighlighting"))) {
        // save hl
        kconfig.writeEntry("Highlighting", highlight()->name());

        // save if set by user, too! see bug 332605, otherwise we loose the hl later again on save
        kconfig.writeEntry("Highlighting Set By User", m_hlSetByUser);
    }

    // indent mode
    kconfig.writeEntry("Indentation Mode", config()->indentationMode());

    // Save Bookmarks
    QList<int> marks;
    for (const auto &mark : std::as_const(m_marks)) {
        if (mark->type & KTextEditor::MarkInterface::markType01) {
            marks.push_back(mark->line);
        }
    }

    kconfig.writeEntry("Bookmarks", marks);
}

// END KTextEditor::SessionConfigInterface and KTextEditor::ParameterizedSessionConfigInterface stuff

uint KTextEditor::DocumentPrivate::mark(int line)
{
    KTextEditor::Mark *m = m_marks.value(line);
    if (!m) {
        return 0;
    }

    return m->type;
}

void KTextEditor::DocumentPrivate::setMark(int line, uint markType)
{
    clearMark(line);
    addMark(line, markType);
}

void KTextEditor::DocumentPrivate::clearMark(int line)
{
    if (line < 0 || line > lastLine()) {
        return;
    }

    if (auto mark = m_marks.take(line)) {
        Q_EMIT markChanged(this, *mark, MarkRemoved);
        Q_EMIT marksChanged(this);
        delete mark;
        tagLine(line);
        repaintViews(true);
    }
}

void KTextEditor::DocumentPrivate::addMark(int line, uint markType)
{
    KTextEditor::Mark *mark;

    if (line < 0 || line > lastLine()) {
        return;
    }

    if (markType == 0) {
        return;
    }

    if ((mark = m_marks.value(line))) {
        // Remove bits already set
        markType &= ~mark->type;

        if (markType == 0) {
            return;
        }

        // Add bits
        mark->type |= markType;
    } else {
        mark = new KTextEditor::Mark;
        mark->line = line;
        mark->type = markType;
        m_marks.insert(line, mark);
    }

    // Emit with a mark having only the types added.
    KTextEditor::Mark temp;
    temp.line = line;
    temp.type = markType;
    Q_EMIT markChanged(this, temp, MarkAdded);

    Q_EMIT marksChanged(this);
    tagLine(line);
    repaintViews(true);
}

void KTextEditor::DocumentPrivate::removeMark(int line, uint markType)
{
    if (line < 0 || line > lastLine()) {
        return;
    }

    auto it = m_marks.find(line);
    if (it == m_marks.end()) {
        return;
    }
    KTextEditor::Mark *mark = it.value();

    // Remove bits not set
    markType &= mark->type;

    if (markType == 0) {
        return;
    }

    // Subtract bits
    mark->type &= ~markType;

    // Emit with a mark having only the types removed.
    KTextEditor::Mark temp;
    temp.line = line;
    temp.type = markType;
    Q_EMIT markChanged(this, temp, MarkRemoved);

    if (mark->type == 0) {
        m_marks.erase(it);
        delete mark;
    }

    Q_EMIT marksChanged(this);
    tagLine(line);
    repaintViews(true);
}

const QHash<int, KTextEditor::Mark *> &KTextEditor::DocumentPrivate::marks()
{
    return m_marks;
}

void KTextEditor::DocumentPrivate::requestMarkTooltip(int line, QPoint position)
{
    KTextEditor::Mark *mark = m_marks.value(line);
    if (!mark) {
        return;
    }

    bool handled = false;
    Q_EMIT markToolTipRequested(this, *mark, position, handled);
}

bool KTextEditor::DocumentPrivate::handleMarkClick(int line)
{
    bool handled = false;
    KTextEditor::Mark *mark = m_marks.value(line);
    if (!mark) {
        Q_EMIT markClicked(this, KTextEditor::Mark{line, 0}, handled);
    } else {
        Q_EMIT markClicked(this, *mark, handled);
    }

    return handled;
}

bool KTextEditor::DocumentPrivate::handleMarkContextMenu(int line, QPoint position)
{
    bool handled = false;
    KTextEditor::Mark *mark = m_marks.value(line);
    if (!mark) {
        Q_EMIT markContextMenuRequested(this, KTextEditor::Mark{line, 0}, position, handled);
    } else {
        Q_EMIT markContextMenuRequested(this, *mark, position, handled);
    }

    return handled;
}

void KTextEditor::DocumentPrivate::clearMarks()
{
    /**
     * work on a copy as deletions below might trigger the use
     * of m_marks
     */
    const QHash<int, KTextEditor::Mark *> marksCopy = m_marks;
    m_marks.clear();

    for (const auto &m : marksCopy) {
        Q_EMIT markChanged(this, *m, MarkRemoved);
        tagLine(m->line);
        delete m;
    }

    Q_EMIT marksChanged(this);
    repaintViews(true);
}

void KTextEditor::DocumentPrivate::setMarkPixmap(MarkInterface::MarkTypes type, const QPixmap &pixmap)
{
    m_markIcons.insert(type, QVariant::fromValue(pixmap));
}

void KTextEditor::DocumentPrivate::setMarkDescription(MarkInterface::MarkTypes type, const QString &description)
{
    m_markDescriptions.insert(type, description);
}

QPixmap KTextEditor::DocumentPrivate::markPixmap(MarkInterface::MarkTypes type) const
{
    auto icon = m_markIcons.value(type, QVariant::fromValue(QPixmap()));
    return (static_cast<QMetaType::Type>(icon.type()) == QMetaType::QIcon) ? icon.value<QIcon>().pixmap(32) : icon.value<QPixmap>();
}

QColor KTextEditor::DocumentPrivate::markColor(MarkInterface::MarkTypes type) const
{
    uint reserved = (0x1 << KTextEditor::MarkInterface::reservedMarkersCount()) - 1;
    if ((uint)type >= (uint)markType01 && (uint)type <= reserved) {
        return KateRendererConfig::global()->lineMarkerColor(type);
    } else {
        return QColor();
    }
}

QString KTextEditor::DocumentPrivate::markDescription(MarkInterface::MarkTypes type) const
{
    return m_markDescriptions.value(type, QString());
}

void KTextEditor::DocumentPrivate::setEditableMarks(uint markMask)
{
    m_editableMarks = markMask;
}

uint KTextEditor::DocumentPrivate::editableMarks() const
{
    return m_editableMarks;
}
// END

void KTextEditor::DocumentPrivate::setMarkIcon(MarkInterface::MarkTypes markType, const QIcon &icon)
{
    m_markIcons.insert(markType, QVariant::fromValue(icon));
}

QIcon KTextEditor::DocumentPrivate::markIcon(MarkInterface::MarkTypes markType) const
{
    auto icon = m_markIcons.value(markType, QVariant::fromValue(QIcon()));
    return (static_cast<QMetaType::Type>(icon.type()) == QMetaType::QIcon) ? icon.value<QIcon>() : QIcon(icon.value<QPixmap>());
}

// BEGIN KTextEditor::PrintInterface stuff
bool KTextEditor::DocumentPrivate::print()
{
    return KatePrinter::print(this);
}

void KTextEditor::DocumentPrivate::printPreview()
{
    KatePrinter::printPreview(this);
}
// END KTextEditor::PrintInterface stuff

// BEGIN KTextEditor::DocumentInfoInterface (### unfinished)
QString KTextEditor::DocumentPrivate::mimeType()
{
    if (!m_modOnHd && url().isLocalFile()) {
        // for unmodified files that reside directly on disk, we don't need to
        // create a temporary buffer - we can just look at the file directly
        return QMimeDatabase().mimeTypeForFile(url().toLocalFile()).name();
    }
    // collect first 4k of text
    // only heuristic
    QByteArray buf;
    for (int i = 0; (i < lines()) && (buf.size() <= 4096); ++i) {
        buf.append(line(i).toUtf8());
        buf.append('\n');
    }

    // use path of url, too, if set
    if (!url().path().isEmpty()) {
        return QMimeDatabase().mimeTypeForFileNameAndData(url().path(), buf).name();
    }

    // else only use the content
    return QMimeDatabase().mimeTypeForData(buf).name();
}
// END KTextEditor::DocumentInfoInterface

// BEGIN: error
void KTextEditor::DocumentPrivate::showAndSetOpeningErrorAccess()
{
    QPointer<KTextEditor::Message> message = new KTextEditor::Message(
        i18n("The file %1 could not be loaded, as it was not possible to read from it.<br />Check if you have read access to this file.",
             this->url().toDisplayString(QUrl::PreferLocalFile)),
        KTextEditor::Message::Error);
    message->setWordWrap(true);
    QAction *tryAgainAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                          i18nc("translators: you can also translate 'Try Again' with 'Reload'", "Try Again"),
                                          nullptr);
    connect(tryAgainAction, &QAction::triggered, this, &KTextEditor::DocumentPrivate::documentReload, Qt::QueuedConnection);

    QAction *closeAction = new QAction(QIcon::fromTheme(QStringLiteral("window-close")), i18n("&Close"), nullptr);
    closeAction->setToolTip(i18n("Close message"));

    // add try again and close actions
    message->addAction(tryAgainAction);
    message->addAction(closeAction);

    // finally post message
    postMessage(message);

    // remember error
    m_openingError = true;
    m_openingErrorMessage = i18n("The file %1 could not be loaded, as it was not possible to read from it.\n\nCheck if you have read access to this file.",
                                 this->url().toDisplayString(QUrl::PreferLocalFile));
}
// END: error

void KTextEditor::DocumentPrivate::openWithLineLengthLimitOverride()
{
    // raise line length limit to the next power of 2
    const int longestLine = m_buffer->longestLineLoaded();
    int newLimit = pow(2, ceil(log2(longestLine)));
    if (newLimit <= longestLine) {
        newLimit *= 2;
    }

    // do the raise
    config()->setLineLengthLimit(newLimit);

    // just reload
    m_buffer->clear();
    openFile();
    if (!m_openingError) {
        setReadWrite(true);
        m_readWriteStateBeforeLoading = true;
    }
}

int KTextEditor::DocumentPrivate::lineLengthLimit() const
{
    return config()->lineLengthLimit();
}

// BEGIN KParts::ReadWrite stuff
bool KTextEditor::DocumentPrivate::openFile()
{
    // we are about to invalidate all cursors/ranges/.. => m_buffer->openFile will do so
    Q_EMIT aboutToInvalidateMovingInterfaceContent(this);

    // no open errors until now...
    m_openingError = false;
    m_openingErrorMessage.clear();

    // add new m_file to dirwatch
    activateDirWatch();

    // remember current encoding
    QString currentEncoding = encoding();

    //
    // mime type magic to get encoding right
    //
    QString mimeType = arguments().mimeType();
    int pos = mimeType.indexOf(QLatin1Char(';'));
    if (pos != -1 && !(m_reloading && m_userSetEncodingForNextReload)) {
        setEncoding(mimeType.mid(pos + 1));
    }

    // update file type, we do this here PRE-LOAD, therefore pass file name for reading from
    updateFileType(KTextEditor::EditorPrivate::self()->modeManager()->fileType(this, localFilePath()));

    // read dir config (if possible and wanted)
    // do this PRE-LOAD to get encoding info!
    readDirConfig();

    // perhaps we need to re-set again the user encoding
    if (m_reloading && m_userSetEncodingForNextReload && (currentEncoding != encoding())) {
        setEncoding(currentEncoding);
    }

    bool success = m_buffer->openFile(localFilePath(), (m_reloading && m_userSetEncodingForNextReload));

    //
    // yeah, success
    // read variables
    //
    if (success) {
        readVariables();
    }

    //
    // update views
    //
    for (auto view : std::as_const(m_views)) {
        // This is needed here because inserting the text moves the view's start position (it is a MovingCursor)
        view->setCursorPosition(KTextEditor::Cursor());
        view->updateView(true);
    }

    // Inform that the text has changed (required as we're not inside the usual editStart/End stuff)
    Q_EMIT textChanged(this);
    Q_EMIT loaded(this);

    //
    // to houston, we are not modified
    //
    if (m_modOnHd) {
        m_modOnHd = false;
        m_modOnHdReason = OnDiskUnmodified;
        m_prevModOnHdReason = OnDiskUnmodified;
        Q_EMIT modifiedOnDisk(this, m_modOnHd, m_modOnHdReason);
    }

    // Now that we have some text, try to auto detect indent if enabled
    // skip this if for this document already settings were done, either by the user or .e.g. modelines/.kateconfig files.
    if (!isEmpty() && config()->autoDetectIndent() && !config()->isSet(KateDocumentConfig::IndentationWidth)
        && !config()->isSet(KateDocumentConfig::ReplaceTabsWithSpaces)) {
        KateIndentDetecter detecter(this);
        auto result = detecter.detect(config()->indentationWidth(), config()->replaceTabsDyn());
        config()->setIndentationWidth(result.indentWidth);
        config()->setReplaceTabsDyn(result.indentUsingSpaces);
    }

    //
    // display errors
    //
    if (!success) {
        showAndSetOpeningErrorAccess();
    }

    // warn: broken encoding
    if (m_buffer->brokenEncoding()) {
        // this file can't be saved again without killing it
        setReadWrite(false);
        m_readWriteStateBeforeLoading = false;
        QPointer<KTextEditor::Message> message = new KTextEditor::Message(
            i18n("The file %1 was opened with %2 encoding but contained invalid characters.<br />"
                 "It is set to read-only mode, as saving might destroy its content.<br />"
                 "Either reopen the file with the correct encoding chosen or enable the read-write mode again in the tools menu to be able to edit it.",
                 this->url().toDisplayString(QUrl::PreferLocalFile),
                 QString::fromLatin1(m_buffer->textCodec()->name())),
            KTextEditor::Message::Warning);
        message->setWordWrap(true);
        postMessage(message);

        // remember error
        m_openingError = true;
        m_openingErrorMessage = i18n(
            "The file %1 was opened with %2 encoding but contained invalid characters."
            " It is set to read-only mode, as saving might destroy its content."
            " Either reopen the file with the correct encoding chosen or enable the read-write mode again in the tools menu to be able to edit it.",
            this->url().toDisplayString(QUrl::PreferLocalFile),
            QString::fromLatin1(m_buffer->textCodec()->name()));
    }

    // warn: too long lines
    if (m_buffer->tooLongLinesWrapped()) {
        // this file can't be saved again without modifications
        setReadWrite(false);
        m_readWriteStateBeforeLoading = false;
        QPointer<KTextEditor::Message> message =
            new KTextEditor::Message(i18n("The file %1 was opened and contained lines longer than the configured Line Length Limit (%2 characters).<br />"
                                          "The longest of those lines was %3 characters long<br/>"
                                          "Those lines were wrapped and the document is set to read-only mode, as saving will modify its content.",
                                          this->url().toDisplayString(QUrl::PreferLocalFile),
                                          config()->lineLengthLimit(),
                                          m_buffer->longestLineLoaded()),
                                     KTextEditor::Message::Warning);
        QAction *increaseAndReload = new QAction(i18n("Temporarily raise limit and reload file"), message);
        connect(increaseAndReload, &QAction::triggered, this, &KTextEditor::DocumentPrivate::openWithLineLengthLimitOverride);
        message->addAction(increaseAndReload, true);
        message->addAction(new QAction(i18n("Close"), message), true);
        message->setWordWrap(true);
        postMessage(message);

        // remember error
        m_openingError = true;
        m_openingErrorMessage = i18n(
            "The file %1 was opened and contained lines longer than the configured Line Length Limit (%2 characters).<br/>"
            "The longest of those lines was %3 characters long<br/>"
            "Those lines were wrapped and the document is set to read-only mode, as saving will modify its content.",
            this->url().toDisplayString(QUrl::PreferLocalFile),
            config()->lineLengthLimit(),
            m_buffer->longestLineLoaded());
    }

    //
    // return the success
    //
    return success;
}

bool KTextEditor::DocumentPrivate::saveFile()
{
    // delete pending mod-on-hd message if applicable.
    delete m_modOnHdHandler;

    // some warnings, if file was changed by the outside!
    if (!url().isEmpty()) {
        if (m_fileChangedDialogsActivated && m_modOnHd) {
            QString str = reasonedMOHString() + QLatin1String("\n\n");

            if (!isModified()) {
                if (KMessageBox::warningContinueCancel(
                        dialogParent(),
                        str + i18n("Do you really want to save this unmodified file? You could overwrite changed data in the file on disk."),
                        i18n("Trying to Save Unmodified File"),
                        KGuiItem(i18n("Save Nevertheless")))
                    != KMessageBox::Continue) {
                    return false;
                }
            } else {
                if (KMessageBox::warningContinueCancel(
                        dialogParent(),
                        str
                            + i18n(
                                "Do you really want to save this file? Both your open file and the file on disk were changed. There could be some data lost."),
                        i18n("Possible Data Loss"),
                        KGuiItem(i18n("Save Nevertheless")))
                    != KMessageBox::Continue) {
                    return false;
                }
            }
        }
    }

    //
    // can we encode it if we want to save it ?
    //
    if (!m_buffer->canEncode()
        && (KMessageBox::warningContinueCancel(dialogParent(),
                                               i18n("The selected encoding cannot encode every Unicode character in this document. Do you really want to save "
                                                    "it? There could be some data lost."),
                                               i18n("Possible Data Loss"),
                                               KGuiItem(i18n("Save Nevertheless")))
            != KMessageBox::Continue)) {
        return false;
    }

    // create a backup file or abort if that fails!
    // if no backup file wanted, this routine will just return true
    if (!createBackupFile()) {
        return false;
    }

    // update file type, pass no file path, read file type content from this document
    QString oldPath = m_dirWatchFile;

    // only update file type if path has changed so that variables are not overridden on normal save
    if (oldPath != localFilePath()) {
        updateFileType(KTextEditor::EditorPrivate::self()->modeManager()->fileType(this, QString()));

        if (url().isLocalFile()) {
            // if file is local then read dir config for new path
            readDirConfig();
        }
    }

    // read our vars
    readVariables();

    // remove file from dirwatch
    deactivateDirWatch();

    // remove all trailing spaces in the document (as edit actions)
    // NOTE: we need this as edit actions, since otherwise the edit actions
    //       in the swap file recovery may happen at invalid cursor positions
    removeTrailingSpaces();

    //
    // try to save
    //
    if (!m_buffer->saveFile(localFilePath())) {
        // add m_file again to dirwatch
        activateDirWatch(oldPath);
        KMessageBox::error(dialogParent(),
                           i18n("The document could not be saved, as it was not possible to write to %1.\nCheck that you have write access to this file or "
                                "that enough disk space is available.\nThe original file may be lost or damaged. "
                                "Don't quit the application until the file is successfully written.",
                                this->url().toDisplayString(QUrl::PreferLocalFile)));
        return false;
    }

    // update the checksum
    createDigest();

    // add m_file again to dirwatch
    activateDirWatch();

    //
    // we are not modified
    //
    if (m_modOnHd) {
        m_modOnHd = false;
        m_modOnHdReason = OnDiskUnmodified;
        m_prevModOnHdReason = OnDiskUnmodified;
        Q_EMIT modifiedOnDisk(this, m_modOnHd, m_modOnHdReason);
    }

    // (dominik) mark last undo group as not mergeable, otherwise the next
    // edit action might be merged and undo will never stop at the saved state
    m_undoManager->undoSafePoint();
    m_undoManager->updateLineModifications();

    //
    // return success
    //
    return true;
}

bool KTextEditor::DocumentPrivate::createBackupFile()
{
    // backup for local or remote files wanted?
    const bool backupLocalFiles = config()->backupOnSaveLocal();
    const bool backupRemoteFiles = config()->backupOnSaveRemote();

    // early out, before mount check: backup wanted at all?
    // => if not, all fine, just return
    if (!backupLocalFiles && !backupRemoteFiles) {
        return true;
    }

    // decide if we need backup based on locality
    // skip that, if we always want backups, as currentMountPoints is not that fast
    QUrl u(url());
    bool needBackup = backupLocalFiles && backupRemoteFiles;
    if (!needBackup) {
        bool slowOrRemoteFile = !u.isLocalFile();
        if (!slowOrRemoteFile) {
            // could be a mounted remote filesystem (e.g. nfs, sshfs, cifs)
            // we have the early out above to skip this, if we want no backup, which is the default
            KMountPoint::Ptr mountPoint = KMountPoint::currentMountPoints().findByDevice(u.toLocalFile());
            slowOrRemoteFile = (mountPoint && mountPoint->probablySlow());
        }
        needBackup = (!slowOrRemoteFile && backupLocalFiles) || (slowOrRemoteFile && backupRemoteFiles);
    }

    // no backup needed? be done
    if (!needBackup) {
        return true;
    }

    // else: try to backup
    const auto backupPrefix = KTextEditor::EditorPrivate::self()->variableExpansionManager()->expandText(config()->backupPrefix(), nullptr);
    const auto backupSuffix = KTextEditor::EditorPrivate::self()->variableExpansionManager()->expandText(config()->backupSuffix(), nullptr);
    if (backupPrefix.isEmpty() && backupSuffix.isEmpty()) {
        // no sane backup possible
        return true;
    }

    if (backupPrefix.contains(QDir::separator())) {
        // replace complete path, as prefix is a path!
        u.setPath(backupPrefix + u.fileName() + backupSuffix);
    } else {
        // replace filename in url
        const QString fileName = u.fileName();
        u = u.adjusted(QUrl::RemoveFilename);
        u.setPath(u.path() + backupPrefix + fileName + backupSuffix);
    }

    qCDebug(LOG_KTE) << "backup src file name: " << url();
    qCDebug(LOG_KTE) << "backup dst file name: " << u;

    // handle the backup...
    bool backupSuccess = false;

    // local file mode, no kio
    if (u.isLocalFile()) {
        if (QFile::exists(url().toLocalFile())) {
            // first: check if backupFile is already there, if true, unlink it
            QFile backupFile(u.toLocalFile());
            if (backupFile.exists()) {
                backupFile.remove();
            }

            backupSuccess = QFile::copy(url().toLocalFile(), u.toLocalFile());
        } else {
            backupSuccess = true;
        }
    } else { // remote file mode, kio
        // get the right permissions, start with safe default
        KIO::StatJob *statJob = KIO::statDetails(url(), KIO::StatJob::SourceSide, KIO::StatBasic);
        KJobWidgets::setWindow(statJob, QApplication::activeWindow());
        if (statJob->exec()) {
            // do a evil copy which will overwrite target if possible
            KFileItem item(statJob->statResult(), url());
            KIO::FileCopyJob *job = KIO::file_copy(url(), u, item.permissions(), KIO::Overwrite);
            KJobWidgets::setWindow(job, QApplication::activeWindow());
            backupSuccess = job->exec();
        } else {
            backupSuccess = true;
        }
    }

    // backup has failed, ask user how to proceed
    if (!backupSuccess
        && (KMessageBox::warningContinueCancel(dialogParent(),
                                               i18n("For file %1 no backup copy could be created before saving."
                                                    " If an error occurs while saving, you might lose the data of this file."
                                                    " A reason could be that the media you write to is full or the directory of the file is read-only for you.",
                                                    url().toDisplayString(QUrl::PreferLocalFile)),
                                               i18n("Failed to create backup copy."),
                                               KGuiItem(i18n("Try to Save Nevertheless")),
                                               KStandardGuiItem::cancel(),
                                               QStringLiteral("Backup Failed Warning"))
            != KMessageBox::Continue)) {
        return false;
    }

    return true;
}

void KTextEditor::DocumentPrivate::readDirConfig()
{
    if (!url().isLocalFile() || KNetworkMounts::self()->isOptionEnabledForPath(url().toLocalFile(), KNetworkMounts::MediumSideEffectsOptimizations)) {
        return;
    }

    // first search .kateconfig upwards
    // with recursion guard
    QSet<QString> seenDirectories;
    QDir dir(QFileInfo(localFilePath()).absolutePath());
    while (!seenDirectories.contains(dir.absolutePath())) {
        // fill recursion guard
        seenDirectories.insert(dir.absolutePath());

        // try to open config file in this dir
        QFile f(dir.absolutePath() + QLatin1String("/.kateconfig"));
        if (f.open(QIODevice::ReadOnly)) {
            QTextStream stream(&f);

            uint linesRead = 0;
            QString line = stream.readLine();
            while ((linesRead < 32) && !line.isNull()) {
                readVariableLine(line);

                line = stream.readLine();

                linesRead++;
            }

            return;
        }

        // else: cd up, if possible or abort
        if (!dir.cdUp()) {
            break;
        }
    }

#if EDITORCONFIG_FOUND
    // if there wasn’t any .kateconfig file and KTextEditor was compiled with
    // EditorConfig support, try to load document config from a .editorconfig
    // file, if such is provided
    EditorConfig editorConfig(this);
    editorConfig.parse();
#endif
}

void KTextEditor::DocumentPrivate::activateDirWatch(const QString &useFileName)
{
    QString fileToUse = useFileName;
    if (fileToUse.isEmpty()) {
        fileToUse = localFilePath();
    }

    if (KNetworkMounts::self()->isOptionEnabledForPath(fileToUse, KNetworkMounts::KDirWatchDontAddWatches)) {
        return;
    }

    QFileInfo fileInfo = QFileInfo(fileToUse);
    if (fileInfo.isSymLink()) {
        // Monitor the actual data and not the symlink
        fileToUse = fileInfo.canonicalFilePath();
    }

    // same file as we are monitoring, return
    if (fileToUse == m_dirWatchFile) {
        return;
    }

    // remove the old watched file
    deactivateDirWatch();

    // add new file if needed
    if (url().isLocalFile() && !fileToUse.isEmpty()) {
        KTextEditor::EditorPrivate::self()->dirWatch()->addFile(fileToUse);
        m_dirWatchFile = fileToUse;
    }
}

void KTextEditor::DocumentPrivate::deactivateDirWatch()
{
    if (!m_dirWatchFile.isEmpty()) {
        KTextEditor::EditorPrivate::self()->dirWatch()->removeFile(m_dirWatchFile);
    }

    m_dirWatchFile.clear();
}

bool KTextEditor::DocumentPrivate::openUrl(const QUrl &url)
{
    if (!m_reloading) {
        // Reset filetype when opening url
        m_fileTypeSetByUser = false;
    }
    bool res = KTextEditor::Document::openUrl(normalizeUrl(url));
    updateDocName();
    return res;
}

bool KTextEditor::DocumentPrivate::closeUrl()
{
    //
    // file mod on hd
    //
    if (!m_reloading && !url().isEmpty()) {
        if (m_fileChangedDialogsActivated && m_modOnHd) {
            // make sure to not forget pending mod-on-hd handler
            delete m_modOnHdHandler;

            QWidget *parentWidget(dialogParent());
            if (!(KMessageBox::warningContinueCancel(parentWidget,
                                                     reasonedMOHString() + QLatin1String("\n\n")
                                                         + i18n("Do you really want to continue to close this file? Data loss may occur."),
                                                     i18n("Possible Data Loss"),
                                                     KGuiItem(i18n("Close Nevertheless")),
                                                     KStandardGuiItem::cancel(),
                                                     QStringLiteral("kate_close_modonhd_%1").arg(m_modOnHdReason))
                  == KMessageBox::Continue)) {
                // reset reloading
                m_reloading = false;
                return false;
            }
        }
    }

    //
    // first call the normal kparts implementation
    //
    if (!KParts::ReadWritePart::closeUrl()) {
        // reset reloading
        m_reloading = false;
        return false;
    }

    // Tell the world that we're about to go ahead with the close
    if (!m_reloading) {
        Q_EMIT aboutToClose(this);
    }

    // delete all KTE::Messages
    if (!m_messageHash.isEmpty()) {
        const auto keys = m_messageHash.keys();
        for (KTextEditor::Message *message : keys) {
            delete message;
        }
    }

    // we are about to invalidate all cursors/ranges/.. => m_buffer->clear will do so
    Q_EMIT aboutToInvalidateMovingInterfaceContent(this);

    // remove file from dirwatch
    deactivateDirWatch();

    //
    // empty url + fileName
    //
    setUrl(QUrl());
    setLocalFilePath(QString());

    // we are not modified
    if (m_modOnHd) {
        m_modOnHd = false;
        m_modOnHdReason = OnDiskUnmodified;
        m_prevModOnHdReason = OnDiskUnmodified;
        Q_EMIT modifiedOnDisk(this, m_modOnHd, m_modOnHdReason);
    }

    // remove all marks
    clearMarks();

    // clear the buffer
    m_buffer->clear();

    // clear undo/redo history
    m_undoManager->clearUndo();
    m_undoManager->clearRedo();

    // no, we are no longer modified
    setModified(false);

    // we have no longer any hl
    m_buffer->setHighlight(0);

    // update all our views
    for (auto view : std::as_const(m_views)) {
        view->clearSelection(); // fix bug #118588
        view->clear();
    }

    // purge swap file
    if (m_swapfile) {
        m_swapfile->fileClosed();
    }

    // success
    return true;
}

bool KTextEditor::DocumentPrivate::isDataRecoveryAvailable() const
{
    return m_swapfile && m_swapfile->shouldRecover();
}

void KTextEditor::DocumentPrivate::recoverData()
{
    if (isDataRecoveryAvailable()) {
        m_swapfile->recover();
    }
}

void KTextEditor::DocumentPrivate::discardDataRecovery()
{
    if (isDataRecoveryAvailable()) {
        m_swapfile->discard();
    }
}

void KTextEditor::DocumentPrivate::setReadWrite(bool rw)
{
    if (isReadWrite() == rw) {
        return;
    }

    KParts::ReadWritePart::setReadWrite(rw);

    for (auto view : std::as_const(m_views)) {
        view->slotUpdateUndo();
        view->slotReadWriteChanged();
    }

    Q_EMIT readWriteChanged(this);
}

void KTextEditor::DocumentPrivate::setModified(bool m)
{
    if (isModified() != m) {
        KParts::ReadWritePart::setModified(m);

        for (auto view : std::as_const(m_views)) {
            view->slotUpdateUndo();
        }

        Q_EMIT modifiedChanged(this);
    }

    m_undoManager->setModified(m);
}
// END

// BEGIN Kate specific stuff ;)

void KTextEditor::DocumentPrivate::makeAttribs(bool needInvalidate)
{
    for (auto view : std::as_const(m_views)) {
        view->renderer()->updateAttributes();
    }

    if (needInvalidate) {
        m_buffer->invalidateHighlighting();
    }

    for (auto view : std::as_const(m_views)) {
        view->tagAll();
        view->updateView(true);
    }
}

// the attributes of a hl have changed, update
void KTextEditor::DocumentPrivate::internalHlChanged()
{
    makeAttribs();
}

void KTextEditor::DocumentPrivate::addView(KTextEditor::View *view)
{
    Q_ASSERT(!m_views.contains(view));
    m_views.insert(view, static_cast<KTextEditor::ViewPrivate *>(view));
    m_viewsCache.append(view);

    // apply the view & renderer vars from the file type
    if (!m_fileType.isEmpty()) {
        readVariableLine(KTextEditor::EditorPrivate::self()->modeManager()->fileType(m_fileType).varLine, true);
    }

    // apply the view & renderer vars from the file
    readVariables(true);

    setActiveView(view);
}

void KTextEditor::DocumentPrivate::removeView(KTextEditor::View *view)
{
    Q_ASSERT(m_views.contains(view));
    m_views.remove(view);
    m_viewsCache.removeAll(view);

    if (activeView() == view) {
        setActiveView(nullptr);
    }
}

void KTextEditor::DocumentPrivate::setActiveView(KTextEditor::View *view)
{
    if (m_activeView == view) {
        return;
    }

    m_activeView = static_cast<KTextEditor::ViewPrivate *>(view);
}

bool KTextEditor::DocumentPrivate::ownedView(KTextEditor::ViewPrivate *view)
{
    // do we own the given view?
    return (m_views.contains(view));
}

int KTextEditor::DocumentPrivate::toVirtualColumn(int line, int column) const
{
    const QString lineText = m_buffer->lineText(line);
    return Kate::TextLineData::toVirtualColumn(lineText, column, config()->tabWidth());
}

int KTextEditor::DocumentPrivate::toVirtualColumn(const KTextEditor::Cursor cursor) const
{
    return toVirtualColumn(cursor.line(), cursor.column());
}

int KTextEditor::DocumentPrivate::fromVirtualColumn(int line, int column) const
{
    const QString lineText = m_buffer->lineText(line);
    return Kate::TextLineData::fromVirtualColumn(lineText, column, config()->tabWidth());
}

int KTextEditor::DocumentPrivate::fromVirtualColumn(const KTextEditor::Cursor cursor) const
{
    return fromVirtualColumn(cursor.line(), cursor.column());
}

void KTextEditor::DocumentPrivate::typeChars(KTextEditor::ViewPrivate *view, QString chars)
{
    // nop for empty chars
    if (chars.isEmpty()) {
        return;
    }

    // auto bracket handling
    QChar closingBracket;
    if (view->config()->autoBrackets()) {
        // Check if entered closing bracket is already balanced
        const QChar typedChar = chars.at(0);
        const QChar openBracket = matchingStartBracket(typedChar);
        if (!openBracket.isNull()) {
            KTextEditor::Cursor curPos = view->cursorPosition();
            if ((characterAt(curPos) == typedChar) && findMatchingBracket(curPos, 123 /*Which value may best?*/).isValid()) {
                // Do nothing
                view->cursorRight();
                return;
            }
        }

        // for newly inserted text: remember if we should auto add some bracket
        if (chars.size() == 1) {
            // we inserted a bracket? => remember the matching closing one
            closingBracket = matchingEndBracket(typedChar);

            // closing bracket for the autobracket we inserted earlier?
            if (m_currentAutobraceClosingChar == typedChar && m_currentAutobraceRange) {
                // do nothing
                m_currentAutobraceRange.reset(nullptr);
                view->cursorRight();
                return;
            }
        }
    }

    // Treat some char also as "auto bracket" only when we have a selection
    if (view->selection() && closingBracket.isNull() && view->config()->encloseSelectionInChars()) {
        const QChar typedChar = chars.at(0);
        if (view->config()->charsToEncloseSelection().contains(typedChar)) {
            // The unconditional mirroring cause no harm, but allows funny brackets
            closingBracket = typedChar.mirroredChar();
        }
    }

    editStart();

    // special handling if we want to add auto brackets to a selection
    if (view->selection() && !closingBracket.isNull()) {
        std::unique_ptr<KTextEditor::MovingRange> selectionRange(newMovingRange(view->selectionRange()));
        const int startLine = qMax(0, selectionRange->start().line());
        const int endLine = qMin(selectionRange->end().line(), lastLine());
        const bool blockMode = view->blockSelection() && (startLine != endLine);
        if (blockMode) {
            if (selectionRange->start().column() > selectionRange->end().column()) {
                // Selection was done from right->left, requires special setting to ensure the new
                // added brackets will not be part of the selection
                selectionRange->setInsertBehaviors(MovingRange::ExpandLeft | MovingRange::ExpandRight);
            }
            // Add brackets to each line of the block
            const int startColumn = qMin(selectionRange->start().column(), selectionRange->end().column());
            const int endColumn = qMax(selectionRange->start().column(), selectionRange->end().column());
            const KTextEditor::Range workingRange(startLine, startColumn, endLine, endColumn);
            for (int line = startLine; line <= endLine; ++line) {
                const KTextEditor::Range r(rangeOnLine(workingRange, line));
                insertText(r.end(), QString(closingBracket));
                view->slotTextInserted(view, r.end(), QString(closingBracket));
                insertText(r.start(), chars);
                view->slotTextInserted(view, r.start(), chars);
            }

        } else {
            // No block, just add to start & end of selection
            insertText(selectionRange->end(), QString(closingBracket));
            view->slotTextInserted(view, selectionRange->end(), QString(closingBracket));
            insertText(selectionRange->start(), chars);
            view->slotTextInserted(view, selectionRange->start(), chars);
        }

        // Refresh selection
        view->setSelection(selectionRange->toRange());
        view->setCursorPosition(selectionRange->end());

        editEnd();
        return;
    }

    // normal handling
    if (!view->config()->persistentSelection() && view->selection()) {
        view->removeSelectedText();
    }

    const KTextEditor::Cursor oldCur(view->cursorPosition());

    const bool multiLineBlockMode = view->blockSelection() && view->selection();
    if (view->currentInputMode()->overwrite()) {
        // blockmode multiline selection case: remove chars in every line
        const KTextEditor::Range selectionRange = view->selectionRange();
        const int startLine = multiLineBlockMode ? qMax(0, selectionRange.start().line()) : view->cursorPosition().line();
        const int endLine = multiLineBlockMode ? qMin(selectionRange.end().line(), lastLine()) : startLine;
        const int virtualColumn = toVirtualColumn(multiLineBlockMode ? selectionRange.end() : view->cursorPosition());

        for (int line = endLine; line >= startLine; --line) {
            const QString &lineText = m_buffer->lineText(line);
            const int column = fromVirtualColumn(line, virtualColumn);
            KTextEditor::Range r = KTextEditor::Range(KTextEditor::Cursor(line, column), qMin(chars.length(), lineText.length() - column));

            // replace mode needs to know what was removed so it can be restored with backspace
            if (oldCur.column() < lineLength(line)) {
                QChar removed = characterAt(KTextEditor::Cursor(line, column));
                view->currentInputMode()->overwrittenChar(removed);
            }

            removeText(r);
        }
    }

    chars = eventuallyReplaceTabs(view->cursorPosition(), chars);

    if (multiLineBlockMode) {
        KTextEditor::Range selectionRange = view->selectionRange();
        const int startLine = qMax(0, selectionRange.start().line());
        const int endLine = qMin(selectionRange.end().line(), lastLine());
        const int column = toVirtualColumn(selectionRange.end());
        for (int line = endLine; line >= startLine; --line) {
            editInsertText(line, fromVirtualColumn(line, column), chars);
        }
        int newSelectionColumn = toVirtualColumn(view->cursorPosition());
        selectionRange.setRange(KTextEditor::Cursor(selectionRange.start().line(), fromVirtualColumn(selectionRange.start().line(), newSelectionColumn)),
                                KTextEditor::Cursor(selectionRange.end().line(), fromVirtualColumn(selectionRange.end().line(), newSelectionColumn)));
        view->setSelection(selectionRange);
    } else {
        insertText(view->cursorPosition(), chars);
    }

    // auto bracket handling for newly inserted text
    // we inserted a bracket?
    // => add the matching closing one to the view + input chars
    // try to preserve the cursor position
    bool skipAutobrace = closingBracket == QLatin1Char('\'');
    if (highlight() && skipAutobrace) {
        // skip adding ' in spellchecked areas, because those are text
        skipAutobrace = highlight()->spellCheckingRequiredForLocation(this, view->cursorPosition() - Cursor{0, 1});
    }

    const auto cursorPos(view->cursorPosition());
    if (!skipAutobrace && (closingBracket == QLatin1Char('\''))) {
        // skip auto quotes when these looks already balanced, bug 405089
        const QString lineText = m_buffer->lineText(cursorPos.line());
        // RegEx match quote, but not escaped quote, thanks to https://stackoverflow.com/a/11819111
        static const QRegularExpression re(QStringLiteral("(?<!\\\\)(?:\\\\\\\\)*\\\'"));
        const int count = lineText.left(cursorPos.column()).count(re);
        skipAutobrace = (count % 2 == 0) ? true : false;
    }
    if (!skipAutobrace && (closingBracket == QLatin1Char('\"'))) {
        // ...same trick for double quotes
        const QString lineText = m_buffer->lineText(cursorPos.line());
        static const QRegularExpression re(QStringLiteral("(?<!\\\\)(?:\\\\\\\\)*\\\""));
        const int count = lineText.left(cursorPos.column()).count(re);
        skipAutobrace = (count % 2 == 0) ? true : false;
    }

    if (!closingBracket.isNull() && !skipAutobrace) {
        // add bracket to the view
        const auto nextChar = view->document()->text({cursorPos, cursorPos + Cursor{0, 1}}).trimmed();
        if (nextChar.isEmpty() || !nextChar.at(0).isLetterOrNumber()) {
            insertText(view->cursorPosition(), QString(closingBracket));
            const auto insertedAt(view->cursorPosition());
            view->setCursorPosition(cursorPos);
            m_currentAutobraceRange.reset(newMovingRange({cursorPos - Cursor{0, 1}, insertedAt}, KTextEditor::MovingRange::DoNotExpand));
            connect(view, &View::cursorPositionChanged, this, &DocumentPrivate::checkCursorForAutobrace, Qt::UniqueConnection);

            // add bracket to chars inserted! needed for correct signals + indent
            chars.append(closingBracket);
        }
        m_currentAutobraceClosingChar = closingBracket;
    }

    // end edit session here, to have updated HL in userTypedChar!
    editEnd();

    // trigger indentation
    KTextEditor::Cursor b(view->cursorPosition());
    m_indenter->userTypedChar(view, b, chars.isEmpty() ? QChar() : chars.at(chars.length() - 1));

    // inform the view about the original inserted chars
    view->slotTextInserted(view, oldCur, chars);
}

void KTextEditor::DocumentPrivate::checkCursorForAutobrace(KTextEditor::View *, const KTextEditor::Cursor newPos)
{
    if (m_currentAutobraceRange && !m_currentAutobraceRange->toRange().contains(newPos)) {
        m_currentAutobraceRange.reset();
    }
}

void KTextEditor::DocumentPrivate::newLine(KTextEditor::ViewPrivate *v, KTextEditor::DocumentPrivate::NewLineIndent indent)
{
    editStart();

    if (!v->config()->persistentSelection() && v->selection()) {
        v->removeSelectedText();
        v->clearSelection();
    }

    // query cursor position
    KTextEditor::Cursor c = v->cursorPosition();

    if (c.line() > lastLine()) {
        c.setLine(lastLine());
    }

    if (c.line() < 0) {
        c.setLine(0);
    }

    int ln = c.line();

    int len = lineLength(ln);

    if (c.column() > len) {
        c.setColumn(len);
    }

    // first: wrap line
    editWrapLine(c.line(), c.column());

    // update highlighting to have updated HL in userTypedChar!
    m_buffer->updateHighlighting();

    // second: if "indent" is true, indent the new line, if needed...
    if (indent == KTextEditor::DocumentPrivate::Indent) {
        m_indenter->userTypedChar(v, v->cursorPosition(), QLatin1Char('\n'));
    }

    editEnd();
}

void KTextEditor::DocumentPrivate::transpose(const KTextEditor::Cursor cursor)
{
    const QString lineText = m_buffer->lineText(cursor.line());

    if (lineText.size() < 2) {
        return;
    }

    uint col = cursor.column();

    if (col > 0) {
        col--;
    }

    if ((lineText.length() - col) < 2) {
        return;
    }

    uint line = cursor.line();
    QString s;

    // clever swap code if first character on the line swap right&left
    // otherwise left & right
    s.append(lineText.at(col + 1));
    s.append(lineText.at(col));
    // do the swap

    // do it right, never ever manipulate a textline
    editStart();
    editRemoveText(line, col, 2);
    editInsertText(line, col, s);
    editEnd();
}

void KTextEditor::DocumentPrivate::swapTextRanges(KTextEditor::Range firstWord, KTextEditor::Range secondWord)
{
    Q_ASSERT(firstWord.isValid() && secondWord.isValid());
    Q_ASSERT(!firstWord.overlaps(secondWord));
    // ensure that secondWord comes AFTER firstWord
    if (firstWord.start().column() > secondWord.start().column() || firstWord.start().line() > secondWord.start().line()) {
        const KTextEditor::Range tempRange = firstWord;
        firstWord.setRange(secondWord);
        secondWord.setRange(tempRange);
    }

    const QString tempString = text(secondWord);
    editStart();
    // edit secondWord first as the range might be invalidated after editing firstWord
    replaceText(secondWord, text(firstWord));
    replaceText(firstWord, tempString);
    editEnd();
}

void KTextEditor::DocumentPrivate::backspace(KTextEditor::ViewPrivate *view, const KTextEditor::Cursor c)
{
    if (!view->config()->persistentSelection() && view->selection()) {
        KTextEditor::Range range = view->selectionRange();
        editStart(); // Avoid bad selection in case of undo
        if (view->blockSelection() && view->selection() && range.start().column() > 0 && toVirtualColumn(range.start()) == toVirtualColumn(range.end())) {
            // Remove one character before vertical selection line by expanding the selection
            range.setStart(KTextEditor::Cursor(range.start().line(), range.start().column() - 1));
            view->setSelection(range);
        }
        view->removeSelectedText();
        editEnd();
        return;
    }

    uint col = qMax(c.column(), 0);
    uint line = qMax(c.line(), 0);
    if ((col == 0) && (line == 0)) {
        return;
    }

    const QString lineText = m_buffer->lineText(line);
    // don't forget this check!!!! really!!!!
    if (lineText.isEmpty()) {
        return;
    }

    if (col > 0) {
        bool useNextBlock = false;
        if (config()->backspaceIndents()) {
            // backspace indents: erase to next indent position
            int colX = Kate::TextLineData::toVirtualColumn(lineText, col, config()->tabWidth());
            int pos = Kate::TextLineData::firstChar(lineText);
            if (pos > 0) {
                pos = Kate::TextLineData::toVirtualColumn(lineText, pos, config()->tabWidth());
            }
            if (pos < 0 || pos >= (int)colX) {
                // only spaces on left side of cursor
                indent(KTextEditor::Range(line, 0, line, 0), -1);
            } else {
                useNextBlock = true;
            }
        }
        if (!config()->backspaceIndents() || useNextBlock) {
            KTextEditor::Cursor beginCursor(line, 0);
            KTextEditor::Cursor endCursor(line, col);
            if (!view->config()->backspaceRemoveComposed()) { // Normal backspace behavior
                beginCursor.setColumn(col - 1);
                // move to left of surrogate pair
                if (!isValidTextPosition(beginCursor)) {
                    Q_ASSERT(col >= 2);
                    beginCursor.setColumn(col - 2);
                }
            } else {
                beginCursor.setColumn(view->textLayout(c)->previousCursorPosition(c.column()));
            }
            removeText(KTextEditor::Range(beginCursor, endCursor));
            // in most cases cursor is moved by removeText, but we should do it manually
            // for past-end-of-line cursors in block mode
            view->setCursorPosition(beginCursor);
        }

    } else {
        // col == 0: wrap to previous line
        const QString textLine = m_buffer->lineText(line - 1);

        if (line > 0) {
            if (config()->wordWrap() && textLine.endsWith(QLatin1Char(' '))) {
                // gg: in hard wordwrap mode, backspace must also eat the trailing space
                removeText(KTextEditor::Range(line - 1, textLine.length() - 1, line, 0));
            } else {
                removeText(KTextEditor::Range(line - 1, textLine.length(), line, 0));
            }
        }
    }

    if (m_currentAutobraceRange) {
        const auto r = m_currentAutobraceRange->toRange();
        if (r.columnWidth() == 1 && view->cursorPosition() == r.start()) {
            // start parenthesis removed and range length is 1, remove end as well
            del(view, view->cursorPosition());
            m_currentAutobraceRange.reset();
        }
    }
}

void KTextEditor::DocumentPrivate::del(KTextEditor::ViewPrivate *view, const KTextEditor::Cursor c)
{
    if (!view->config()->persistentSelection() && view->selection()) {
        KTextEditor::Range range = view->selectionRange();
        editStart(); // Avoid bad selection in case of undo
        if (view->blockSelection() && toVirtualColumn(range.start()) == toVirtualColumn(range.end())) {
            // Remove one character after vertical selection line by expanding the selection
            range.setEnd(KTextEditor::Cursor(range.end().line(), range.end().column() + 1));
            view->setSelection(range);
        }
        view->removeSelectedText();
        editEnd();
        return;
    }

    if (c.column() < (int)m_buffer->lineLength(c.line())) {
        KTextEditor::Cursor endCursor(c.line(), view->textLayout(c)->nextCursorPosition(c.column()));
        removeText(KTextEditor::Range(c, endCursor));
    } else if (c.line() < lastLine()) {
        removeText(KTextEditor::Range(c.line(), c.column(), c.line() + 1, 0));
    }
}

void KTextEditor::DocumentPrivate::paste(KTextEditor::ViewPrivate *view, const QString &text)
{
    // nop if nothing to paste
    if (text.isEmpty()) {
        return;
    }

    // normalize line endings, to e.g. catch issues with \r\n in paste buffer
    // see bug 410951
    QString s = text;
    s.replace(QRegularExpression(QStringLiteral("\r\n?")), QStringLiteral("\n"));

    int lines = s.count(QLatin1Char('\n'));
    const bool isSingleLine = lines == 0;

    m_undoManager->undoSafePoint();

    editStart();

    KTextEditor::Cursor pos = view->cursorPosition();

    bool skipIndentOnPaste = false;
    if (isSingleLine) {
        const int length = lineLength(pos.line());
        // if its a single line and the line already contains some text, skip indenting
        skipIndentOnPaste = length > 0;
    }

    if (!view->config()->persistentSelection() && view->selection()) {
        pos = view->selectionRange().start();
        if (view->blockSelection()) {
            pos = rangeOnLine(view->selectionRange(), pos.line()).start();
            if (lines == 0) {
                s += QLatin1Char('\n');
                s = s.repeated(view->selectionRange().numberOfLines() + 1);
                s.chop(1);
            }
        }
        view->removeSelectedText();
    }

    if (config()->ovr()) {
        const auto pasteLines = QStringView(s).split(QLatin1Char('\n'));

        if (!view->blockSelection()) {
            int endColumn = (pasteLines.count() == 1 ? pos.column() : 0) + pasteLines.last().length();
            removeText(KTextEditor::Range(pos, pos.line() + pasteLines.count() - 1, endColumn));
        } else {
            int maxi = qMin(pos.line() + pasteLines.count(), this->lines());

            for (int i = pos.line(); i < maxi; ++i) {
                int pasteLength = pasteLines.at(i - pos.line()).length();
                removeText(KTextEditor::Range(i, pos.column(), i, qMin(pasteLength + pos.column(), lineLength(i))));
            }
        }
    }

    insertText(pos, s, view->blockSelection());
    editEnd();

    // move cursor right for block select, as the user is moved right internal
    // even in that case, but user expects other behavior in block selection
    // mode !
    // just let cursor stay, that was it before I changed to moving ranges!
    if (view->blockSelection()) {
        view->setCursorPositionInternal(pos);
    }

    if (config()->indentPastedText()) {
        KTextEditor::Range range = KTextEditor::Range(KTextEditor::Cursor(pos.line(), 0), KTextEditor::Cursor(pos.line() + lines, 0));
        if (!skipIndentOnPaste) {
            m_indenter->indent(view, range);
        }
    }

    if (!view->blockSelection()) {
        Q_EMIT charactersSemiInteractivelyInserted(pos, s);
    }
    m_undoManager->undoSafePoint();
}

void KTextEditor::DocumentPrivate::indent(KTextEditor::Range range, int change)
{
    if (!isReadWrite()) {
        return;
    }

    editStart();
    m_indenter->changeIndent(range, change);
    editEnd();
}

void KTextEditor::DocumentPrivate::align(KTextEditor::ViewPrivate *view, KTextEditor::Range range)
{
    m_indenter->indent(view, range);
}

void KTextEditor::DocumentPrivate::insertTab(KTextEditor::ViewPrivate *view, const KTextEditor::Cursor)
{
    if (!isReadWrite()) {
        return;
    }

    int lineLen = line(view->cursorPosition().line()).length();
    KTextEditor::Cursor c = view->cursorPosition();

    editStart();

    if (!view->config()->persistentSelection() && view->selection()) {
        view->removeSelectedText();
    } else if (view->currentInputMode()->overwrite() && c.column() < lineLen) {
        KTextEditor::Range r = KTextEditor::Range(view->cursorPosition(), 1);

        // replace mode needs to know what was removed so it can be restored with backspace
        QChar removed = line(view->cursorPosition().line()).at(r.start().column());
        view->currentInputMode()->overwrittenChar(removed);
        removeText(r);
    }

    c = view->cursorPosition();
    editInsertText(c.line(), c.column(), QStringLiteral("\t"));

    editEnd();
}

/*
  Remove a given string at the beginning
  of the current line.
*/
bool KTextEditor::DocumentPrivate::removeStringFromBeginning(int line, const QString &str)
{
    const QString textline = m_buffer->lineText(line);

    KTextEditor::Cursor cursor(line, 0);
    bool there = textline.startsWith(str);

    if (!there) {
        cursor.setColumn(Kate::TextLineData::firstChar(textline));
        there = Kate::TextLineData::matchesAt(textline, cursor.column(), str);
    }

    if (there) {
        // Remove some chars
        removeText(KTextEditor::Range(cursor, str.length()));
    }

    return there;
}

/*
  Remove a given string at the end
  of the current line.
*/
bool KTextEditor::DocumentPrivate::removeStringFromEnd(int line, const QString &str)
{
    const QString textline = m_buffer->lineText(line);

    KTextEditor::Cursor cursor(line, 0);
    bool there = textline.endsWith(str);

    if (there) {
        cursor.setColumn(textline.length() - str.length());
    } else {
        cursor.setColumn(Kate::TextLineData::lastChar(textline) - str.length() + 1);
        there = Kate::TextLineData::matchesAt(textline, cursor.column(), str);
    }

    if (there) {
        // Remove some chars
        removeText(KTextEditor::Range(cursor, str.length()));
    }

    return there;
}

/*
  Replace tabs by spaces in the given string, if enabled.
 */
QString KTextEditor::DocumentPrivate::eventuallyReplaceTabs(const KTextEditor::Cursor cursorPos, const QString &str) const
{
    const bool replacetabs = config()->replaceTabsDyn();
    if (!replacetabs) {
        return str;
    }
    const int indentWidth = config()->indentationWidth();
    static const QLatin1Char tabChar('\t');

    int column = cursorPos.column();

    // The result will always be at least as long as the input
    QString result;
    result.reserve(str.size());

    for (const QChar ch : str) {
        if (ch == tabChar) {
            // Insert only enough spaces to align to the next indentWidth column
            // This fixes bug #340212
            int spacesToInsert = indentWidth - (column % indentWidth);
            result += QString(spacesToInsert, QLatin1Char(' '));
            column += spacesToInsert;
        } else {
            // Just keep all other typed characters as-is
            result += ch;
            ++column;
        }
    }
    return result;
}

/*
  Add to the current line a comment line mark at the beginning.
*/
void KTextEditor::DocumentPrivate::addStartLineCommentToSingleLine(int line, int attrib)
{
    QString commentLineMark = highlight()->getCommentSingleLineStart(attrib);
    int pos = -1;

    if (highlight()->getCommentSingleLinePosition(attrib) == KSyntaxHighlighting::CommentPosition::StartOfLine) {
        pos = 0;
        commentLineMark += QLatin1Char(' ');
    } else {
        const QString l = kateTextLine(line);
        pos = Kate::TextLineData::firstChar(l);
    }

    if (pos >= 0) {
        insertText(KTextEditor::Cursor(line, pos), commentLineMark);
    }
}

/*
  Remove from the current line a comment line mark at
  the beginning if there is one.
*/
bool KTextEditor::DocumentPrivate::removeStartLineCommentFromSingleLine(int line, int attrib)
{
    const QString shortCommentMark = highlight()->getCommentSingleLineStart(attrib);
    const QString longCommentMark = shortCommentMark + QLatin1Char(' ');

    editStart();

    // Try to remove the long comment mark first
    bool removed = (removeStringFromBeginning(line, longCommentMark) || removeStringFromBeginning(line, shortCommentMark));

    editEnd();

    return removed;
}

/*
  Add to the current line a start comment mark at the
  beginning and a stop comment mark at the end.
*/
void KTextEditor::DocumentPrivate::addStartStopCommentToSingleLine(int line, int attrib)
{
    const QString startCommentMark = highlight()->getCommentStart(attrib) + QLatin1Char(' ');
    const QString stopCommentMark = QLatin1Char(' ') + highlight()->getCommentEnd(attrib);

    editStart();

    // Add the start comment mark
    insertText(KTextEditor::Cursor(line, 0), startCommentMark);

    // Go to the end of the line
    const int col = m_buffer->lineLength(line);

    // Add the stop comment mark
    insertText(KTextEditor::Cursor(line, col), stopCommentMark);

    editEnd();
}

/*
  Remove from the current line a start comment mark at
  the beginning and a stop comment mark at the end.
*/
bool KTextEditor::DocumentPrivate::removeStartStopCommentFromSingleLine(int line, int attrib)
{
    const QString shortStartCommentMark = highlight()->getCommentStart(attrib);
    const QString longStartCommentMark = shortStartCommentMark + QLatin1Char(' ');
    const QString shortStopCommentMark = highlight()->getCommentEnd(attrib);
    const QString longStopCommentMark = QLatin1Char(' ') + shortStopCommentMark;

    editStart();

    // Try to remove the long start comment mark first
    const bool removedStart = (removeStringFromBeginning(line, longStartCommentMark) || removeStringFromBeginning(line, shortStartCommentMark));

    // Try to remove the long stop comment mark first
    const bool removedStop = removedStart && (removeStringFromEnd(line, longStopCommentMark) || removeStringFromEnd(line, shortStopCommentMark));

    editEnd();

    return (removedStart || removedStop);
}

/*
  Add to the current selection a start comment mark at the beginning
  and a stop comment mark at the end.
*/
void KTextEditor::DocumentPrivate::addStartStopCommentToSelection(KTextEditor::ViewPrivate *view, int attrib)
{
    const QString startComment = highlight()->getCommentStart(attrib);
    const QString endComment = highlight()->getCommentEnd(attrib);

    KTextEditor::Range range = view->selectionRange();

    if ((range.end().column() == 0) && (range.end().line() > 0)) {
        range.setEnd(KTextEditor::Cursor(range.end().line() - 1, lineLength(range.end().line() - 1)));
    }

    editStart();

    if (!view->blockSelection()) {
        insertText(range.end(), endComment);
        insertText(range.start(), startComment);
    } else {
        for (int line = range.start().line(); line <= range.end().line(); line++) {
            KTextEditor::Range subRange = rangeOnLine(range, line);
            insertText(subRange.end(), endComment);
            insertText(subRange.start(), startComment);
        }
    }

    editEnd();
    // selection automatically updated (MovingRange)
}

/*
  Add to the current selection a comment line mark at the beginning of each line.
*/
void KTextEditor::DocumentPrivate::addStartLineCommentToSelection(KTextEditor::ViewPrivate *view, int attrib)
{
    // const QString commentLineMark = highlight()->getCommentSingleLineStart(attrib) + QLatin1Char(' ');

    int sl = view->selectionRange().start().line();
    int el = view->selectionRange().end().line();

    // if end of selection is in column 0 in last line, omit the last line
    if ((view->selectionRange().end().column() == 0) && (el > 0)) {
        el--;
    }

    editStart();

    // For each line of the selection
    for (int z = el; z >= sl; z--) {
        // insertText (z, 0, commentLineMark);
        addStartLineCommentToSingleLine(z, attrib);
    }

    editEnd();
    // selection automatically updated (MovingRange)
}

bool KTextEditor::DocumentPrivate::nextNonSpaceCharPos(int &line, int &col)
{
    for (; line < (int)m_buffer->count(); line++) {
        const QString lineText = plainKateTextLine(line);
        if (lineText.isEmpty()) {
            break;
        }
        col = Kate::TextLineData::nextNonSpaceChar(lineText, col);
        if (col != -1) {
            return true; // Next non-space char found
        }
        col = 0;
    }
    // No non-space char found
    line = -1;
    col = -1;
    return false;
}

bool KTextEditor::DocumentPrivate::previousNonSpaceCharPos(int &line, int &col)
{
    while (true) {
        const QString textLine = m_buffer->lineText(line);
        if (textLine.isEmpty()) {
            break;
        }

        col = Kate::TextLineData::previousNonSpaceChar(textLine, col);
        if (col != -1) {
            return true;
        }
        if (line == 0) {
            return false;
        }
        --line;
        col = textLine.length();
    }
    // No non-space char found
    line = -1;
    col = -1;
    return false;
}

/*
  Remove from the selection a start comment mark at
  the beginning and a stop comment mark at the end.
*/
bool KTextEditor::DocumentPrivate::removeStartStopCommentFromSelection(KTextEditor::ViewPrivate *view, int attrib)
{
    const QString startComment = highlight()->getCommentStart(attrib);
    const QString endComment = highlight()->getCommentEnd(attrib);

    int sl = qMax<int>(0, view->selectionRange().start().line());
    int el = qMin<int>(view->selectionRange().end().line(), lastLine());
    int sc = view->selectionRange().start().column();
    int ec = view->selectionRange().end().column();

    // The selection ends on the char before selectEnd
    if (ec != 0) {
        --ec;
    } else if (el > 0) {
        --el;
        ec = m_buffer->lineLength(el) - 1;
    }

    const int startCommentLen = startComment.length();
    const int endCommentLen = endComment.length();

    // had this been perl or sed: s/^\s*$startComment(.+?)$endComment\s*/$2/
    const QString startLine = plainKateTextLine(sl);
    const QString endLine = plainKateTextLine(el);

    bool remove = nextNonSpaceCharPos(sl, sc) && Kate::TextLineData::matchesAt(startLine, sc, startComment) && previousNonSpaceCharPos(el, ec)
        && ((ec - endCommentLen + 1) >= 0) && Kate::TextLineData::matchesAt(endLine, ec - endCommentLen + 1, endComment);

    if (remove) {
        editStart();

        removeText(KTextEditor::Range(el, ec - endCommentLen + 1, el, ec + 1));
        removeText(KTextEditor::Range(sl, sc, sl, sc + startCommentLen));

        editEnd();
        // selection automatically updated (MovingRange)
    }

    return remove;
}

bool KTextEditor::DocumentPrivate::removeStartStopCommentFromRegion(const KTextEditor::Cursor start, const KTextEditor::Cursor end, int attrib)
{
    const QString startComment = highlight()->getCommentStart(attrib);
    const QString endComment = highlight()->getCommentEnd(attrib);
    const int startCommentLen = startComment.length();
    const int endCommentLen = endComment.length();

    const QString startLine = plainKateTextLine(start.line());
    const QString endLine = plainKateTextLine(end.line());

    const bool remove = Kate::TextLineData::matchesAt(startLine, start.column(), startComment)
        && Kate::TextLineData::matchesAt(endLine, end.column() - endCommentLen, endComment);
    if (remove) {
        editStart();
        removeText(KTextEditor::Range(end.line(), end.column() - endCommentLen, end.line(), end.column()));
        removeText(KTextEditor::Range(start, startCommentLen));
        editEnd();
    }
    return remove;
}

/*
  Remove from the beginning of each line of the
  selection a start comment line mark.
*/
bool KTextEditor::DocumentPrivate::removeStartLineCommentFromSelection(KTextEditor::ViewPrivate *view, int attrib, bool toggleComment)
{
    const QString shortCommentMark = highlight()->getCommentSingleLineStart(attrib);
    const QString longCommentMark = shortCommentMark + QLatin1Char(' ');

    int startLine = view->selectionRange().start().line();
    int endLine = view->selectionRange().end().line();

    if ((view->selectionRange().end().column() == 0) && (endLine > 0)) {
        endLine--;
    }

    bool removed = false;

    // If we are toggling, we check whether all lines in the selection start
    // with a comment. If they don't, we return early
    // NOTE: When toggling, we only remove comments if all lines in the selection
    // are comments, otherwise we recomment the comments
    if (toggleComment) {
        bool allLinesAreCommented = true;
        for (int line = endLine; line >= startLine; line--) {
            const QString ln = plainKateTextLine(line);
            if (!ln.startsWith(shortCommentMark) && !ln.startsWith(longCommentMark)) {
                allLinesAreCommented = false;
                break;
            }
        }
        if (!allLinesAreCommented) {
            return false;
        }
    }

    editStart();

    // For each line of the selection
    for (int z = endLine; z >= startLine; z--) {
        // Try to remove the long comment mark first
        removed = (removeStringFromBeginning(z, longCommentMark) || removeStringFromBeginning(z, shortCommentMark) || removed);
    }

    editEnd();
    // selection automatically updated (MovingRange)

    return removed;
}

/*
  Comment or uncomment the selection or the current
  line if there is no selection.
*/
void KTextEditor::DocumentPrivate::comment(KTextEditor::ViewPrivate *v, uint line, uint column, int change)
{
    // skip word wrap bug #105373
    const bool skipWordWrap = wordWrap();
    if (skipWordWrap) {
        setWordWrap(false);
    }

    const bool hasSelection = v->selection();
    int selectionCol = 0;

    if (hasSelection) {
        selectionCol = v->selectionRange().start().column();
    }

    int startAttrib = 0;

    if (selectionCol < lineLength(line)) {
        startAttrib = m_buffer->attributeInLine(line, selectionCol);
    } else if (!m_buffer->lineAttributes(line).isEmpty()) {
        startAttrib = m_buffer->lineAttributes(line).back().attributeValue;
    }

    bool hasStartLineCommentMark = !(highlight()->getCommentSingleLineStart(startAttrib).isEmpty());
    bool hasStartStopCommentMark = (!(highlight()->getCommentStart(startAttrib).isEmpty()) && !(highlight()->getCommentEnd(startAttrib).isEmpty()));

    if (change > 0) { // comment
        if (!hasSelection) {
            if (hasStartLineCommentMark) {
                addStartLineCommentToSingleLine(line, startAttrib);
            } else if (hasStartStopCommentMark) {
                addStartStopCommentToSingleLine(line, startAttrib);
            }
        } else {
            // anders: prefer single line comment to avoid nesting probs
            // If the selection starts after first char in the first line
            // or ends before the last char of the last line, we may use
            // multiline comment markers.
            // TODO We should try to detect nesting.
            //    - if selection ends at col 0, most likely she wanted that
            // line ignored
            const KTextEditor::Range sel = v->selectionRange();
            if (hasStartStopCommentMark
                && (!hasStartLineCommentMark
                    || ((sel.start().column() > Kate::TextLineData::firstChar(plainKateTextLine(sel.start().line())))
                        || (sel.end().column() > 0 && sel.end().column() < (lineLength(sel.end().line())))))) {
                addStartStopCommentToSelection(v, startAttrib);
            } else if (hasStartLineCommentMark) {
                addStartLineCommentToSelection(v, startAttrib);
            }
        }
    } else { // uncomment
        bool removed = false;
        const bool toggleComment = change == 0;
        if (!hasSelection) {
            removed = (hasStartLineCommentMark && removeStartLineCommentFromSingleLine(line, startAttrib))
                || (hasStartStopCommentMark && removeStartStopCommentFromSingleLine(line, startAttrib));
        } else {
            // anders: this seems like it will work with above changes :)
            removed = (hasStartStopCommentMark && removeStartStopCommentFromSelection(v, startAttrib))
                || (hasStartLineCommentMark && removeStartLineCommentFromSelection(v, startAttrib, toggleComment));
        }

        // recursive call for toggle comment
        if (!removed && toggleComment) {
            comment(v, line, column, 1);
        }
    }

    if (skipWordWrap) {
        setWordWrap(true); // see begin of function ::comment (bug #105373)
    }
}

void KTextEditor::DocumentPrivate::transform(KTextEditor::ViewPrivate *v, const KTextEditor::Cursor c, KTextEditor::DocumentPrivate::TextTransform t)
{
    if (v->selection()) {
        editStart();

        // cache the selection and cursor, so we can be sure to restore.
        KTextEditor::Range selection = v->selectionRange();

        KTextEditor::Range range(selection.start(), 0);
        while (range.start().line() <= selection.end().line()) {
            int start = 0;
            int end = lineLength(range.start().line());

            if (range.start().line() == selection.start().line() || v->blockSelection()) {
                start = selection.start().column();
            }

            if (range.start().line() == selection.end().line() || v->blockSelection()) {
                end = selection.end().column();
            }

            if (start > end) {
                int swapCol = start;
                start = end;
                end = swapCol;
            }
            range.setStart(KTextEditor::Cursor(range.start().line(), start));
            range.setEnd(KTextEditor::Cursor(range.end().line(), end));

            QString s = text(range);
            QString old = s;

            if (t == Uppercase) {
                s = s.toUpper();
            } else if (t == Lowercase) {
                s = s.toLower();
            } else { // Capitalize
                const QString l = plainKateTextLine(range.start().line());
                int p(0);
                while (p < s.length()) {
                    // If bol or the character before is not in a word, up this one:
                    // 1. if both start and p is 0, upper char.
                    // 2. if blockselect or first line, and p == 0 and start-1 is not in a word, upper
                    // 3. if p-1 is not in a word, upper.
                    if ((!range.start().column() && !p)
                        || ((range.start().line() == selection.start().line() || v->blockSelection()) && !p
                            && !highlight()->isInWord(l.at(range.start().column() - 1)))
                        || (p && !highlight()->isInWord(s.at(p - 1)))) {
                        s[p] = s.at(p).toUpper();
                    }
                    p++;
                }
            }

            if (s != old) {
                removeText(range);
                insertText(range.start(), s);
            }

            range.setBothLines(range.start().line() + 1);
        }

        editEnd();

        // restore selection & cursor
        v->setSelection(selection);
        v->setCursorPosition(c);

    } else { // no selection
        editStart();

        // get cursor
        KTextEditor::Cursor cursor = c;

        QString old = text(KTextEditor::Range(cursor, 1));
        QString s;
        switch (t) {
        case Uppercase:
            s = old.toUpper();
            break;
        case Lowercase:
            s = old.toLower();
            break;
        case Capitalize: {
            const QString l = plainKateTextLine(cursor.line());
            while (cursor.column() > 0 && highlight()->isInWord(l.at(cursor.column() - 1), m_buffer->attributeInLine(cursor.line(), cursor.column() - 1))) {
                cursor.setColumn(cursor.column() - 1);
            }
            old = text(KTextEditor::Range(cursor, 1));
            s = old.toUpper();
        } break;
        default:
            break;
        }

        removeText(KTextEditor::Range(cursor, 1));
        insertText(cursor, s);

        editEnd();
    }
}

void KTextEditor::DocumentPrivate::joinLines(uint first, uint last)
{
    //   if ( first == last ) last += 1;
    editStart();
    int line(first);
    while (first < last) {
        // Normalize the whitespace in the joined lines by making sure there's
        // always exactly one space between the joined lines
        // This cannot be done in editUnwrapLine, because we do NOT want this
        // behavior when deleting from the start of a line, just when explicitly
        // calling the join command
        const QString l = kateTextLine(line);
        const QString tl = kateTextLine(line + 1);

        if (l.isEmpty() || tl.isEmpty()) {
            editEnd();
            return;
        }

        int pos = Kate::TextLineData::firstChar(tl);
        if (pos >= 0) {
            if (pos != 0) {
                editRemoveText(line + 1, 0, pos);
            }
            if (!(l.length() == 0 || l.at(l.length() - 1).isSpace())) {
                editInsertText(line + 1, 0, QStringLiteral(" "));
            }
        } else {
            // Just remove the whitespace and let Kate handle the rest
            editRemoveText(line + 1, 0, tl.length());
        }

        editUnWrapLine(line);
        first++;
    }
    editEnd();
}

void KTextEditor::DocumentPrivate::tagLines(KTextEditor::LineRange lineRange)
{
    for (auto view : std::as_const(m_views)) {
        view->tagLines(lineRange, true);
    }
}

void KTextEditor::DocumentPrivate::tagLine(int line)
{
    tagLines({line, line});
}

void KTextEditor::DocumentPrivate::repaintViews(bool paintOnlyDirty)
{
    for (auto view : std::as_const(m_views)) {
        view->repaintText(paintOnlyDirty);
    }
}

/*
   Bracket matching uses the following algorithm:
   If in overwrite mode, match the bracket currently underneath the cursor.
   Otherwise, if the character to the left is a bracket,
   match it. Otherwise if the character to the right of the cursor is a
   bracket, match it. Otherwise, don't match anything.
*/
KTextEditor::Range KTextEditor::DocumentPrivate::findMatchingBracket(const KTextEditor::Cursor start, int maxLines)
{
    if (maxLines < 0) {
        return KTextEditor::Range::invalid();
    }

    const QString textLine = plainKateTextLine(start.line());
    if (textLine.isEmpty()) {
        return KTextEditor::Range::invalid();
    }

    auto inRange = [&textLine](int pos) {
        return pos >= 0 && pos < textLine.size();
    };

    KTextEditor::Range range(start, start);
    const QChar right = inRange(range.start().column()) ? textLine.at(range.start().column()) : QChar();
    const QChar left = inRange(range.start().column() - 1) ? textLine.at(range.start().column() - 1) : QChar();
    QChar bracket;

    if (config()->ovr()) {
        if (isBracket(right)) {
            bracket = right;
        } else {
            return KTextEditor::Range::invalid();
        }
    } else if (isBracket(right)) {
        bracket = right;
    } else if (isBracket(left)) {
        range.setStart(KTextEditor::Cursor(range.start().line(), range.start().column() - 1));
        bracket = left;
    } else {
        return KTextEditor::Range::invalid();
    }

    const QChar opposite = matchingBracket(bracket);
    if (opposite.isNull()) {
        return KTextEditor::Range::invalid();
    }

    const int searchDir = isStartBracket(bracket) ? 1 : -1;
    uint nesting = 0;

    const int minLine = qMax(range.start().line() - maxLines, 0);
    const int maxLine = qMin(range.start().line() + maxLines, documentEnd().line());

    range.setEnd(range.start());
    KTextEditor::DocumentCursor cursor(this);
    cursor.setPosition(range.start());
    int validAttr = m_buffer->attributeInLine(cursor.line(), cursor.column());
    //     int validAttr = kateTextLine(cursor.line())->attribute(cursor.column());

    while (cursor.line() >= minLine && cursor.line() <= maxLine) {
        if (!cursor.move(searchDir)) {
            return KTextEditor::Range::invalid();
        }

        if (m_buffer->attributeInLine(cursor.line(), cursor.column()) == validAttr) {
            // Check for match
            QChar c = line(cursor.line()).at(cursor.column());
            if (c == opposite) {
                if (nesting == 0) {
                    if (searchDir > 0) { // forward
                        range.setEnd(cursor.toCursor());
                    } else {
                        range.setStart(cursor.toCursor());
                    }
                    return range;
                }
                nesting--;
            } else if (c == bracket) {
                nesting++;
            }
        }
    }

    return KTextEditor::Range::invalid();
}

// helper: remove \r and \n from visible document name (bug #170876)
inline static QString removeNewLines(const QString &str)
{
    QString tmp(str);
    return tmp.replace(QLatin1String("\r\n"), QLatin1String(" ")).replace(QLatin1Char('\r'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
}

void KTextEditor::DocumentPrivate::updateDocName()
{
    // if the name is set, and starts with FILENAME, it should not be changed!
    if (!url().isEmpty() && (m_docName == removeNewLines(url().fileName()) || m_docName.startsWith(removeNewLines(url().fileName()) + QLatin1String(" (")))) {
        return;
    }

    int count = -1;

    std::vector<KTextEditor::DocumentPrivate *> docsWithSameName;

    const auto docs = KTextEditor::EditorPrivate::self()->kateDocuments();
    for (KTextEditor::DocumentPrivate *doc : docs) {
        if ((doc != this) && (doc->url().fileName() == url().fileName())) {
            if (doc->m_docNameNumber > count) {
                count = doc->m_docNameNumber;
            }
            docsWithSameName.push_back(doc);
        }
    }

    m_docNameNumber = count + 1;

    QString oldName = m_docName;
    m_docName = removeNewLines(url().fileName());

    m_isUntitled = m_docName.isEmpty();

    if (!m_isUntitled && !docsWithSameName.empty()) {
        docsWithSameName.push_back(this);
        uniquifyDocNames(docsWithSameName);
        return;
    }

    if (m_isUntitled) {
        m_docName = i18n("Untitled");
    }

    if (m_docNameNumber > 0) {
        m_docName = QString(m_docName + QLatin1String(" (%1)")).arg(m_docNameNumber + 1);
    }

    // avoid to emit this, if name doesn't change!
    if (oldName != m_docName) {
        Q_EMIT documentNameChanged(this);
    }
}

/**
 * Find the shortest prefix for doc from urls
 * @p urls contains a list of urls
 *  - /path/to/some/file
 *  - /some/to/path/file
 *
 * we find the shortest path prefix which can be used to
 * identify @p doc
 *
 * for above, it will return "some" for first and "path" for second
 */
static QString shortestPrefix(const std::vector<QString> &urls, KTextEditor::DocumentPrivate *doc)
{
    const auto url = doc->url().toString(QUrl::NormalizePathSegments | QUrl::PreferLocalFile);
    int lastSlash = url.lastIndexOf(QLatin1Char('/'));
    if (lastSlash == -1) {
        // just filename?
        return url;
    }
    int fileNameStart = lastSlash;

    lastSlash--;
    lastSlash = url.lastIndexOf(QLatin1Char('/'), lastSlash);
    if (lastSlash == -1) {
        // already too short?
        lastSlash = 0;
        return url.mid(lastSlash, fileNameStart);
    }

    QStringView urlView = url;
    QStringView urlv = url;
    urlv = urlv.mid(lastSlash);

    for (size_t i = 0; i < urls.size(); ++i) {
        if (urls[i] == url) {
            continue;
        }

        if (urls[i].endsWith(urlv)) {
            lastSlash = url.lastIndexOf(QLatin1Char('/'), lastSlash - 1);
            if (lastSlash == -1) {
                // reached end
                return url.mid(0, fileNameStart);
            }
            // else update urlv and match again from start
            urlv = urlView.mid(lastSlash);
            i = -1;
        }
    }

    return url.mid(lastSlash + 1, fileNameStart - (lastSlash + 1));
}

void KTextEditor::DocumentPrivate::uniquifyDocNames(const std::vector<KTextEditor::DocumentPrivate *> &docs)
{
    std::vector<QString> paths;
    paths.reserve(docs.size());
    std::transform(docs.begin(), docs.end(), std::back_inserter(paths), [](const KTextEditor::DocumentPrivate *d) {
        return d->url().toString(QUrl::NormalizePathSegments | QUrl::PreferLocalFile);
    });

    for (const auto doc : docs) {
        const QString prefix = shortestPrefix(paths, doc);
        const QString fileName = doc->url().fileName();
        const QString oldName = doc->m_docName;

        if (!prefix.isEmpty()) {
            doc->m_docName = fileName + QStringLiteral(" - ") + prefix;
        } else {
            doc->m_docName = fileName;
        }

        if (doc->m_docName != oldName) {
            Q_EMIT doc->documentNameChanged(doc);
        }
    }
}

void KTextEditor::DocumentPrivate::slotModifiedOnDisk(KTextEditor::View * /*v*/)
{
    if (url().isEmpty() || !m_modOnHd) {
        return;
    }

    if (!isModified() && isAutoReload()) {
        onModOnHdAutoReload();
        return;
    }

    if (!m_fileChangedDialogsActivated || m_modOnHdHandler) {
        return;
    }

    // don't ask the user again and again the same thing
    if (m_modOnHdReason == m_prevModOnHdReason) {
        return;
    }
    m_prevModOnHdReason = m_modOnHdReason;

    m_modOnHdHandler = new KateModOnHdPrompt(this, m_modOnHdReason, reasonedMOHString());
    connect(m_modOnHdHandler.data(), &KateModOnHdPrompt::saveAsTriggered, this, &DocumentPrivate::onModOnHdSaveAs);
    connect(m_modOnHdHandler.data(), &KateModOnHdPrompt::closeTriggered, this, &DocumentPrivate::onModOnHdClose);
    connect(m_modOnHdHandler.data(), &KateModOnHdPrompt::reloadTriggered, this, &DocumentPrivate::onModOnHdReload);
    connect(m_modOnHdHandler.data(), &KateModOnHdPrompt::autoReloadTriggered, this, &DocumentPrivate::onModOnHdAutoReload);
    connect(m_modOnHdHandler.data(), &KateModOnHdPrompt::ignoreTriggered, this, &DocumentPrivate::onModOnHdIgnore);
}

void KTextEditor::DocumentPrivate::onModOnHdSaveAs()
{
    m_modOnHd = false;
    const QUrl res = getSaveFileUrl(i18n("Save File"));
    if (!res.isEmpty()) {
        if (!saveAs(res)) {
            KMessageBox::error(dialogParent(), i18n("Save failed"));
            m_modOnHd = true;
        } else {
            delete m_modOnHdHandler;
            m_prevModOnHdReason = OnDiskUnmodified;
            Q_EMIT modifiedOnDisk(this, false, OnDiskUnmodified);
        }
    } else { // the save as dialog was canceled, we are still modified on disk
        m_modOnHd = true;
    }
}

void KTextEditor::DocumentPrivate::onModOnHdClose()
{
    // avoid prompt in closeUrl()
    m_fileChangedDialogsActivated = false;

    // close the file without prompt confirmation
    closeUrl();

    // Useful for applications that provide the necessary interface
    // delay this, otherwise we delete ourself during e.g. event handling + deleting this is undefined!
    // see e.g. bug 433180
    QTimer::singleShot(0, this, [this]() {
        KTextEditor::EditorPrivate::self()->application()->closeDocument(this);
    });
}

void KTextEditor::DocumentPrivate::onModOnHdReload()
{
    m_modOnHd = false;
    m_prevModOnHdReason = OnDiskUnmodified;
    Q_EMIT modifiedOnDisk(this, false, OnDiskUnmodified);

    // MUST Clear Undo/Redo here because by the time we get here
    // the checksum has already been updated and the undo manager
    // sees the new checksum and thinks nothing changed and loads
    // a bad undo history resulting in funny things.
    m_undoManager->clearUndo();
    m_undoManager->clearRedo();

    documentReload();
    delete m_modOnHdHandler;
}

void KTextEditor::DocumentPrivate::autoReloadToggled(bool b)
{
    m_autoReloadMode->setChecked(b);
    if (b) {
        connect(&m_modOnHdTimer, &QTimer::timeout, this, &DocumentPrivate::onModOnHdAutoReload);
    } else {
        disconnect(&m_modOnHdTimer, &QTimer::timeout, this, &DocumentPrivate::onModOnHdAutoReload);
    }
}

bool KTextEditor::DocumentPrivate::isAutoReload()
{
    return m_autoReloadMode->isChecked();
}

void KTextEditor::DocumentPrivate::delayAutoReload()
{
    if (isAutoReload()) {
        m_autoReloadThrottle.start();
    }
}

void KTextEditor::DocumentPrivate::onModOnHdAutoReload()
{
    if (m_modOnHdHandler) {
        delete m_modOnHdHandler;
        autoReloadToggled(true);
    }

    if (!isAutoReload()) {
        return;
    }

    if (m_modOnHd && !m_reloading && !m_autoReloadThrottle.isActive()) {
        m_modOnHd = false;
        m_prevModOnHdReason = OnDiskUnmodified;
        Q_EMIT modifiedOnDisk(this, false, OnDiskUnmodified);

        // MUST clear undo/redo. This comes way after KDirWatch signaled us
        // and the checksum is already updated by the time we start reload.
        m_undoManager->clearUndo();
        m_undoManager->clearRedo();

        documentReload();
        m_autoReloadThrottle.start();
    }
}

void KTextEditor::DocumentPrivate::onModOnHdIgnore()
{
    // ignore as long as m_prevModOnHdReason == m_modOnHdReason
    delete m_modOnHdHandler;
}

void KTextEditor::DocumentPrivate::setModifiedOnDisk(ModifiedOnDiskReason reason)
{
    m_modOnHdReason = reason;
    m_modOnHd = (reason != OnDiskUnmodified);
    Q_EMIT modifiedOnDisk(this, (reason != OnDiskUnmodified), reason);
}

class KateDocumentTmpMark
{
public:
    QString line;
    KTextEditor::Mark mark;
};

void KTextEditor::DocumentPrivate::setModifiedOnDiskWarning(bool on)
{
    m_fileChangedDialogsActivated = on;
}

bool KTextEditor::DocumentPrivate::documentReload()
{
    if (url().isEmpty()) {
        return false;
    }

    // typically, the message for externally modified files is visible. Since it
    // does not make sense showing an additional dialog, just hide the message.
    delete m_modOnHdHandler;

    Q_EMIT aboutToReload(this);

    QVarLengthArray<KateDocumentTmpMark> tmp;
    tmp.reserve(m_marks.size());
    std::transform(m_marks.cbegin(), m_marks.cend(), std::back_inserter(tmp), [this](KTextEditor::Mark *mark) {
        return KateDocumentTmpMark{line(mark->line), *mark};
    });

    const QString oldMode = mode();
    const bool modeByUser = m_fileTypeSetByUser;
    const QString oldHlMode = highlightingMode();
    const bool hlByUser = m_hlSetByUser;

    m_storedVariables.clear();

    // save cursor positions for all views
    QVarLengthArray<std::pair<KTextEditor::ViewPrivate *, KTextEditor::Cursor>, 4> cursorPositions;
    std::transform(m_views.cbegin(), m_views.cend(), std::back_inserter(cursorPositions), [](KTextEditor::ViewPrivate *v) {
        return std::pair<KTextEditor::ViewPrivate *, KTextEditor::Cursor>(v, v->cursorPosition());
    });

    m_reloading = true;
    KTextEditor::DocumentPrivate::openUrl(url());

    // reset some flags only valid for one reload!
    m_userSetEncodingForNextReload = false;

    // restore cursor positions for all views
    for (auto v : std::as_const(m_views)) {
        setActiveView(v);
        auto it = std::find_if(cursorPositions.cbegin(), cursorPositions.cend(), [v](const std::pair<KTextEditor::ViewPrivate *, KTextEditor::Cursor> &p) {
            return p.first == v;
        });
        v->setCursorPosition(it->second);
        if (v->isVisible()) {
            v->repaint();
        }
    }

    int z = 0;
    const int lines = this->lines();
    for (const auto &tmpMark : tmp) {
        if (z < lines) {
            if (tmpMark.line == line(tmpMark.mark.line)) {
                setMark(tmpMark.mark.line, tmpMark.mark.type);
            }
        }
        ++z;
    }

    if (modeByUser) {
        setMode(oldMode);
    }
    if (hlByUser) {
        setHighlightingMode(oldHlMode);
    }

    Q_EMIT reloaded(this);

    return true;
}

bool KTextEditor::DocumentPrivate::documentSave()
{
    if (!url().isValid() || !isReadWrite()) {
        return documentSaveAs();
    }

    return save();
}

bool KTextEditor::DocumentPrivate::documentSaveAs()
{
    const QUrl saveUrl = getSaveFileUrl(i18n("Save File"));
    if (saveUrl.isEmpty()) {
        return false;
    }

    return saveAs(saveUrl);
}

bool KTextEditor::DocumentPrivate::documentSaveAsWithEncoding(const QString &encoding)
{
    const QUrl saveUrl = getSaveFileUrl(i18n("Save File"));
    if (saveUrl.isEmpty()) {
        return false;
    }

    setEncoding(encoding);
    return saveAs(saveUrl);
}

bool KTextEditor::DocumentPrivate::documentSaveCopyAs()
{
    const QUrl saveUrl = getSaveFileUrl(i18n("Save Copy of File"));
    if (saveUrl.isEmpty()) {
        return false;
    }

    QTemporaryFile file;
    if (!file.open()) {
        return false;
    }

    if (!m_buffer->saveFile(file.fileName())) {
        KMessageBox::error(dialogParent(),
                           i18n("The document could not be saved, as it was not possible to write to %1.\n\nCheck that you have write access to this file or "
                                "that enough disk space is available.",
                                this->url().toDisplayString(QUrl::PreferLocalFile)));
        return false;
    }

    // get the right permissions, start with safe default
    KIO::StatJob *statJob = KIO::statDetails(url(), KIO::StatJob::SourceSide, KIO::StatBasic);
    KJobWidgets::setWindow(statJob, QApplication::activeWindow());
    int permissions = -1;
    if (statJob->exec()) {
        permissions = KFileItem(statJob->statResult(), url()).permissions();
    }

    // KIO move, important: allow overwrite, we checked above!
    KIO::FileCopyJob *job = KIO::file_copy(QUrl::fromLocalFile(file.fileName()), saveUrl, permissions, KIO::Overwrite);
    KJobWidgets::setWindow(job, QApplication::activeWindow());
    return job->exec();
}

void KTextEditor::DocumentPrivate::setWordWrap(bool on)
{
    config()->setWordWrap(on);
}

bool KTextEditor::DocumentPrivate::wordWrap() const
{
    return config()->wordWrap();
}

void KTextEditor::DocumentPrivate::setWordWrapAt(uint col)
{
    config()->setWordWrapAt(col);
}

unsigned int KTextEditor::DocumentPrivate::wordWrapAt() const
{
    return config()->wordWrapAt();
}

void KTextEditor::DocumentPrivate::setPageUpDownMovesCursor(bool on)
{
    config()->setPageUpDownMovesCursor(on);
}

bool KTextEditor::DocumentPrivate::pageUpDownMovesCursor() const
{
    return config()->pageUpDownMovesCursor();
}
// END

bool KTextEditor::DocumentPrivate::setEncoding(const QString &e)
{
    return m_config->setEncoding(e);
}

QString KTextEditor::DocumentPrivate::encoding() const
{
    return m_config->encoding();
}

void KTextEditor::DocumentPrivate::updateConfig()
{
    m_undoManager->updateConfig();

    // switch indenter if needed and update config....
    m_indenter->setMode(m_config->indentationMode());
    m_indenter->updateConfig();

    // set tab width there, too
    m_buffer->setTabWidth(config()->tabWidth());

    // update all views, does tagAll and updateView...
    for (auto view : std::as_const(m_views)) {
        view->updateDocumentConfig();
    }

    // update on-the-fly spell checking as spell checking defaults might have changes
    if (m_onTheFlyChecker) {
        m_onTheFlyChecker->updateConfig();
    }

    Q_EMIT configChanged(this);
}

// BEGIN Variable reader
// "local variable" feature by anders, 2003
/* TODO
      add config options (how many lines to read, on/off)
      add interface for plugins/apps to set/get variables
      add view stuff
*/
void KTextEditor::DocumentPrivate::readVariables(bool onlyViewAndRenderer)
{
    if (!onlyViewAndRenderer) {
        m_config->configStart();
    }

    // views!
    for (auto v : std::as_const(m_views)) {
        v->config()->configStart();
        v->renderer()->config()->configStart();
    }
    // read a number of lines in the top/bottom of the document
    for (int i = 0; i < qMin(9, lines()); ++i) {
        readVariableLine(line(i), onlyViewAndRenderer);
    }
    if (lines() > 10) {
        for (int i = qMax(10, lines() - 10); i < lines(); i++) {
            readVariableLine(line(i), onlyViewAndRenderer);
        }
    }

    if (!onlyViewAndRenderer) {
        m_config->configEnd();
    }

    for (auto v : std::as_const(m_views)) {
        v->config()->configEnd();
        v->renderer()->config()->configEnd();
    }
}

void KTextEditor::DocumentPrivate::readVariableLine(const QString &t, bool onlyViewAndRenderer)
{
    static const QRegularExpression kvLine(QStringLiteral("kate:(.*)"));
    static const QRegularExpression kvLineWildcard(QStringLiteral("kate-wildcard\\((.*)\\):(.*)"));
    static const QRegularExpression kvLineMime(QStringLiteral("kate-mimetype\\((.*)\\):(.*)"));
    static const QRegularExpression kvVar(QStringLiteral("([\\w\\-]+)\\s+([^;]+)"));

    // simple check first, no regex
    // no kate inside, no vars, simple...
    if (!t.contains(QLatin1String("kate"))) {
        return;
    }

    // found vars, if any
    QString s;

    // now, try first the normal ones
    auto match = kvLine.match(t);
    if (match.hasMatch()) {
        s = match.captured(1);

        // qCDebug(LOG_KTE) << "normal variable line kate: matched: " << s;
    } else if ((match = kvLineWildcard.match(t)).hasMatch()) { // regex given
        const QStringList wildcards(match.captured(1).split(QLatin1Char(';'), Qt::SkipEmptyParts));
        const QString nameOfFile = url().fileName();

        bool found = false;
        for (const QString &pattern : wildcards) {
            QRegularExpression wildcard(QRegularExpression::wildcardToRegularExpression(pattern));
            found = wildcard.match(nameOfFile).hasMatch();

            if (found) {
                break;
            }
        }

        // nothing usable found...
        if (!found) {
            return;
        }

        s = match.captured(2);

        // qCDebug(LOG_KTE) << "guarded variable line kate-wildcard: matched: " << s;
    } else if ((match = kvLineMime.match(t)).hasMatch()) { // mime-type given
        const QStringList types(match.captured(1).split(QLatin1Char(';'), Qt::SkipEmptyParts));

        // no matching type found
        if (!types.contains(mimeType())) {
            return;
        }

        s = match.captured(2);

        // qCDebug(LOG_KTE) << "guarded variable line kate-mimetype: matched: " << s;
    } else { // nothing found
        return;
    }

    // view variable names
    static const auto vvl = {QLatin1String("dynamic-word-wrap"),
                             QLatin1String("dynamic-word-wrap-indicators"),
                             QLatin1String("line-numbers"),
                             QLatin1String("icon-border"),
                             QLatin1String("folding-markers"),
                             QLatin1String("folding-preview"),
                             QLatin1String("bookmark-sorting"),
                             QLatin1String("auto-center-lines"),
                             QLatin1String("icon-bar-color"),
                             QLatin1String("scrollbar-minimap"),
                             QLatin1String("scrollbar-preview")
                             // renderer
                             ,
                             QLatin1String("background-color"),
                             QLatin1String("selection-color"),
                             QLatin1String("current-line-color"),
                             QLatin1String("bracket-highlight-color"),
                             QLatin1String("word-wrap-marker-color"),
                             QLatin1String("font"),
                             QLatin1String("font-size"),
                             QLatin1String("scheme")};
    int spaceIndent = -1; // for backward compatibility; see below
    bool replaceTabsSet = false;
    int startPos(0);

    QString var;
    QString val;
    while ((match = kvVar.match(s, startPos)).hasMatch()) {
        startPos = match.capturedEnd(0);
        var = match.captured(1);
        val = match.captured(2).trimmed();
        bool state; // store booleans here
        int n; // store ints here

        // only apply view & renderer config stuff
        if (onlyViewAndRenderer) {
            if (contains(vvl, var)) { // FIXME define above
                setViewVariable(var, val);
            }
        } else {
            // BOOL  SETTINGS
            if (var == QLatin1String("word-wrap") && checkBoolValue(val, &state)) {
                setWordWrap(state); // ??? FIXME CHECK
            }
            // KateConfig::configFlags
            // FIXME should this be optimized to only a few calls? how?
            else if (var == QLatin1String("backspace-indents") && checkBoolValue(val, &state)) {
                m_config->setBackspaceIndents(state);
            } else if (var == QLatin1String("indent-pasted-text") && checkBoolValue(val, &state)) {
                m_config->setIndentPastedText(state);
            } else if (var == QLatin1String("replace-tabs") && checkBoolValue(val, &state)) {
                m_config->setReplaceTabsDyn(state);
                replaceTabsSet = true; // for backward compatibility; see below
            } else if (var == QLatin1String("remove-trailing-space") && checkBoolValue(val, &state)) {
                qCWarning(LOG_KTE) << i18n(
                    "Using deprecated modeline 'remove-trailing-space'. "
                    "Please replace with 'remove-trailing-spaces modified;', see "
                    "https://docs.kde.org/?application=katepart&branch=stable5&path=config-variables.html#variable-remove-trailing-spaces");
                m_config->setRemoveSpaces(state ? 1 : 0);
            } else if (var == QLatin1String("replace-trailing-space-save") && checkBoolValue(val, &state)) {
                qCWarning(LOG_KTE) << i18n(
                    "Using deprecated modeline 'replace-trailing-space-save'. "
                    "Please replace with 'remove-trailing-spaces all;', see "
                    "https://docs.kde.org/?application=katepart&branch=stable5&path=config-variables.html#variable-remove-trailing-spaces");
                m_config->setRemoveSpaces(state ? 2 : 0);
            } else if (var == QLatin1String("overwrite-mode") && checkBoolValue(val, &state)) {
                m_config->setOvr(state);
            } else if (var == QLatin1String("keep-extra-spaces") && checkBoolValue(val, &state)) {
                m_config->setKeepExtraSpaces(state);
            } else if (var == QLatin1String("tab-indents") && checkBoolValue(val, &state)) {
                m_config->setTabIndents(state);
            } else if (var == QLatin1String("show-tabs") && checkBoolValue(val, &state)) {
                m_config->setShowTabs(state);
            } else if (var == QLatin1String("show-trailing-spaces") && checkBoolValue(val, &state)) {
                m_config->setShowSpaces(state ? KateDocumentConfig::Trailing : KateDocumentConfig::None);
            } else if (var == QLatin1String("space-indent") && checkBoolValue(val, &state)) {
                // this is for backward compatibility; see below
                spaceIndent = state;
            } else if (var == QLatin1String("smart-home") && checkBoolValue(val, &state)) {
                m_config->setSmartHome(state);
            } else if (var == QLatin1String("newline-at-eof") && checkBoolValue(val, &state)) {
                m_config->setNewLineAtEof(state);
            }

            // INTEGER SETTINGS
            else if (var == QLatin1String("tab-width") && checkIntValue(val, &n)) {
                m_config->setTabWidth(n);
            } else if (var == QLatin1String("indent-width") && checkIntValue(val, &n)) {
                m_config->setIndentationWidth(n);
            } else if (var == QLatin1String("indent-mode")) {
                m_config->setIndentationMode(val);
            } else if (var == QLatin1String("word-wrap-column") && checkIntValue(val, &n) && n > 0) { // uint, but hard word wrap at 0 will be no fun ;)
                m_config->setWordWrapAt(n);
            }

            // STRING SETTINGS
            else if (var == QLatin1String("eol") || var == QLatin1String("end-of-line")) {
                const auto l = {QLatin1String("unix"), QLatin1String("dos"), QLatin1String("mac")};
                if ((n = indexOf(l, val.toLower())) != -1) {
                    // set eol + avoid that it is overwritten by auto-detection again!
                    // this fixes e.g. .kateconfig files with // kate: eol dos; to work, bug 365705
                    m_config->setEol(n);
                    m_config->setAllowEolDetection(false);
                }
            } else if (var == QLatin1String("bom") || var == QLatin1String("byte-order-mark") || var == QLatin1String("byte-order-marker")) {
                if (checkBoolValue(val, &state)) {
                    m_config->setBom(state);
                }
            } else if (var == QLatin1String("remove-trailing-spaces")) {
                val = val.toLower();
                if (val == QLatin1String("1") || val == QLatin1String("modified") || val == QLatin1String("mod") || val == QLatin1String("+")) {
                    m_config->setRemoveSpaces(1);
                } else if (val == QLatin1String("2") || val == QLatin1String("all") || val == QLatin1String("*")) {
                    m_config->setRemoveSpaces(2);
                } else {
                    m_config->setRemoveSpaces(0);
                }
            } else if (var == QLatin1String("syntax") || var == QLatin1String("hl")) {
                setHighlightingMode(val);
            } else if (var == QLatin1String("mode")) {
                setMode(val);
            } else if (var == QLatin1String("encoding")) {
                setEncoding(val);
            } else if (var == QLatin1String("default-dictionary")) {
                setDefaultDictionary(val);
            } else if (var == QLatin1String("automatic-spell-checking") && checkBoolValue(val, &state)) {
                onTheFlySpellCheckingEnabled(state);
            }

            // VIEW SETTINGS
            else if (contains(vvl, var)) {
                setViewVariable(var, val);
            } else {
                m_storedVariables[var] = val;
            }
        }
    }

    // Backward compatibility
    // If space-indent was set, but replace-tabs was not set, we assume
    // that the user wants to replace tabulators and set that flag.
    // If both were set, replace-tabs has precedence.
    // At this point spaceIndent is -1 if it was never set,
    // 0 if it was set to off, and 1 if it was set to on.
    // Note that if onlyViewAndRenderer was requested, spaceIndent is -1.
    if (!replaceTabsSet && spaceIndent >= 0) {
        m_config->setReplaceTabsDyn(spaceIndent > 0);
    }
}

void KTextEditor::DocumentPrivate::setViewVariable(const QString &var, const QString &val)
{
    bool state;
    int n;
    QColor c;
    for (auto v : std::as_const(m_views)) {
        // First, try the new config interface
        QVariant help(val); // Special treatment to catch "on"/"off"
        if (checkBoolValue(val, &state)) {
            help = state;
        }
        if (v->config()->setValue(var, help)) {
        } else if (v->renderer()->config()->setValue(var, help)) {
            // No success? Go the old way
        } else if (var == QLatin1String("dynamic-word-wrap") && checkBoolValue(val, &state)) {
            v->config()->setDynWordWrap(state);
        } else if (var == QLatin1String("block-selection") && checkBoolValue(val, &state)) {
            v->setBlockSelection(state);

            // else if ( var = "dynamic-word-wrap-indicators" )
        } else if (var == QLatin1String("icon-bar-color") && checkColorValue(val, c)) {
            v->renderer()->config()->setIconBarColor(c);
        }
        // RENDERER
        else if (var == QLatin1String("background-color") && checkColorValue(val, c)) {
            v->renderer()->config()->setBackgroundColor(c);
        } else if (var == QLatin1String("selection-color") && checkColorValue(val, c)) {
            v->renderer()->config()->setSelectionColor(c);
        } else if (var == QLatin1String("current-line-color") && checkColorValue(val, c)) {
            v->renderer()->config()->setHighlightedLineColor(c);
        } else if (var == QLatin1String("bracket-highlight-color") && checkColorValue(val, c)) {
            v->renderer()->config()->setHighlightedBracketColor(c);
        } else if (var == QLatin1String("word-wrap-marker-color") && checkColorValue(val, c)) {
            v->renderer()->config()->setWordWrapMarkerColor(c);
        } else if (var == QLatin1String("font") || (checkIntValue(val, &n) && var == QLatin1String("font-size"))) {
            QFont _f(v->renderer()->currentFont());

            if (var == QLatin1String("font")) {
                _f.setFamily(val);
                _f.setFixedPitch(QFont(val).fixedPitch());
            } else {
                _f.setPointSize(n);
            }

            v->renderer()->config()->setFont(_f);
        } else if (var == QLatin1String("scheme")) {
            v->renderer()->config()->setSchema(val);
        }
    }
}

bool KTextEditor::DocumentPrivate::checkBoolValue(QString val, bool *result)
{
    val = val.trimmed().toLower();
    static const auto trueValues = {QLatin1String("1"), QLatin1String("on"), QLatin1String("true")};
    if (contains(trueValues, val)) {
        *result = true;
        return true;
    }

    static const auto falseValues = {QLatin1String("0"), QLatin1String("off"), QLatin1String("false")};
    if (contains(falseValues, val)) {
        *result = false;
        return true;
    }
    return false;
}

bool KTextEditor::DocumentPrivate::checkIntValue(const QString &val, int *result)
{
    bool ret(false);
    *result = val.toInt(&ret);
    return ret;
}

bool KTextEditor::DocumentPrivate::checkColorValue(const QString &val, QColor &c)
{
    c.setNamedColor(val);
    return c.isValid();
}

// KTextEditor::variable
QString KTextEditor::DocumentPrivate::variable(const QString &name) const
{
    auto it = m_storedVariables.find(name);
    if (it == m_storedVariables.end()) {
        return QString();
    }
    return it->second;
}

void KTextEditor::DocumentPrivate::setVariable(const QString &name, const QString &value)
{
    QString s = QStringLiteral("kate: ");
    s.append(name);
    s.append(QLatin1Char(' '));
    s.append(value);
    readVariableLine(s);
}

// END

void KTextEditor::DocumentPrivate::slotModOnHdDirty(const QString &path)
{
    if ((path == m_dirWatchFile) && (!m_modOnHd || m_modOnHdReason != OnDiskModified)) {
        m_modOnHd = true;
        m_modOnHdReason = OnDiskModified;

        if (!m_modOnHdTimer.isActive()) {
            m_modOnHdTimer.start();
        }
    }
}

void KTextEditor::DocumentPrivate::slotModOnHdCreated(const QString &path)
{
    if ((path == m_dirWatchFile) && (!m_modOnHd || m_modOnHdReason != OnDiskCreated)) {
        m_modOnHd = true;
        m_modOnHdReason = OnDiskCreated;

        if (!m_modOnHdTimer.isActive()) {
            m_modOnHdTimer.start();
        }
    }
}

void KTextEditor::DocumentPrivate::slotModOnHdDeleted(const QString &path)
{
    if ((path == m_dirWatchFile) && (!m_modOnHd || m_modOnHdReason != OnDiskDeleted)) {
        m_modOnHd = true;
        m_modOnHdReason = OnDiskDeleted;

        if (!m_modOnHdTimer.isActive()) {
            m_modOnHdTimer.start();
        }
    }
}

void KTextEditor::DocumentPrivate::slotDelayedHandleModOnHd()
{
    // compare git hash with the one we have (if we have one)
    const QByteArray oldDigest = checksum();
    if (!oldDigest.isEmpty() && !url().isEmpty() && url().isLocalFile()) {
        // if current checksum == checksum of new file => unmodified
        if (m_modOnHdReason != OnDiskDeleted && m_modOnHdReason != OnDiskCreated && createDigest() && oldDigest == checksum()) {
            m_modOnHd = false;
            m_modOnHdReason = OnDiskUnmodified;
            m_prevModOnHdReason = OnDiskUnmodified;
        }

        // if still modified, try to take a look at git
        // skip that, if document is modified!
        // only do that, if the file is still there, else reload makes no sense!
        if (m_modOnHd && !isModified() && QFile::exists(url().toLocalFile())) {
            // we only want to use git from PATH, cache this
            static const QString fullGitPath = QStandardPaths::findExecutable(QStringLiteral("git"));
            if (!fullGitPath.isEmpty()) {
                QProcess git;
                const QStringList args{QStringLiteral("cat-file"), QStringLiteral("-e"), QString::fromUtf8(oldDigest.toHex())};
                git.setWorkingDirectory(url().adjusted(QUrl::RemoveFilename).toLocalFile());
                git.start(fullGitPath, args);
                if (git.waitForStarted()) {
                    git.closeWriteChannel();
                    if (git.waitForFinished()) {
                        if (git.exitCode() == 0) {
                            // this hash exists still in git => just reload
                            m_modOnHd = false;
                            m_modOnHdReason = OnDiskUnmodified;
                            m_prevModOnHdReason = OnDiskUnmodified;
                            documentReload();
                        }
                    }
                }
            }
        }
    }

    // emit our signal to the outside!
    Q_EMIT modifiedOnDisk(this, m_modOnHd, m_modOnHdReason);
}

QByteArray KTextEditor::DocumentPrivate::checksum() const
{
    return m_buffer->digest();
}

bool KTextEditor::DocumentPrivate::createDigest()
{
    QByteArray digest;

    if (url().isLocalFile()) {
        QFile f(url().toLocalFile());
        if (f.open(QIODevice::ReadOnly)) {
            // init the hash with the git header
            QCryptographicHash crypto(QCryptographicHash::Sha1);
            const QString header = QStringLiteral("blob %1").arg(f.size());
            crypto.addData(QByteArray(header.toLatin1() + '\0'));

            while (!f.atEnd()) {
                crypto.addData(f.read(256 * 1024));
            }

            digest = crypto.result();
        }
    }

    // set new digest
    m_buffer->setDigest(digest);
    return !digest.isEmpty();
}

QString KTextEditor::DocumentPrivate::reasonedMOHString() const
{
    // squeeze path
    const QString str = KStringHandler::csqueeze(url().toDisplayString(QUrl::PreferLocalFile));

    switch (m_modOnHdReason) {
    case OnDiskModified:
        return i18n("The file '%1' was modified on disk.", str);
        break;
    case OnDiskCreated:
        return i18n("The file '%1' was created on disk.", str);
        break;
    case OnDiskDeleted:
        return i18n("The file '%1' was deleted on disk.", str);
        break;
    default:
        return QString();
    }
    Q_UNREACHABLE();
    return QString();
}

void KTextEditor::DocumentPrivate::removeTrailingSpaces()
{
    const int remove = config()->removeSpaces();
    if (remove == 0) {
        return;
    }

    // temporarily disable static word wrap (see bug #328900)
    const bool wordWrapEnabled = config()->wordWrap();
    if (wordWrapEnabled) {
        setWordWrap(false);
    }

    editStart();

    const int lines = this->lines();
    for (int line = 0; line < lines; ++line) {

        // remove trailing spaces in entire document, remove = 2
        // remove trailing spaces of touched lines, remove = 1
        // remove trailing spaces of lines saved on disk, remove = 1
        if (remove == 2 || m_buffer->isLineModified(line) || m_buffer->isLineSaved(line)) {
            const auto lineText = plainKateTextLine(line);
            const int p = Kate::TextLineData::lastChar(lineText) + 1;
            const int l = lineText.length() - p;
            if (l > 0) {
                editRemoveText(line, p, l);
            }
        }
    }

    editEnd();

    // enable word wrap again, if it was enabled (see bug #328900)
    if (wordWrapEnabled) {
        setWordWrap(true); // see begin of this function
    }
}

bool KTextEditor::DocumentPrivate::updateFileType(const QString &newType, bool user)
{
    if (user || !m_fileTypeSetByUser) {
        if (newType.isEmpty()) {
            return false;
        }
        KateFileType fileType = KTextEditor::EditorPrivate::self()->modeManager()->fileType(newType);
        // if the mode "newType" does not exist
        if (fileType.name.isEmpty()) {
            return false;
        }

        // remember that we got set by user
        m_fileTypeSetByUser = user;

        m_fileType = newType;

        m_config->configStart();

        // NOTE: if the user changes the Mode, the Highlighting also changes.
        // m_hlSetByUser avoids resetting the highlight when saving the document, if
        // the current hl isn't stored (eg, in sftp:// or fish:// files) (see bug #407763)
        if ((user || !m_hlSetByUser) && !fileType.hl.isEmpty()) {
            int hl(KateHlManager::self()->nameFind(fileType.hl));

            if (hl >= 0) {
                m_buffer->setHighlight(hl);
            }
        }

        // set the indentation mode, if any in the mode...
        // and user did not set it before!
        // NOTE: KateBuffer::setHighlight() also sets the indentation.
        if (!m_indenterSetByUser && !fileType.indenter.isEmpty()) {
            config()->setIndentationMode(fileType.indenter);
        }

        // views!
        for (auto v : std::as_const(m_views)) {
            v->config()->configStart();
            v->renderer()->config()->configStart();
        }

        bool bom_settings = false;
        if (m_bomSetByUser) {
            bom_settings = m_config->bom();
        }
        readVariableLine(fileType.varLine);
        if (m_bomSetByUser) {
            m_config->setBom(bom_settings);
        }
        m_config->configEnd();
        for (auto v : std::as_const(m_views)) {
            v->config()->configEnd();
            v->renderer()->config()->configEnd();
        }
    }

    // fixme, make this better...
    Q_EMIT modeChanged(this);
    return true;
}

void KTextEditor::DocumentPrivate::slotQueryClose_save(bool *handled, bool *abortClosing)
{
    *handled = true;
    *abortClosing = true;
    if (url().isEmpty()) {
        const QUrl res = getSaveFileUrl(i18n("Save File"));
        if (res.isEmpty()) {
            *abortClosing = true;
            return;
        }
        saveAs(res);
        *abortClosing = false;
    } else {
        save();
        *abortClosing = false;
    }
}

// BEGIN KTextEditor::ConfigInterface

// BEGIN ConfigInterface stff
QStringList KTextEditor::DocumentPrivate::configKeys() const
{
    // expose all internally registered keys of the KateDocumentConfig
    return m_config->configKeys();
}

QVariant KTextEditor::DocumentPrivate::configValue(const QString &key)
{
    // just dispatch to internal key => value lookup
    return m_config->value(key);
}

void KTextEditor::DocumentPrivate::setConfigValue(const QString &key, const QVariant &value)
{
    // just dispatch to internal key + value set
    m_config->setValue(key, value);
}

// END KTextEditor::ConfigInterface

KTextEditor::Cursor KTextEditor::DocumentPrivate::documentEnd() const
{
    return KTextEditor::Cursor(lastLine(), lineLength(lastLine()));
}

bool KTextEditor::DocumentPrivate::replaceText(const KTextEditor::Range &range, const QString &s, bool block)
{
    // TODO more efficient?
    editStart();
    bool changed = removeText(range, block);
    changed |= insertText(range.start(), s, block);
    editEnd();
    return changed;
}

KateHighlighting *KTextEditor::DocumentPrivate::highlight() const
{
    return m_buffer->highlight();
}

QString KTextEditor::DocumentPrivate::kateTextLine(int i)
{
    m_buffer->ensureHighlighted(i);
    return m_buffer->lineText(i);
}

QString KTextEditor::DocumentPrivate::plainKateTextLine(int i)
{
    return m_buffer->lineText(i);
}

bool KTextEditor::DocumentPrivate::isEditRunning() const
{
    return editIsRunning;
}

void KTextEditor::DocumentPrivate::setUndoMergeAllEdits(bool merge)
{
    if (merge && m_undoMergeAllEdits) {
        // Don't add another undo safe point: it will override our current one,
        // meaning we'll need two undo's to get back there - which defeats the object!
        return;
    }
    m_undoManager->undoSafePoint();
    m_undoManager->setAllowComplexMerge(merge);
    m_undoMergeAllEdits = merge;
}

// BEGIN KTextEditor::MovingInterface
KTextEditor::MovingCursor *KTextEditor::DocumentPrivate::newMovingCursor(const KTextEditor::Cursor &position,
                                                                         KTextEditor::MovingCursor::InsertBehavior insertBehavior)
{
    return new Kate::TextCursor(buffer(), position, insertBehavior);
}

KTextEditor::MovingRange *KTextEditor::DocumentPrivate::newMovingRange(const KTextEditor::Range &range,
                                                                       KTextEditor::MovingRange::InsertBehaviors insertBehaviors,
                                                                       KTextEditor::MovingRange::EmptyBehavior emptyBehavior)
{
    return new Kate::TextRange(buffer(), range, insertBehaviors, emptyBehavior);
}

qint64 KTextEditor::DocumentPrivate::revision() const
{
    return m_buffer->history().revision();
}

qint64 KTextEditor::DocumentPrivate::lastSavedRevision() const
{
    return m_buffer->history().lastSavedRevision();
}

void KTextEditor::DocumentPrivate::lockRevision(qint64 revision)
{
    m_buffer->history().lockRevision(revision);
}

void KTextEditor::DocumentPrivate::unlockRevision(qint64 revision)
{
    m_buffer->history().unlockRevision(revision);
}

void KTextEditor::DocumentPrivate::transformCursor(int &line,
                                                   int &column,
                                                   KTextEditor::MovingCursor::InsertBehavior insertBehavior,
                                                   qint64 fromRevision,
                                                   qint64 toRevision)
{
    m_buffer->history().transformCursor(line, column, insertBehavior, fromRevision, toRevision);
}

void KTextEditor::DocumentPrivate::transformCursor(KTextEditor::Cursor &cursor,
                                                   KTextEditor::MovingCursor::InsertBehavior insertBehavior,
                                                   qint64 fromRevision,
                                                   qint64 toRevision)
{
    int line = cursor.line();
    int column = cursor.column();
    m_buffer->history().transformCursor(line, column, insertBehavior, fromRevision, toRevision);
    cursor.setPosition(line, column);
}

void KTextEditor::DocumentPrivate::transformRange(KTextEditor::Range &range,
                                                  KTextEditor::MovingRange::InsertBehaviors insertBehaviors,
                                                  KTextEditor::MovingRange::EmptyBehavior emptyBehavior,
                                                  qint64 fromRevision,
                                                  qint64 toRevision)
{
    m_buffer->history().transformRange(range, insertBehaviors, emptyBehavior, fromRevision, toRevision);
}

// END

// BEGIN KTextEditor::AnnotationInterface
void KTextEditor::DocumentPrivate::setAnnotationModel(KTextEditor::AnnotationModel *model)
{
    KTextEditor::AnnotationModel *oldmodel = m_annotationModel;
    m_annotationModel = model;
    Q_EMIT annotationModelChanged(oldmodel, m_annotationModel);
}

KTextEditor::AnnotationModel *KTextEditor::DocumentPrivate::annotationModel() const
{
    return m_annotationModel;
}
// END KTextEditor::AnnotationInterface

// TAKEN FROM kparts.h
bool KTextEditor::DocumentPrivate::queryClose()
{
    if (!isReadWrite() // Can't be modified
        || !isModified() // Nothing was modified
        || (url() == QUrl() && lines() == 1 && text() == QString()) // Unsaved and blank
    ) {
        return true;
    }

    QString docName = documentName();

    int res = KMessageBox::warningYesNoCancel(dialogParent(),
                                              i18n("The document \"%1\" has been modified.\n"
                                                   "Do you want to save your changes or discard them?",
                                                   docName),
                                              i18n("Close Document"),
                                              KStandardGuiItem::save(),
                                              KStandardGuiItem::discard());

    bool abortClose = false;
    bool handled = false;

    switch (res) {
    case KMessageBox::Yes:
        sigQueryClose(&handled, &abortClose);
        if (!handled) {
            if (url().isEmpty()) {
                const QUrl url = getSaveFileUrl(i18n("Save File"));
                if (url.isEmpty()) {
                    return false;
                }

                saveAs(url);
            } else {
                save();
            }
        } else if (abortClose) {
            return false;
        }
        return waitSaveComplete();
    case KMessageBox::No:
        return true;
    default: // case KMessageBox::Cancel :
        return false;
    }
}

void KTextEditor::DocumentPrivate::slotStarted(KIO::Job *job)
{
    // if we are idle before, we are now loading!
    if (m_documentState == DocumentIdle) {
        m_documentState = DocumentLoading;
    }

    // if loading:
    // - remember pre loading read-write mode
    // if remote load:
    // - set to read-only
    // - trigger possible message
    if (m_documentState == DocumentLoading) {
        // remember state
        m_readWriteStateBeforeLoading = isReadWrite();

        // perhaps show loading message, but wait one second
        if (job) {
            // only read only if really remote file!
            setReadWrite(false);

            // perhaps some message about loading in one second!
            // remember job pointer, we want to be able to kill it!
            m_loadingJob = job;
            QTimer::singleShot(1000, this, SLOT(slotTriggerLoadingMessage()));
        }
    }
}

void KTextEditor::DocumentPrivate::slotCompleted()
{
    // if were loading, reset back to old read-write mode before loading
    // and kill the possible loading message
    if (m_documentState == DocumentLoading) {
        setReadWrite(m_readWriteStateBeforeLoading);
        delete m_loadingMessage;
    }

    // Emit signal that we saved  the document, if needed
    if (m_documentState == DocumentSaving || m_documentState == DocumentSavingAs) {
        Q_EMIT documentSavedOrUploaded(this, m_documentState == DocumentSavingAs);
    }

    // back to idle mode
    m_documentState = DocumentIdle;
    m_reloading = false;
}

void KTextEditor::DocumentPrivate::slotCanceled()
{
    // if were loading, reset back to old read-write mode before loading
    // and kill the possible loading message
    if (m_documentState == DocumentLoading) {
        setReadWrite(m_readWriteStateBeforeLoading);
        delete m_loadingMessage;

        showAndSetOpeningErrorAccess();

        updateDocName();
    }

    // back to idle mode
    m_documentState = DocumentIdle;
    m_reloading = false;
}

void KTextEditor::DocumentPrivate::slotTriggerLoadingMessage()
{
    // no longer loading?
    // no message needed!
    if (m_documentState != DocumentLoading) {
        return;
    }

    // create message about file loading in progress
    delete m_loadingMessage;
    m_loadingMessage =
        new KTextEditor::Message(i18n("The file <a href=\"%1\">%2</a> is still loading.", url().toDisplayString(QUrl::PreferLocalFile), url().fileName()));
    m_loadingMessage->setPosition(KTextEditor::Message::TopInView);

    // if around job: add cancel action
    if (m_loadingJob) {
        QAction *cancel = new QAction(i18n("&Abort Loading"), nullptr);
        connect(cancel, &QAction::triggered, this, &KTextEditor::DocumentPrivate::slotAbortLoading);
        m_loadingMessage->addAction(cancel);
    }

    // really post message
    postMessage(m_loadingMessage);
}

void KTextEditor::DocumentPrivate::slotAbortLoading()
{
    // no job, no work
    if (!m_loadingJob) {
        return;
    }

    // abort loading if any job
    // signal results!
    m_loadingJob->kill(KJob::EmitResult);
    m_loadingJob = nullptr;
}

void KTextEditor::DocumentPrivate::slotUrlChanged(const QUrl &url)
{
    if (m_reloading) {
        // the URL is temporarily unset and then reset to the previous URL during reload
        // we do not want to notify the outside about this
        return;
    }

    Q_UNUSED(url);
    updateDocName();
    Q_EMIT documentUrlChanged(this);
}

bool KTextEditor::DocumentPrivate::save()
{
    // no double save/load
    // we need to allow DocumentPreSavingAs here as state, as save is called in saveAs!
    if ((m_documentState != DocumentIdle) && (m_documentState != DocumentPreSavingAs)) {
        return false;
    }

    // if we are idle, we are now saving
    if (m_documentState == DocumentIdle) {
        m_documentState = DocumentSaving;
    } else {
        m_documentState = DocumentSavingAs;
    }

    // let anyone listening know that we are going to save
    Q_EMIT aboutToSave(this);

    // call back implementation for real work
    return KTextEditor::Document::save();
}

bool KTextEditor::DocumentPrivate::saveAs(const QUrl &url)
{
    // abort on bad URL
    // that is done in saveAs below, too
    // but we must check it here already to avoid messing up
    // as no signals will be send, then
    if (!url.isValid()) {
        return false;
    }

    // no double save/load
    if (m_documentState != DocumentIdle) {
        return false;
    }

    // we enter the pre save as phase
    m_documentState = DocumentPreSavingAs;

    // call base implementation for real work
    return KTextEditor::Document::saveAs(normalizeUrl(url));
}

QString KTextEditor::DocumentPrivate::defaultDictionary() const
{
    return m_defaultDictionary;
}

QList<QPair<KTextEditor::MovingRange *, QString>> KTextEditor::DocumentPrivate::dictionaryRanges() const
{
    return m_dictionaryRanges;
}

void KTextEditor::DocumentPrivate::clearDictionaryRanges()
{
    for (QList<QPair<KTextEditor::MovingRange *, QString>>::iterator i = m_dictionaryRanges.begin(); i != m_dictionaryRanges.end(); ++i) {
        delete (*i).first;
    }
    m_dictionaryRanges.clear();
    if (m_onTheFlyChecker) {
        m_onTheFlyChecker->refreshSpellCheck();
    }
    Q_EMIT dictionaryRangesPresent(false);
}

void KTextEditor::DocumentPrivate::setDictionary(const QString &newDictionary, KTextEditor::Range range, bool blockmode)
{
    if (blockmode) {
        for (int i = range.start().line(); i <= range.end().line(); ++i) {
            setDictionary(newDictionary, rangeOnLine(range, i));
        }
    } else {
        setDictionary(newDictionary, range);
    }

    Q_EMIT dictionaryRangesPresent(!m_dictionaryRanges.isEmpty());
}

void KTextEditor::DocumentPrivate::setDictionary(const QString &newDictionary, KTextEditor::Range range)
{
    KTextEditor::Range newDictionaryRange = range;
    if (!newDictionaryRange.isValid() || newDictionaryRange.isEmpty()) {
        return;
    }
    QList<QPair<KTextEditor::MovingRange *, QString>> newRanges;
    // all ranges is 'm_dictionaryRanges' are assumed to be mutually disjoint
    for (auto i = m_dictionaryRanges.begin(); i != m_dictionaryRanges.end();) {
        qCDebug(LOG_KTE) << "new iteration" << newDictionaryRange;
        if (newDictionaryRange.isEmpty()) {
            break;
        }
        QPair<KTextEditor::MovingRange *, QString> pair = *i;
        QString dictionarySet = pair.second;
        KTextEditor::MovingRange *dictionaryRange = pair.first;
        qCDebug(LOG_KTE) << *dictionaryRange << dictionarySet;
        if (dictionaryRange->contains(newDictionaryRange) && newDictionary == dictionarySet) {
            qCDebug(LOG_KTE) << "dictionaryRange contains newDictionaryRange";
            return;
        }
        if (newDictionaryRange.contains(*dictionaryRange)) {
            delete dictionaryRange;
            i = m_dictionaryRanges.erase(i);
            qCDebug(LOG_KTE) << "newDictionaryRange contains dictionaryRange";
            continue;
        }

        KTextEditor::Range intersection = dictionaryRange->toRange().intersect(newDictionaryRange);
        if (!intersection.isEmpty() && intersection.isValid()) {
            if (dictionarySet == newDictionary) { // we don't have to do anything for 'intersection'
                // except cut off the intersection
                QList<KTextEditor::Range> remainingRanges = KateSpellCheckManager::rangeDifference(newDictionaryRange, intersection);
                Q_ASSERT(remainingRanges.size() == 1);
                newDictionaryRange = remainingRanges.first();
                ++i;
                qCDebug(LOG_KTE) << "dictionarySet == newDictionary";
                continue;
            }
            QList<KTextEditor::Range> remainingRanges = KateSpellCheckManager::rangeDifference(*dictionaryRange, intersection);
            for (auto j = remainingRanges.begin(); j != remainingRanges.end(); ++j) {
                KTextEditor::MovingRange *remainingRange = newMovingRange(*j, KTextEditor::MovingRange::ExpandLeft | KTextEditor::MovingRange::ExpandRight);
                remainingRange->setFeedback(this);
                newRanges.push_back({remainingRange, dictionarySet});
            }
            i = m_dictionaryRanges.erase(i);
            delete dictionaryRange;
        } else {
            ++i;
        }
    }
    m_dictionaryRanges += newRanges;
    if (!newDictionaryRange.isEmpty() && !newDictionary.isEmpty()) { // we don't add anything for the default dictionary
        KTextEditor::MovingRange *newDictionaryMovingRange =
            newMovingRange(newDictionaryRange, KTextEditor::MovingRange::ExpandLeft | KTextEditor::MovingRange::ExpandRight);
        newDictionaryMovingRange->setFeedback(this);
        m_dictionaryRanges.push_back({newDictionaryMovingRange, newDictionary});
    }
    if (m_onTheFlyChecker && !newDictionaryRange.isEmpty()) {
        m_onTheFlyChecker->refreshSpellCheck(newDictionaryRange);
    }
}

void KTextEditor::DocumentPrivate::setDefaultDictionary(const QString &dict)
{
    if (m_defaultDictionary == dict) {
        return;
    }

    m_defaultDictionary = dict;

    if (m_onTheFlyChecker) {
        m_onTheFlyChecker->updateConfig();
        refreshOnTheFlyCheck();
    }
    Q_EMIT defaultDictionaryChanged(this);
}

void KTextEditor::DocumentPrivate::onTheFlySpellCheckingEnabled(bool enable)
{
    if (isOnTheFlySpellCheckingEnabled() == enable) {
        return;
    }

    if (enable) {
        Q_ASSERT(m_onTheFlyChecker == nullptr);
        m_onTheFlyChecker = new KateOnTheFlyChecker(this);
    } else {
        delete m_onTheFlyChecker;
        m_onTheFlyChecker = nullptr;
    }

    for (auto view : std::as_const(m_views)) {
        view->reflectOnTheFlySpellCheckStatus(enable);
    }
}

bool KTextEditor::DocumentPrivate::isOnTheFlySpellCheckingEnabled() const
{
    return m_onTheFlyChecker != nullptr;
}

QString KTextEditor::DocumentPrivate::dictionaryForMisspelledRange(KTextEditor::Range range) const
{
    if (!m_onTheFlyChecker) {
        return QString();
    } else {
        return m_onTheFlyChecker->dictionaryForMisspelledRange(range);
    }
}

void KTextEditor::DocumentPrivate::clearMisspellingForWord(const QString &word)
{
    if (m_onTheFlyChecker) {
        m_onTheFlyChecker->clearMisspellingForWord(word);
    }
}

void KTextEditor::DocumentPrivate::refreshOnTheFlyCheck(KTextEditor::Range range)
{
    if (m_onTheFlyChecker) {
        m_onTheFlyChecker->refreshSpellCheck(range);
    }
}

void KTextEditor::DocumentPrivate::rangeInvalid(KTextEditor::MovingRange *movingRange)
{
    deleteDictionaryRange(movingRange);
}

void KTextEditor::DocumentPrivate::rangeEmpty(KTextEditor::MovingRange *movingRange)
{
    deleteDictionaryRange(movingRange);
}

void KTextEditor::DocumentPrivate::deleteDictionaryRange(KTextEditor::MovingRange *movingRange)
{
    qCDebug(LOG_KTE) << "deleting" << movingRange;

    auto finder = [=](const QPair<KTextEditor::MovingRange *, QString> &item) -> bool {
        return item.first == movingRange;
    };

    auto it = std::find_if(m_dictionaryRanges.begin(), m_dictionaryRanges.end(), finder);

    if (it != m_dictionaryRanges.end()) {
        m_dictionaryRanges.erase(it);
        delete movingRange;
    }

    Q_ASSERT(std::find_if(m_dictionaryRanges.begin(), m_dictionaryRanges.end(), finder) == m_dictionaryRanges.end());
}

bool KTextEditor::DocumentPrivate::containsCharacterEncoding(KTextEditor::Range range)
{
    KateHighlighting *highlighting = highlight();

    const int rangeStartLine = range.start().line();
    const int rangeStartColumn = range.start().column();
    const int rangeEndLine = range.end().line();
    const int rangeEndColumn = range.end().column();

    for (int line = range.start().line(); line <= rangeEndLine; ++line) {
        const QString textLine = kateTextLine(line);
        const int startColumn = (line == rangeStartLine) ? rangeStartColumn : 0;
        const int endColumn = (line == rangeEndLine) ? rangeEndColumn : textLine.length();
        for (int col = startColumn; col < endColumn; ++col) {
            int attr = m_buffer->attributeInLine(line, col);
            const KatePrefixStore &prefixStore = highlighting->getCharacterEncodingsPrefixStore(attr);
            if (!prefixStore.findPrefix(textLine, col).isEmpty()) {
                return true;
            }
        }
    }

    return false;
}

int KTextEditor::DocumentPrivate::computePositionWrtOffsets(const OffsetList &offsetList, int pos)
{
    int previousOffset = 0;
    for (auto i = offsetList.cbegin(); i != offsetList.cend(); ++i) {
        if (i->first > pos) {
            break;
        }
        previousOffset = i->second;
    }
    return pos + previousOffset;
}

QString KTextEditor::DocumentPrivate::decodeCharacters(KTextEditor::Range range,
                                                       KTextEditor::DocumentPrivate::OffsetList &decToEncOffsetList,
                                                       KTextEditor::DocumentPrivate::OffsetList &encToDecOffsetList)
{
    QString toReturn;
    KTextEditor::Cursor previous = range.start();
    int decToEncCurrentOffset = 0;
    int encToDecCurrentOffset = 0;
    int i = 0;
    int newI = 0;

    KateHighlighting *highlighting = highlight();

    const int rangeStartLine = range.start().line();
    const int rangeStartColumn = range.start().column();
    const int rangeEndLine = range.end().line();
    const int rangeEndColumn = range.end().column();

    for (int line = range.start().line(); line <= rangeEndLine; ++line) {
        const QString textLine = kateTextLine(line);
        int startColumn = (line == rangeStartLine) ? rangeStartColumn : 0;
        int endColumn = (line == rangeEndLine) ? rangeEndColumn : textLine.length();
        for (int col = startColumn; col < endColumn;) {
            int attr = m_buffer->attributeInLine(line, col);
            const KatePrefixStore &prefixStore = highlighting->getCharacterEncodingsPrefixStore(attr);
            const QHash<QString, QChar> &characterEncodingsHash = highlighting->getCharacterEncodings(attr);
            QString matchingPrefix = prefixStore.findPrefix(textLine, col);
            if (!matchingPrefix.isEmpty()) {
                toReturn += text(KTextEditor::Range(previous, KTextEditor::Cursor(line, col)));
                const QChar &c = characterEncodingsHash.value(matchingPrefix);
                const bool isNullChar = c.isNull();
                if (!c.isNull()) {
                    toReturn += c;
                }
                i += matchingPrefix.length();
                col += matchingPrefix.length();
                previous = KTextEditor::Cursor(line, col);
                decToEncCurrentOffset = decToEncCurrentOffset - (isNullChar ? 0 : 1) + matchingPrefix.length();
                encToDecCurrentOffset = encToDecCurrentOffset - matchingPrefix.length() + (isNullChar ? 0 : 1);
                newI += (isNullChar ? 0 : 1);
                decToEncOffsetList.push_back(QPair<int, int>(newI, decToEncCurrentOffset));
                encToDecOffsetList.push_back(QPair<int, int>(i, encToDecCurrentOffset));
                continue;
            }
            ++col;
            ++i;
            ++newI;
        }
        ++i;
        ++newI;
    }
    if (previous < range.end()) {
        toReturn += text(KTextEditor::Range(previous, range.end()));
    }
    return toReturn;
}

void KTextEditor::DocumentPrivate::replaceCharactersByEncoding(KTextEditor::Range range)
{
    KateHighlighting *highlighting = highlight();

    const int rangeStartLine = range.start().line();
    const int rangeStartColumn = range.start().column();
    const int rangeEndLine = range.end().line();
    const int rangeEndColumn = range.end().column();

    for (int line = range.start().line(); line <= rangeEndLine; ++line) {
        const QString textLine = kateTextLine(line);
        int startColumn = (line == rangeStartLine) ? rangeStartColumn : 0;
        int endColumn = (line == rangeEndLine) ? rangeEndColumn : textLine.length();
        for (int col = startColumn; col < endColumn;) {
            int attr = m_buffer->attributeInLine(line, col);
            const QHash<QChar, QString> &reverseCharacterEncodingsHash = highlighting->getReverseCharacterEncodings(attr);
            auto it = reverseCharacterEncodingsHash.find(textLine.at(col));
            if (it != reverseCharacterEncodingsHash.end()) {
                replaceText(KTextEditor::Range(line, col, line, col + 1), *it);
                col += (*it).length();
                continue;
            }
            ++col;
        }
    }
}

//
// Highlighting information
//

QStringList KTextEditor::DocumentPrivate::embeddedHighlightingModes() const
{
    return highlight()->getEmbeddedHighlightingModes();
}

QString KTextEditor::DocumentPrivate::highlightingModeAt(const KTextEditor::Cursor &position)
{
    return highlight()->higlightingModeForLocation(this, position);
}

Kate::SwapFile *KTextEditor::DocumentPrivate::swapFile()
{
    return m_swapfile;
}

/**
 * \return \c -1 if \c line or \c column invalid, otherwise one of
 * standard style attribute number
 */
int KTextEditor::DocumentPrivate::defStyleNum(int line, int column)
{
    // Validate parameters to prevent out of range access
    if (line < 0 || line >= lines() || column < 0) {
        return -1;
    }

    // get highlighted line
    const QString tl = kateTextLine(line);
    // make sure the textline is a valid line
    if (tl.isEmpty()) {
        return -1;
    }

    // either get char attribute or attribute of context still active at end of line
    int attribute = 0;
    if (column < tl.length()) {
        attribute = m_buffer->attributeInLine(line, column);
    } else if (column == tl.length()) {
        if (!m_buffer->lineAttributes(line).isEmpty()) {
            attribute = m_buffer->lineAttributes(line).back().attributeValue;
        } else {
            return -1;
        }
    } else {
        return -1;
    }

    return highlight()->defaultStyleForAttribute(attribute);
}

bool KTextEditor::DocumentPrivate::isComment(int line, int column)
{
    const int defaultStyle = defStyleNum(line, column);
    return defaultStyle == KTextEditor::dsComment;
}

int KTextEditor::DocumentPrivate::findTouchedLine(int startLine, bool down)
{
    const int offset = down ? 1 : -1;
    const int lineCount = lines();
    while (startLine >= 0 && startLine < lineCount) {
        if (m_buffer->isLineModified(startLine) || m_buffer->isLineSaved(startLine)) {
            return startLine;
        }
        startLine += offset;
    }

    return -1;
}

void KTextEditor::DocumentPrivate::setActiveTemplateHandler(KateTemplateHandler *handler)
{
    // delete any active template handler
    delete m_activeTemplateHandler.data();
    m_activeTemplateHandler = handler;
}

// BEGIN KTextEditor::MessageInterface
bool KTextEditor::DocumentPrivate::postMessage(KTextEditor::Message *message)
{
    // no message -> cancel
    if (!message) {
        return false;
    }

    // make sure the desired view belongs to this document
    if (message->view() && message->view()->document() != this) {
        qCWarning(LOG_KTE) << "trying to post a message to a view of another document:" << message->text();
        return false;
    }

    message->setParent(this);
    message->setDocument(this);

    // if there are no actions, add a close action by default if widget does not auto-hide
    if (message->actions().count() == 0 && message->autoHide() < 0) {
        QAction *closeAction = new QAction(QIcon::fromTheme(QStringLiteral("window-close")), i18n("&Close"), nullptr);
        closeAction->setToolTip(i18n("Close message"));
        message->addAction(closeAction);
    }

    // reparent actions, as we want full control over when they are deleted
    QList<QSharedPointer<QAction>> managedMessageActions;
    const auto messageActions = message->actions();
    managedMessageActions.reserve(messageActions.size());
    for (QAction *action : messageActions) {
        action->setParent(nullptr);
        managedMessageActions.append(QSharedPointer<QAction>(action));
    }
    m_messageHash.insert(message, managedMessageActions);

    // post message to requested view, or to all views
    if (KTextEditor::ViewPrivate *view = qobject_cast<KTextEditor::ViewPrivate *>(message->view())) {
        view->postMessage(message, managedMessageActions);
    } else {
        for (auto view : std::as_const(m_views)) {
            view->postMessage(message, managedMessageActions);
        }
    }

    // also catch if the user manually calls delete message
    connect(message, &Message::closed, this, &DocumentPrivate::messageDestroyed);

    return true;
}

void KTextEditor::DocumentPrivate::messageDestroyed(KTextEditor::Message *message)
{
    // KTE:Message is already in destructor
    Q_ASSERT(m_messageHash.contains(message));
    m_messageHash.remove(message);
}
// END KTextEditor::MessageInterface
