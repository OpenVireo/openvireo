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

#include "AccountPlan.h"

#include <QCoreApplication>
#include <QLocale>

namespace AccountPlan
{
    const char* const AppStoreSubscriptionsUrl = "https://apps.apple.com/account/subscriptions";

    namespace
    {
        QString tr(const char* text)
        {
            return QCoreApplication::translate("AccountPlan", text);
        }

        QDateTime parseTime(const QJsonValue& value)
        {
            if (!value.isString()) {
                return {};
            }
            return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
        }

        QString longDate(const QDateTime& when)
        {
            return QLocale().toString(when.toLocalTime().date(), QStringLiteral("d MMMM yyyy"));
        }
    } // namespace

    Billing parseBilling(const QJsonObject& json)
    {
        Billing billing;
        billing.source = json.value("source").toString();
        billing.interval = json.value("interval").toString();
        billing.status = json.value("status").toString();
        // An older server that knows only Stripe: the website, as before
        billing.manage = json.value("manage").toString(billing.source == "apple" ? "app_store" : "website");
        billing.renewsAt = parseTime(json.value("renews_at"));
        billing.endsAt = parseTime(json.value("ends_at"));
        return billing;
    }

    Plan parse(const QJsonValue& value)
    {
        Plan plan;
        if (!value.isObject()) {
            return plan;
        }
        const auto json = value.toObject();
        plan.id = json.value("id").toString();
        const auto billing = json.value("billing");
        if (billing.isObject()) {
            plan.hasBilling = true;
            plan.billing = parseBilling(billing.toObject());
        }
        return plan;
    }

    QString displayName(const QString& id)
    {
        if (id == "free" || id == "org_free") {
            return tr("Free");
        }
        if (id == "premium") {
            return tr("Premium");
        }
        if (id == "families") {
            return tr("Families");
        }
        if (id == "teams") {
            return tr("Teams");
        }
        if (id == "business") {
            return tr("Business");
        }
        if (id.isEmpty()) {
            return {};
        }
        // A plan this version doesn't know yet: its id, capitalised
        return id.left(1).toUpper() + id.mid(1).replace('_', ' ');
    }

    QString summary(const Plan& plan)
    {
        if (!plan.isKnown()) {
            return {};
        }
        const auto name = displayName(plan.id);
        if (!plan.hasBilling) {
            return tr("%1 plan.").arg(name);
        }
        const auto& billing = plan.billing;
        QString when;
        const bool trial = billing.status == "trialing";
        if (billing.endsAt.isValid()) {
            when = trial ? tr("free trial, ends %1").arg(longDate(billing.endsAt))
                         : tr("ends %1").arg(longDate(billing.endsAt));
        } else if (billing.renewsAt.isValid()) {
            when = trial ? tr("free trial until %1, then billed %2")
                               .arg(longDate(billing.renewsAt),
                                    billing.interval == "year" ? tr("yearly") : tr("monthly"))
                         : tr("renews %1").arg(longDate(billing.renewsAt));
        } else if (trial) {
            when = tr("free trial");
        }
        if (billing.source == "apple") {
            if (trial && !billing.endsAt.isValid() && billing.renewsAt.isValid()) {
                return tr("%1, %2 by Apple.").arg(name, when);
            }
            return when.isEmpty() ? tr("%1, billed by Apple.").arg(name)
                                  : tr("%1, %2, billed by Apple.").arg(name, when);
        }
        return when.isEmpty() ? tr("%1.").arg(name) : tr("%1, %2.").arg(name, when);
    }

    QString manageHint(const Plan& plan)
    {
        if (!plan.isKnown()) {
            return {};
        }
        if (plan.isAppStore()) {
            return tr("Manage it on your iPhone or in your Apple ID settings.");
        }
        if (!plan.hasBilling) {
            return tr("Change it on the account website.");
        }
        return tr("Manage it on the account website.");
    }

    QString describe(const Plan& plan)
    {
        const auto first = summary(plan);
        const auto second = manageHint(plan);
        return second.isEmpty() ? first : first + ' ' + second;
    }

    QUrl manageUrl(const Plan& plan, const QString& issuer, const QString& orgId)
    {
        if (plan.isAppStore()) {
            return QUrl(QString::fromLatin1(AppStoreSubscriptionsUrl));
        }
        if (!plan.isKnown() || issuer.isEmpty()) {
            return {};
        }
        QUrl url(issuer);
        auto path = url.path();
        while (path.endsWith('/')) {
            path.chop(1);
        }
        url.setPath(orgId.isEmpty() ? path + "/subscription/pricing" : path + "/subscription/orgs/" + orgId);
        return url;
    }

    QString deletionNote(const QJsonObject& subscription)
    {
        const auto name = displayName(subscription.value("plan").toString());
        if (subscription.value("manage").toString() == "app_store") {
            return tr("Your %1 plan is billed by Apple and keeps renewing until you cancel it. Cancel it first on "
                      "your iPhone (Settings › Apple ID › Subscriptions) or at %2.")
                .arg(name, QString::fromLatin1(AppStoreSubscriptionsUrl));
        }
        return tr("Your %1 plan stops renewing.").arg(name);
    }
} // namespace AccountPlan
