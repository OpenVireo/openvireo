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

#ifndef KEEPASSXC_ACCOUNTPLAN_H
#define KEEPASSXC_ACCOUNTPLAN_H

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QUrl>

/**
 * The plan of a user or an org as GET /me and GET /orgs/{id} describe it (SPEC §19): its id and, for a
 * paid plan, who bills it. The desktop app never sells or changes a plan; it says what the plan is and
 * where to manage it: the account website for Stripe, the person's Apple ID for In-App Purchase.
 */
namespace AccountPlan
{
    // Where an Apple ID's subscriptions are managed outside the iPhone
    extern const char* const AppStoreSubscriptionsUrl;

    struct Billing
    {
        // stripe or apple
        QString source;
        // month or year
        QString interval;
        // active, trialing or past_due
        QString status;
        // website or app_store; what clients follow, not `source`
        QString manage;
        QDateTime renewsAt;
        QDateTime endsAt;

        bool isAppStore() const
        {
            return manage == "app_store";
        }
    };

    struct Plan
    {
        // free, premium, org_free, families, teams or business; empty when billing is off or unknown
        QString id;
        // Set for a paid plan; absent on a free plan, when billing is off or from an older server
        bool hasBilling = false;
        Billing billing;

        bool isKnown() const
        {
            return !id.isEmpty();
        }
        bool isAppStore() const
        {
            return hasBilling && billing.isAppStore();
        }
    };

    // `plan` from GET /me, an entry of its `orgs` or GET /orgs/{id}; anything missing is left empty
    Plan parse(const QJsonValue& plan);
    Billing parseBilling(const QJsonObject& billing);

    // "Premium", "Families", "Free" …
    QString displayName(const QString& id);
    // "Premium, renews 8 November 2027, billed by Apple." or "Free plan."
    QString summary(const Plan& plan);
    // "Manage it on your iPhone or in your Apple ID settings." / "Manage it on the account website."
    QString manageHint(const Plan& plan);
    // summary() and manageHint() together
    QString describe(const Plan& plan);
    // The Apple ID subscriptions page, or the auth site's billing page (`orgId` empty for the user's plan)
    QUrl manageUrl(const Plan& plan, const QString& issuer, const QString& orgId = {});
    // A line for the Delete Account summary about `subscription` in GET /me/deletion (SPEC §23)
    QString deletionNote(const QJsonObject& subscription);
} // namespace AccountPlan

#endif // KEEPASSXC_ACCOUNTPLAN_H
