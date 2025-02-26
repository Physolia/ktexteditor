/*
    This file is part of the KDE libraries
    SPDX-FileCopyrightText: 2010-2018 Dominik Haumann <dhaumann@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#ifndef KATE_MOVINGRANGE_TEST_H
#define KATE_MOVINGRANGE_TEST_H

#include <QObject>

class MovingRangeTest : public QObject
{
    Q_OBJECT

public:
    MovingRangeTest();
    ~MovingRangeTest() override;

private Q_SLOTS:
    void testFeedbackEmptyRange();
    void testFeedbackInvalidRange();
    void testFeedbackCaret();
    void testFeedbackMouse();
    void testLineRemoved();
    void testLineWrapOrUnwrapUpdateRangeForLineCache();
    void testMultiline();
    void testMultiblock();
    void benchCursorsInsertionRemoval();
    void benchCheckValidity_data();
    void benchCheckValidity();
    void benchRangeForLine();
    void testMultiblockRangeWithLineUnwrapping();
    void testRangeSurvivesDocument();
    void testRangeWithDynAttrNoCrash();
    void testNoCrashIfFeedbackWasClearedBeforeDtor();
    void testNoCrashIfDynAttrWasClearedBeforeDtor();
    void testNoCrashWithMultiblockRange();
    void testNoFlippedRange();
    void testBlockSplitAndMerge();
};

#endif // KATE_MOVINGRANGE_TEST_H
