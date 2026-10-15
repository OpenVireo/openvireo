/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "TestSyncScheduler.h"

#include "gui/remote/SyncEventStream.h"
#include "gui/remote/SyncScheduler.h"

#include <QSignalSpy>
#include <QTest>

QTEST_GUILESS_MAIN(TestSyncScheduler)

namespace
{
    // Long enough that only the timer under test fires
    constexpr int Never = 60 * 60 * 1000;
} // namespace

void TestSyncScheduler::testDisabledNeverRequests()
{
    SyncScheduler scheduler;
    scheduler.setSaveDelay(10);
    scheduler.setPollInterval(10);
    QSignalSpy requested(&scheduler, &SyncScheduler::syncRequested);

    scheduler.notifySaved();
    scheduler.requestNow();
    scheduler.notifyEvent();
    QTest::qWait(50);
    QCOMPARE(requested.count(), 0);

    // Disabling drops a pending save
    scheduler.setEnabled(true);
    scheduler.setPollInterval(Never);
    scheduler.notifySaved();
    scheduler.setEnabled(false);
    QTest::qWait(50);
    QCOMPARE(requested.count(), 0);
}

void TestSyncScheduler::testSavesAreFoldedIntoOneRequest()
{
    SyncScheduler scheduler;
    scheduler.setSaveDelay(100);
    scheduler.setPollInterval(Never);
    scheduler.setEnabled(true);
    QSignalSpy requested(&scheduler, &SyncScheduler::syncRequested);

    for (int i = 0; i < 4; ++i) {
        scheduler.notifySaved();
        QTest::qWait(30);
    }
    QCOMPARE(requested.count(), 0);
    QTRY_COMPARE(requested.count(), 1);
    QTest::qWait(150);
    QCOMPARE(requested.count(), 1);
}

void TestSyncScheduler::testRequestNowIsImmediate()
{
    SyncScheduler scheduler;
    scheduler.setSaveDelay(50);
    scheduler.setPollInterval(Never);
    scheduler.setEnabled(true);
    QSignalSpy requested(&scheduler, &SyncScheduler::syncRequested);

    scheduler.requestNow();
    QCOMPARE(requested.count(), 1);

    // A request covers a save that was waiting
    scheduler.notifySaved();
    scheduler.requestNow();
    QCOMPARE(requested.count(), 2);
    QTest::qWait(100);
    QCOMPARE(requested.count(), 2);
}

void TestSyncScheduler::testPollsWhileEnabled()
{
    SyncScheduler scheduler;
    scheduler.setPollInterval(40);
    QSignalSpy requested(&scheduler, &SyncScheduler::syncRequested);

    scheduler.setEnabled(true);
    QTRY_VERIFY(requested.count() >= 2);

    scheduler.setEnabled(false);
    const auto count = requested.count();
    QTest::qWait(120);
    QCOMPARE(requested.count(), count);
}

void TestSyncScheduler::testEventsAreRateLimited()
{
    SyncScheduler scheduler;
    scheduler.setPollInterval(Never);
    scheduler.setMinEventGap(100);
    scheduler.setEnabled(true);
    QSignalSpy requested(&scheduler, &SyncScheduler::syncRequested);

    // The first event syncs, a second right after doesn't
    scheduler.notifyEvent();
    scheduler.notifyEvent();
    QCOMPARE(requested.count(), 1);

    // Nor does one right after any other sync
    QTest::qWait(120);
    scheduler.requestNow();
    scheduler.notifyEvent();
    QCOMPARE(requested.count(), 2);

    QTest::qWait(120);
    scheduler.notifyEvent();
    QCOMPARE(requested.count(), 3);
}

void TestSyncScheduler::testParsesServerEvents()
{
    QByteArray buffer("retry: 5000\n: connected\n\n"
                      "event: database\ndata: {\"id\":\"personal\",\"etag\":\"\\\"abc\\\"\",\"revision\":7}\n\n"
                      ": ping\n\n"
                      "event: database\r\ndata: {\"id\":\"work\",\"deleted\":true}\r\n\r\n"
                      "event: other\ndata: {\"id\":\"ignored\"}\n\n"
                      "event: database\ndata: {\"id\":\"par");

    auto events = SyncEventStream::takeEvents(buffer);
    QCOMPARE(events.size(), 2);
    QCOMPARE(events[0].databaseId, QString("personal"));
    QCOMPARE(events[0].etag, QString("\"abc\""));
    QCOMPARE(events[0].revision, qint64(7));
    QVERIFY(!events[0].deleted);
    QCOMPARE(events[1].databaseId, QString("work"));
    QVERIFY(events[1].deleted);

    // The partial event waits for the rest
    QCOMPARE(buffer, QByteArray("event: database\ndata: {\"id\":\"par"));
    buffer += "tial\"}\n\n";
    events = SyncEventStream::takeEvents(buffer);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].databaseId, QString("partial"));
    QVERIFY(buffer.isEmpty());
}
