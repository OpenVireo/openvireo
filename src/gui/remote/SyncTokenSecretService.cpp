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

#include "SyncTokenSecretService.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QEventLoop>
#include <QScopeGuard>
#include <QTimer>

namespace
{
    const QString ServiceName = QStringLiteral("org.freedesktop.secrets");
    const QString ServicePath = QStringLiteral("/org/freedesktop/secrets");
    const QString ServiceInterface = QStringLiteral("org.freedesktop.Secret.Service");
    const QString CollectionInterface = QStringLiteral("org.freedesktop.Secret.Collection");
    const QString ItemInterface = QStringLiteral("org.freedesktop.Secret.Item");
    const QString PromptInterface = QStringLiteral("org.freedesktop.Secret.Prompt");
    const QString SessionInterface = QStringLiteral("org.freedesktop.Secret.Session");
    const QString NoPath = QStringLiteral("/");

    // Calls normally answer at once; a few seconds covers a keyring that is being started
    constexpr int CallTimeoutMs = 10000;
    // Long enough to type the keyring password
    constexpr int PromptTimeoutMs = 120000;

    using StringMap = QMap<QString, QString>;

    // The Secret Service's (oayays) secret
    struct Secret
    {
        QDBusObjectPath session;
        QByteArray parameters;
        QByteArray value;
        QString contentType;
    };
} // namespace

Q_DECLARE_METATYPE(Secret)

namespace
{
    QDBusArgument& operator<<(QDBusArgument& argument, const Secret& secret)
    {
        argument.beginStructure();
        argument << secret.session << secret.parameters << secret.value << secret.contentType;
        argument.endStructure();
        return argument;
    }

    const QDBusArgument& operator>>(const QDBusArgument& argument, Secret& secret)
    {
        argument.beginStructure();
        argument >> secret.session >> secret.parameters >> secret.value >> secret.contentType;
        argument.endStructure();
        return argument;
    }
} // namespace

/*
 * Waits for a Secret Service prompt (usually the keyring's unlock dialog) to finish.
 */
class SecretServicePrompt : public QObject
{
    Q_OBJECT

public:
    // Returns false when the user dismissed the prompt or it timed out
    static bool run(const QDBusObjectPath& prompt, QVariant* result = nullptr)
    {
        if (prompt.path() == NoPath) {
            return true;
        }
        // Events keep flowing while waiting; a second prompt from one of them would stack up
        static bool running = false;
        if (running) {
            return false;
        }
        running = true;
        auto reset = qScopeGuard([] { running = false; });

        SecretServicePrompt waiter;
        auto bus = QDBusConnection::sessionBus();
        if (!bus.connect(ServiceName,
                         prompt.path(),
                         PromptInterface,
                         "Completed",
                         &waiter,
                         SLOT(onCompleted(bool, QDBusVariant)))) {
            return false;
        }
        auto call = QDBusMessage::createMethodCall(ServiceName, prompt.path(), PromptInterface, "Prompt");
        call << QString();
        if (bus.call(call, QDBus::Block, CallTimeoutMs).type() == QDBusMessage::ErrorMessage) {
            return false;
        }
        if (!waiter.m_done) {
            // The keyring shows its own window; keep the rest of the app from changing meanwhile
            QTimer::singleShot(PromptTimeoutMs, &waiter.m_loop, &QEventLoop::quit);
            waiter.m_loop.exec(QEventLoop::ExcludeUserInputEvents);
        }
        bus.disconnect(
            ServiceName, prompt.path(), PromptInterface, "Completed", &waiter, SLOT(onCompleted(bool, QDBusVariant)));
        if (result) {
            *result = waiter.m_result;
        }
        return waiter.m_done && !waiter.m_dismissed;
    }

private slots:
    void onCompleted(bool dismissed, const QDBusVariant& result)
    {
        m_done = true;
        m_dismissed = dismissed;
        m_result = result.variant();
        m_loop.quit();
    }

private:
    QEventLoop m_loop;
    bool m_done = false;
    bool m_dismissed = false;
    QVariant m_result;
};

namespace
{
    class SecretServiceBackend : public SyncTokenStore::Backend
    {
    public:
        SecretServiceBackend()
        {
            qDBusRegisterMetaType<Secret>();
            qDBusRegisterMetaType<StringMap>();
        }

        bool isAvailable() override
        {
            auto bus = QDBusConnection::sessionBus();
            if (!bus.isConnected() || !bus.interface()) {
                return false;
            }
            const auto owner = bus.interface()->serviceOwner(ServiceName);
            if (owner.isValid()) {
                return owner.value() != bus.baseService();
            }
            // Not running yet, but the bus starts it on the first call
            const auto activatable = bus.interface()->activatableServiceNames();
            return activatable.isValid() && activatable.value().contains(ServiceName);
        }

        QString load(const QString& key) override
        {
            auto items = findItems(key);
            if (items.isEmpty()) {
                return {};
            }

            const auto session = openSession();
            if (session.path() == NoPath) {
                return {};
            }
            auto call = QDBusMessage::createMethodCall(ServiceName, items.first().path(), ItemInterface, "GetSecret");
            call << QVariant::fromValue(session);
            const auto reply = send(call);
            closeSession(session);
            if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
                return {};
            }
            const auto secret = qdbus_cast<Secret>(reply.arguments().at(0));
            return QString::fromUtf8(secret.value);
        }

