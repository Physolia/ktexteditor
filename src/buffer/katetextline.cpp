/*
    SPDX-FileCopyrightText: 2010 Christoph Cullmann <cullmann@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "katetextline.h"

namespace Kate
{
TextLineData::TextLineData()
{
}

TextLineData::TextLineData(const QString &text)
    : m_text(text)
    , m_flags(0)
{
}

TextLineData::~TextLineData() = default;

int TextLineData::firstChar(const QString &text)
{
    return nextNonSpaceChar(text, 0);
}

int TextLineData::lastChar(const QString &text)
{
    return previousNonSpaceChar(text, text.length() - 1);
}

int TextLineData::nextNonSpaceChar(const QString &text, int pos)
{
    Q_ASSERT(pos >= 0);

    for (int i = pos; i < text.length(); i++) {
        if (!text[i].isSpace()) {
            return i;
        }
    }

    return -1;
}

int TextLineData::previousNonSpaceChar(const QString &text, int pos)
{
    if (pos >= text.length()) {
        pos = text.length() - 1;
    }

    for (int i = pos; i >= 0; i--) {
        if (!text[i].isSpace()) {
            return i;
        }
    }

    return -1;
}

QString TextLineData::leadingWhitespace(const QString &text)
{
    if (firstChar(text) < 0) {
        return text;
    }

    return text.mid(0, firstChar(text));
}

int TextLineData::indentDepth(const QString &text, int tabWidth)
{
    int d = 0;
    const int len = text.length();
    const QChar *unicode = text.unicode();

    for (int i = 0; i < len; ++i) {
        if (unicode[i].isSpace()) {
            if (unicode[i] == QLatin1Char('\t')) {
                d += tabWidth - (d % tabWidth);
            } else {
                d++;
            }
        } else {
            return d;
        }
    }

    return d;
}

bool TextLineData::matchesAt(const QString &text, int column, const QString &match)
{
    if (column < 0) {
        return false;
    }

    const int len = text.length();
    const int matchlen = match.length();

    if ((column + matchlen) > len) {
        return false;
    }

    const QChar *unicode = text.unicode();
    const QChar *matchUnicode = match.unicode();

    for (int i = 0; i < matchlen; ++i) {
        if (unicode[i + column] != matchUnicode[i]) {
            return false;
        }
    }

    return true;
}

int TextLineData::toVirtualColumn(const QString &text, int column, int tabWidth)
{
    if (column < 0) {
        return 0;
    }

    int x = 0;
    const int zmax = qMin(column, text.length());
    const QChar *unicode = text.unicode();

    for (int z = 0; z < zmax; ++z) {
        if (unicode[z] == QLatin1Char('\t')) {
            x += tabWidth - (x % tabWidth);
        } else {
            x++;
        }
    }

    return x + column - zmax;
}

int TextLineData::fromVirtualColumn(const QString &text, int column, int tabWidth)
{
    if (column < 0) {
        return 0;
    }

    const int zmax = qMin(text.length(), column);
    const QChar *unicode = text.unicode();

    int x = 0;
    int z = 0;
    for (; z < zmax; ++z) {
        int diff = 1;
        if (unicode[z] == QLatin1Char('\t')) {
            diff = tabWidth - (x % tabWidth);
        }

        if (x + diff > column) {
            break;
        }
        x += diff;
    }

    return z + qMax(column - x, 0);
}

int TextLineData::virtualLength(const QString &text, int tabWidth)
{
    int x = 0;
    const int len = text.length();
    const QChar *unicode = text.unicode();

    for (int z = 0; z < len; ++z) {
        if (unicode[z] == QLatin1Char('\t')) {
            x += tabWidth - (x % tabWidth);
        } else {
            x++;
        }
    }

    return x;
}

void TextLineData::addAttribute(const Attribute &attribute)
{
    // try to append to previous range, if same attribute value
    if (!m_attributesList.isEmpty() && (m_attributesList.back().attributeValue == attribute.attributeValue)
        && ((m_attributesList.back().offset + m_attributesList.back().length) == attribute.offset)) {
        m_attributesList.back().length += attribute.length;
        return;
    }

    m_attributesList.append(attribute);
}

short TextLineData::attribute(int pos) const
{
    auto found = std::upper_bound(m_attributesList.cbegin(), m_attributesList.cend(), pos, [](const int &p, const Attribute &x) {
        return p < x.offset + x.length;
    });
    if (found != m_attributesList.cend() && found->offset <= pos && pos < (found->offset + found->length)) {
        return found->attributeValue;
    }
    return 0;
}

}
