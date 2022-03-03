/*
    SPDX-FileCopyrightText: 2011 Dominik Haumann <dhaumann@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "katemodifiedundo.h"

#include "katebuffer.h"
#include "katedocument.h"
#include "kateundomanager.h"

#include <ktexteditor/cursor.h>
#include <ktexteditor/view.h>

KateModifiedInsertText::KateModifiedInsertText(KTextEditor::DocumentPrivate *document, int line, int col, const QString &text)
    : KateEditInsertTextUndo(document, line, col, text)
{
    setFlag(RedoLine1Modified);
    if (document->buffer().isLineModified(line)) {
        setFlag(UndoLine1Modified);
    } else {
        setFlag(UndoLine1Saved);
    }
}

KateModifiedRemoveText::KateModifiedRemoveText(KTextEditor::DocumentPrivate *document, int line, int col, const QString &text)
    : KateEditRemoveTextUndo(document, line, col, text)
{
    setFlag(RedoLine1Modified);
    if (document->buffer().isLineModified(line)) {
        setFlag(UndoLine1Modified);
    } else {
        setFlag(UndoLine1Saved);
    }
}

KateModifiedWrapLine::KateModifiedWrapLine(KTextEditor::DocumentPrivate *document, int line, int col, int len, bool newLine)
    : KateEditWrapLineUndo(document, line, col, len, newLine)
{
    const bool modified = document->buffer().isLineModified(line);
    const bool saved = document->buffer().isLineSaved(line);
    if (len > 0 || modified) {
        setFlag(RedoLine1Modified);
    } else if (saved) {
        setFlag(RedoLine1Saved);
    }

    if (col > 0 || len == 0 || modified) {
        setFlag(RedoLine2Modified);
    } else if (saved) {
        setFlag(RedoLine2Saved);
    }

    if (modified) {
        setFlag(UndoLine1Modified);
    } else if ((len > 0 && col > 0) || saved) {
        setFlag(UndoLine1Saved);
    }
}

KateModifiedUnWrapLine::KateModifiedUnWrapLine(KTextEditor::DocumentPrivate *document, int line, int col, int len, bool removeLine)
    : KateEditUnWrapLineUndo(document, line, col, len, removeLine)
{
    const bool lineModified = document->buffer().isLineModified(line);
    const bool nextLineModified = document->buffer().isLineModified(line + 1);
    const bool lineSaved = document->buffer().isLineSaved(line);
    const bool nextLineSaved = document->buffer().isLineSaved(line + 1);

    const int len1 = document->lineLength(line);
    const int len2 = document->lineLength(line + 1);

    if (len1 > 0 && len2 > 0) {
        setFlag(RedoLine1Modified);

        if (lineModified) {
            setFlag(UndoLine1Modified);
        } else {
            setFlag(UndoLine1Saved);
        }

        if (nextLineModified) {
            setFlag(UndoLine2Modified);
        } else {
            setFlag(UndoLine2Saved);
        }
    } else if (len1 == 0) {
        if (nextLineModified) {
            setFlag(RedoLine1Modified);
        } else if (nextLineSaved) {
            setFlag(RedoLine1Saved);
        }

        if (lineModified) {
            setFlag(UndoLine1Modified);
        } else {
            setFlag(UndoLine1Saved);
        }

        if (nextLineModified) {
            setFlag(UndoLine2Modified);
        } else if (nextLineSaved) {
            setFlag(UndoLine2Saved);
        }
    } else { // len2 == 0
        if (nextLineModified) {
            setFlag(RedoLine1Modified);
        } else if (nextLineSaved) {
            setFlag(RedoLine1Saved);
        }

        if (lineModified) {
            setFlag(UndoLine1Modified);
        } else if (lineSaved) {
            setFlag(UndoLine1Saved);
        }

        if (nextLineModified) {
            setFlag(UndoLine2Modified);
        } else {
            setFlag(UndoLine2Saved);
        }
    }
}

KateModifiedInsertLine::KateModifiedInsertLine(KTextEditor::DocumentPrivate *document, int line, const QString &text)
    : KateEditInsertLineUndo(document, line, text)
{
    setFlag(RedoLine1Modified);
}

KateModifiedRemoveLine::KateModifiedRemoveLine(KTextEditor::DocumentPrivate *document, int line, const QString &text)
    : KateEditRemoveLineUndo(document, line, text)
{
    if (document->buffer().isLineModified(line)) {
        setFlag(UndoLine1Modified);
    } else {
        setFlag(UndoLine1Saved);
    }
}

void KateModifiedInsertText::undo()
{
    KateEditInsertTextUndo::undo();

    KTextEditor::DocumentPrivate *doc = document();

    doc->buffer().setLineModified(line(), isFlagSet(UndoLine1Modified));
    doc->buffer().setLineSaved(line(), isFlagSet(UndoLine1Saved));
}

void KateModifiedRemoveText::undo()
{
    KateEditRemoveTextUndo::undo();

    KTextEditor::DocumentPrivate *doc = document();

    doc->buffer().setLineModified(line(), isFlagSet(UndoLine1Modified));
    doc->buffer().setLineSaved(line(), isFlagSet(UndoLine1Saved));
}

void KateModifiedWrapLine::undo()
{
    KateEditWrapLineUndo::undo();

    KTextEditor::DocumentPrivate *doc = document();

    doc->buffer().setLineModified(line(), isFlagSet(UndoLine1Modified));
    doc->buffer().setLineSaved(line(), isFlagSet(UndoLine1Saved));
}

void KateModifiedUnWrapLine::undo()
{
    KateEditUnWrapLineUndo::undo();

    KTextEditor::DocumentPrivate *doc = document();

    doc->buffer().setLineModified(line(), isFlagSet(UndoLine1Modified));
    doc->buffer().setLineSaved(line(), isFlagSet(UndoLine1Saved));

    doc->buffer().setLineModified(line() + 1, isFlagSet(UndoLine2Modified));
    doc->buffer().setLineSaved(line() + 1, isFlagSet(UndoLine2Saved));
}

void KateModifiedInsertLine::undo()
{
    KateEditInsertLineUndo::undo();

    // no line modification needed, since the line is removed
}

void KateModifiedRemoveLine::undo()
{
    KateEditRemoveLineUndo::undo();

    KTextEditor::DocumentPrivate *doc = document();

    doc->buffer().setLineModified(line(), isFlagSet(UndoLine1Modified));
    doc->buffer().setLineSaved(line(), isFlagSet(UndoLine1Saved));
}

void KateModifiedRemoveText::redo()
{
    KateEditRemoveTextUndo::redo();

    KTextEditor::DocumentPrivate *doc = document();

    doc->buffer().setLineModified(line(), isFlagSet(RedoLine1Modified));
    doc->buffer().setLineSaved(line(), isFlagSet(RedoLine1Saved));
}

void KateModifiedInsertText::redo()
{
    KateEditInsertTextUndo::redo();

    KTextEditor::DocumentPrivate *doc = document();

    doc->buffer().setLineModified(line(), isFlagSet(RedoLine1Modified));
    doc->buffer().setLineSaved(line(), isFlagSet(RedoLine1Saved));
}

void KateModifiedUnWrapLine::redo()
{
    KateEditUnWrapLineUndo::redo();

    KTextEditor::DocumentPrivate *doc = document();

    doc->buffer().setLineModified(line(), isFlagSet(RedoLine1Modified));
    doc->buffer().setLineSaved(line(), isFlagSet(RedoLine1Saved));
}

void KateModifiedWrapLine::redo()
{
    KateEditWrapLineUndo::redo();

    KTextEditor::DocumentPrivate *doc = document();

    doc->buffer().setLineModified(line(), isFlagSet(RedoLine1Modified));
    doc->buffer().setLineSaved(line(), isFlagSet(RedoLine1Saved));

    doc->buffer().setLineModified(line() + 1, isFlagSet(RedoLine2Modified));
    doc->buffer().setLineSaved(line() + 1, isFlagSet(RedoLine2Saved));
}

void KateModifiedRemoveLine::redo()
{
    KateEditRemoveLineUndo::redo();

    // no line modification needed, since the line is removed
}

void KateModifiedInsertLine::redo()
{
    KateEditInsertLineUndo::redo();

    KTextEditor::DocumentPrivate *doc = document();

    doc->buffer().setLineModified(line(), isFlagSet(RedoLine1Modified));
    doc->buffer().setLineSaved(line(), isFlagSet(RedoLine1Saved));
}

void KateModifiedInsertText::updateRedoSavedOnDiskFlag(QBitArray &lines)
{
    if (line() >= lines.size()) {
        lines.resize(line() + 1);
    }

    if (!lines.testBit(line())) {
        lines.setBit(line());

        unsetFlag(RedoLine1Modified);
        setFlag(RedoLine1Saved);
    }
}

void KateModifiedInsertText::updateUndoSavedOnDiskFlag(QBitArray &lines)
{
    if (line() >= lines.size()) {
        lines.resize(line() + 1);
    }

    if (!lines.testBit(line())) {
        lines.setBit(line());

        unsetFlag(UndoLine1Modified);
        setFlag(UndoLine1Saved);
    }
}

void KateModifiedRemoveText::updateRedoSavedOnDiskFlag(QBitArray &lines)
{
    if (line() >= lines.size()) {
        lines.resize(line() + 1);
    }

    if (!lines.testBit(line())) {
        lines.setBit(line());

        unsetFlag(RedoLine1Modified);
        setFlag(RedoLine1Saved);
    }
}

void KateModifiedRemoveText::updateUndoSavedOnDiskFlag(QBitArray &lines)
{
    if (line() >= lines.size()) {
        lines.resize(line() + 1);
    }

    if (!lines.testBit(line())) {
        lines.setBit(line());

        unsetFlag(UndoLine1Modified);
        setFlag(UndoLine1Saved);
    }
}

void KateModifiedWrapLine::updateRedoSavedOnDiskFlag(QBitArray &lines)
{
    if (line() + 1 >= lines.size()) {
        lines.resize(line() + 2);
    }

    if (isFlagSet(RedoLine1Modified) && !lines.testBit(line())) {
        lines.setBit(line());

        unsetFlag(RedoLine1Modified);
        setFlag(RedoLine1Saved);
    }

    if (isFlagSet(RedoLine2Modified) && !lines.testBit(line() + 1)) {
        lines.setBit(line() + 1);

        unsetFlag(RedoLine2Modified);
        setFlag(RedoLine2Saved);
    }
}

void KateModifiedWrapLine::updateUndoSavedOnDiskFlag(QBitArray &lines)
{
    if (line() >= lines.size()) {
        lines.resize(line() + 1);
    }

    if (isFlagSet(UndoLine1Modified) && !lines.testBit(line())) {
        lines.setBit(line());

        unsetFlag(UndoLine1Modified);
        setFlag(UndoLine1Saved);
    }
}

void KateModifiedUnWrapLine::updateRedoSavedOnDiskFlag(QBitArray &lines)
{
    if (line() >= lines.size()) {
        lines.resize(line() + 1);
    }

    if (isFlagSet(RedoLine1Modified) && !lines.testBit(line())) {
        lines.setBit(line());

        unsetFlag(RedoLine1Modified);
        setFlag(RedoLine1Saved);
    }
}

void KateModifiedUnWrapLine::updateUndoSavedOnDiskFlag(QBitArray &lines)
{
    if (line() + 1 >= lines.size()) {
        lines.resize(line() + 2);
    }

    if (isFlagSet(UndoLine1Modified) && !lines.testBit(line())) {
        lines.setBit(line());

        unsetFlag(UndoLine1Modified);
        setFlag(UndoLine1Saved);
    }

    if (isFlagSet(UndoLine2Modified) && !lines.testBit(line() + 1)) {
        lines.setBit(line() + 1);

        unsetFlag(UndoLine2Modified);
        setFlag(UndoLine2Saved);
    }
}

void KateModifiedInsertLine::updateRedoSavedOnDiskFlag(QBitArray &lines)
{
    if (line() >= lines.size()) {
        lines.resize(line() + 1);
    }

    if (!lines.testBit(line())) {
        lines.setBit(line());

        unsetFlag(RedoLine1Modified);
        setFlag(RedoLine1Saved);
    }
}

void KateModifiedRemoveLine::updateUndoSavedOnDiskFlag(QBitArray &lines)
{
    if (line() >= lines.size()) {
        lines.resize(line() + 1);
    }

    if (!lines.testBit(line())) {
        lines.setBit(line());

        unsetFlag(UndoLine1Modified);
        setFlag(UndoLine1Saved);
    }
}