        bool save(const QString& key, const QString& token) override
        {
            const auto collection = defaultCollection();
            if (collection.path() == NoPath) {
                return false;
            }
            const auto session = openSession();
            if (session.path() == NoPath) {
                return false;
            }

            QVariantMap properties;
            properties.insert(ItemInterface + ".Label",
                              QCoreApplication::translate("SyncTokenStore", "KeePassXC sync server access token"));
            properties.insert(ItemInterface + ".Attributes", QVariant::fromValue(attributes(key)));
            Secret secret{session, {}, token.toUtf8(), QStringLiteral("text/plain")};

            auto call =
                QDBusMessage::createMethodCall(ServiceName, collection.path(), CollectionInterface, "CreateItem");
            call << properties << QVariant::fromValue(secret) << true;
            const auto reply = send(call);
            closeSession(session);
            if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() < 2) {
                return false;
            }
            const auto item = reply.arguments().at(0).value<QDBusObjectPath>();
            if (item.path() != NoPath) {
                return true;
            }
            // The collection is locked; the prompt unlocks it and then creates the item
            return SecretServicePrompt::run(reply.arguments().at(1).value<QDBusObjectPath>());
        }

        bool remove(const QString& key) override
        {
            auto ok = true;
            for (const auto& item : findItems(key)) {
                auto call = QDBusMessage::createMethodCall(ServiceName, item.path(), ItemInterface, "Delete");
                const auto reply = send(call);
                if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
                    ok = false;
                    continue;
                }
                ok = SecretServicePrompt::run(reply.arguments().at(0).value<QDBusObjectPath>()) && ok;
            }
            return ok;
        }

        QString description() const override
        {
            return QCoreApplication::translate("SyncTokenStore", "the system keyring");
        }

    private:
        static StringMap attributes(const QString& key)
        {
            return {{QStringLiteral("xdg:schema"), QStringLiteral("org.keepassxc.SyncToken")},
                    {QStringLiteral("service"), QStringLiteral("KeePassXC Sync")},
                    {QStringLiteral("account"), key}};
        }

        static QDBusMessage send(const QDBusMessage& call)
        {
            return QDBusConnection::sessionBus().call(call, QDBus::Block, CallTimeoutMs);
        }

        static QList<QDBusObjectPath> paths(const QVariant& argument)
        {
            return qdbus_cast<QList<QDBusObjectPath>>(argument);
        }

        // The key's items, unlocked; asks to unlock them when needed
        static QList<QDBusObjectPath> findItems(const QString& key)
        {
            auto call = QDBusMessage::createMethodCall(ServiceName, ServicePath, ServiceInterface, "SearchItems");
            call << QVariant::fromValue(attributes(key));
            const auto reply = send(call);
            if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() < 2) {
                return {};
            }
            auto unlocked = paths(reply.arguments().at(0));
            const auto locked = paths(reply.arguments().at(1));
            if (locked.isEmpty()) {
                return unlocked;
            }

            auto unlock = QDBusMessage::createMethodCall(ServiceName, ServicePath, ServiceInterface, "Unlock");
            unlock << QVariant::fromValue(locked);
            const auto unlockReply = send(unlock);
            if (unlockReply.type() == QDBusMessage::ErrorMessage || unlockReply.arguments().size() < 2) {
                return unlocked;
            }
            unlocked += paths(unlockReply.arguments().at(0));
            if (unlocked.size() < locked.size()) {
                QVariant result;
                if (SecretServicePrompt::run(unlockReply.arguments().at(1).value<QDBusObjectPath>(), &result)) {
                    unlocked += paths(result);
                }
            }
            return unlocked;
        }

        static QDBusObjectPath defaultCollection()
        {
            auto call = QDBusMessage::createMethodCall(ServiceName, ServicePath, ServiceInterface, "ReadAlias");
            call << QStringLiteral("default");
            const auto reply = send(call);
            if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
                return QDBusObjectPath(NoPath);
            }
            return reply.arguments().at(0).value<QDBusObjectPath>();
        }

        // A "plain" session: the token crosses the user's own session bus unencrypted, which
        // only processes that could read the user's files anyway can watch
        static QDBusObjectPath openSession()
        {
            auto call = QDBusMessage::createMethodCall(ServiceName, ServicePath, ServiceInterface, "OpenSession");
            call << QStringLiteral("plain") << QVariant::fromValue(QDBusVariant(QString()));
            const auto reply = send(call);
            if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() < 2) {
                return QDBusObjectPath(NoPath);
            }
            return reply.arguments().at(1).value<QDBusObjectPath>();
        }

        static void closeSession(const QDBusObjectPath& session)
        {
            send(QDBusMessage::createMethodCall(ServiceName, session.path(), SessionInterface, "Close"));
        }
    };
} // namespace

std::unique_ptr<SyncTokenStore::Backend> createSecretServiceBackend()
{
    return std::make_unique<SecretServiceBackend>();
}

#include "SyncTokenSecretService.moc"
