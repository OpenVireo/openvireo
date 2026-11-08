/*
 *  Copyright (C) 2026 Bongani Mbigi
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

#include "TestAccountPlan.h"

#include "account/AccountPlan.h"

#include <QJsonDocument>
#include <QLocale>
#include <QTest>

QTEST_GUILESS_MAIN(TestAccountPlan)

namespace
{
    QJsonObject json(const char* text)
    {
        return QJsonDocument::fromJson(text).object();
    }
} // namespace

void TestAccountPlan::initTestCase()
{
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedKingdom));
    // Noon UTC is the same date in every time zone the test machines use
    qputenv("TZ", "UTC");
}

void TestAccountPlan::testParseApple()
{
    const auto plan = AccountPlan::parse(json(R"({"id": "premium", "limits": {}, "billing": {
        "source": "apple", "interval": "year", "status": "active",
        "renews_at": "2027-11-08T12:00:00Z", "ends_at": null, "manage": "app_store"}})"));
    QCOMPARE(plan.id, QString("premium"));
    QVERIFY(plan.hasBilling);
    QVERIFY(plan.isAppStore());
    QCOMPARE(plan.billing.source, QString("apple"));
    QCOMPARE(plan.billing.interval, QString("year"));
    QCOMPARE(plan.billing.status, QString("active"));
    QCOMPARE(plan.billing.renewsAt, QDateTime(QDate(2027, 11, 8), QTime(12, 0), Qt::UTC));
    QVERIFY(!plan.billing.endsAt.isValid());
}

void TestAccountPlan::testParseStripe()
{
    const auto plan = AccountPlan::parse(json(R"({"id": "families", "billing": {
        "source": "stripe", "interval": "month", "status": "past_due",
        "renews_at": null, "ends_at": "2026-12-01T00:00:00.000Z", "manage": "website"}})"));
    QVERIFY(plan.hasBilling);
    QVERIFY(!plan.isAppStore());
    QCOMPARE(plan.billing.endsAt, QDateTime(QDate(2026, 12, 1), QTime(0, 0), Qt::UTC));

    // A server without `manage` still says who bills it
    QVERIFY(AccountPlan::parse(json(R"({"id": "premium", "billing": {"source": "apple"}})")).isAppStore());
    QVERIFY(!AccountPlan::parse(json(R"({"id": "premium", "billing": {"source": "stripe"}})")).isAppStore());
}

void TestAccountPlan::testParseFreeAndMissing()
{
    auto plan = AccountPlan::parse(json(R"({"id": "free", "limits": {}, "billing": null})"));
    QCOMPARE(plan.id, QString("free"));
    QVERIFY(!plan.hasBilling);
    QVERIFY(!plan.isAppStore());

    // An older server sends no billing at all
    plan = AccountPlan::parse(json(R"({"id": "premium", "limits": {}})"));
    QVERIFY(plan.isKnown());
    QVERIFY(!plan.hasBilling);

    // Billing off: id null
    plan = AccountPlan::parse(json(R"({"id": null, "limits": {}})"));
    QVERIFY(!plan.isKnown());
    QVERIFY(AccountPlan::describe(plan).isEmpty());

    QVERIFY(!AccountPlan::parse(QJsonValue()).isKnown());
    QVERIFY(!AccountPlan::parse(QJsonValue("premium")).isKnown());
}

void TestAccountPlan::testDescribe()
{
    auto plan = AccountPlan::parse(json(R"({"id": "premium", "billing": {"source": "apple", "interval": "year",
        "status": "active", "renews_at": "2027-11-08T12:00:00Z", "ends_at": null, "manage": "app_store"}})"));
    QCOMPARE(AccountPlan::describe(plan),
             QString("Premium, renews 8 November 2027, billed by Apple. Manage it on your iPhone or in your "
                     "Apple ID settings."));

    plan = AccountPlan::parse(json(R"({"id": "families", "billing": {"source": "stripe", "interval": "month",
        "status": "active", "renews_at": null, "ends_at": "2026-12-01T12:00:00Z", "manage": "website"}})"));
    QCOMPARE(AccountPlan::describe(plan), QString("Families, ends 1 December 2026. Manage it on the account website."));

    plan = AccountPlan::parse(json(R"({"id": "org_free", "billing": null})"));
    QCOMPARE(AccountPlan::describe(plan), QString("Free plan. Change it on the account website."));

    QCOMPARE(AccountPlan::displayName("some_new"), QString("Some new"));
}

void TestAccountPlan::testManageUrl()
{
    const auto apple = AccountPlan::parse(json(R"({"id": "premium", "billing": {"source": "apple",
        "manage": "app_store"}})"));
    QCOMPARE(AccountPlan::manageUrl(apple, "https://auth.example.com").toString(),
             QString("https://apps.apple.com/account/subscriptions"));

    const auto stripe = AccountPlan::parse(json(R"({"id": "families", "billing": {"source": "stripe",
        "manage": "website"}})"));
    QCOMPARE(AccountPlan::manageUrl(stripe, "https://auth.example.com/").toString(),
             QString("https://auth.example.com/subscription/pricing"));
    QCOMPARE(AccountPlan::manageUrl(stripe, "https://auth.example.com", "0b6e").toString(),
             QString("https://auth.example.com/subscription/orgs/0b6e"));
    QVERIFY(!AccountPlan::manageUrl(stripe, "").isValid());
    QVERIFY(!AccountPlan::manageUrl({}, "https://auth.example.com").isValid());
}

void TestAccountPlan::testDeletionNote()
{
    QCOMPARE(AccountPlan::deletionNote(json(R"({"plan": "premium", "period_end": null})")),
             QString("Your Premium plan stops renewing."));
    QCOMPARE(AccountPlan::deletionNote(json(R"({"plan": "premium", "source": "stripe", "manage": "website"})")),
             QString("Your Premium plan stops renewing."));
    const auto apple = AccountPlan::deletionNote(json(R"({"plan": "premium", "source": "apple",
        "manage": "app_store"})"));
    QVERIFY(apple.contains("billed by Apple"));
    QVERIFY(apple.contains("https://apps.apple.com/account/subscriptions"));
}
