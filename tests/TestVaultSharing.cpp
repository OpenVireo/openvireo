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

#include "TestVaultSharing.h"

#include "util/FakeAccountServer.h"

#include "account/AccountManager.h"
#include "account/ServiceAccountAdmin.h"
#include "account/VaultKeyring.h"
#include "account/VaultSharing.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "crypto/Crypto.h"
#include "crypto/kdf/AesKdf.h"
#include "crypto/kdf/Argon2Kdf.h"
#include "gui/remote/RemoteSettings.h"
#include "gui/remote/SyncTokenStore.h"
#include "keys/PasswordKey.h"

#include <QSignalSpy>
#include <QTest>

QTEST_GUILESS_MAIN(TestVaultSharing)

using namespace AccountCrypto;
using namespace FakeAccount;

namespace
{
    const QString AliceEmail = QStringLiteral("alice@example.com");
    const QString Password = QStringLiteral("correct horse battery staple");

    struct Fixture
    {
        QSharedPointer<Server> server = QSharedPointer<Server>::create();
        QScopedPointer<AccountManager> alice;
        QScopedPointer<VaultSharing> sharing;

        bool signUp()
        {
            alice.reset(createManager(server));
            alice->setServer(serverConfig());
            alice->registerAccount(AliceEmail, Password);
            QSignalSpy vaults(alice.data(), &AccountManager::vaultsChanged);
            return QTest::qWaitFor([&] { return alice->state() == AccountManager::State::Unlocked; }, 10000)
                   && vaults.wait(5000) && (sharing.reset(new VaultSharing(alice.data())), true);
        }

        // A vault of Alice's with a blob on the sync server
        QString createVault(const QString& name, QString* error = nullptr)
        {
            QString vaultId;
            bool finished = false;
            sharing->createVault(name, [&](const QString& id, const QSharedPointer<Database>&, const QString& message) {
                vaultId = id;
                if (error) {
                    *error = message;
                }
                finished = true;
            });
            QTest::qWaitFor([&] { return finished; }, 10000);
            // The keyring catches up once the vaults are reloaded
            QTest::qWaitFor([&] { return alice->keyring().contains(vaultId); }, 5000);
            return vaultId;
        }
    };

    bool run(VaultSharing* sharing,
             const std::function<void(VaultSharing*, VaultSharing::Done)>& operation,
             QString* error = nullptr)
    {
        bool finished = false;
        bool result = false;
        operation(sharing, [&](bool ok, const QString& message) {
            result = ok;
            if (error) {
                *error = message;
            }
            finished = true;
        });
        QTest::qWaitFor([&] { return finished; }, 10000);
        return finished && result;
    }

    VaultSharing::Recipient lookup(VaultSharing* sharing, const QString& email)
    {
        VaultSharing::Recipient found;
        bool finished = false;
        sharing->lookupRecipient(email, [&](const VaultSharing::Recipient& recipient, const QString&) {
            found = recipient;
            finished = true;
        });
        QTest::qWaitFor([&] { return finished; }, 5000);
        return found;
    }

    QStringList titles(Database* db)
    {
        QStringList result;
        for (const auto* entry : db->rootGroup()->entriesRecursive()) {
            result << entry->title();
        }
        result.sort();
        return result;
    }
} // namespace

void TestVaultSharing::initTestCase()
{
    QVERIFY(Crypto::init());
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
}

void TestVaultSharing::cleanup()
{
    restoreSyncFactory();
}

void TestVaultSharing::testSharesTheCurrentKeyWithACheckedRecipient()
{
    Fixture f;
    installSyncFactory(f.server);
    QVERIFY(f.signUp());
    const auto vaultId = f.createVault("Family");
    QVERIFY(!vaultId.isEmpty());

    QString bob;
    const auto bobKeys = f.server->addUser("bob@example.com", &bob);
    const auto recipient = lookup(f.sharing.data(), "bob@example.com");
    QCOMPARE(recipient.principal, bob);
    QVERIFY(recipient.verified);
    QCOMPARE(recipient.fingerprint(), fingerprint(bobKeys.publicKeys));

    QVERIFY(run(f.sharing.data(),
                [&](VaultSharing* s, VaultSharing::Done done) { s->grant(vaultId, recipient, "write", done); }));
    QCOMPARE(f.server->roleOf(vaultId, bob), QString("write"));
    // Bob can unseal exactly Alice's vault key, for the current version
    const auto key = f.server->vaultKeyFor(vaultId, bob, bobKeys.encSk);
    QVERIFY(!key.empty());
    QVERIFY(key == f.alice->keyring().vaultKey(vaultId));
    // and with it open the blob on the sync server
    QVERIFY(VaultKeyring::readVault(f.server->blobs.value(vaultId).data, VaultKeyring::compositeKeyFor(key)));
    // The server never saw the key in the clear
    QVERIFY(!f.server->vaults.value(vaultId).access.last().sealedKey.contains(toBase64Url(key)));
}

void TestVaultSharing::testRefusesKeysWithABadSignature()
{
    Fixture f;
    installSyncFactory(f.server);
    QVERIFY(f.signUp());
    const auto vaultId = f.createVault("Family");

    QString mallory;
    f.server->addUser("mallory@example.com", &mallory);
    // The server swaps in a key of its own, which the signature doesn't cover
    auto entry = f.server->directory.value(mallory);
    entry["enc_pk"] = toBase64Url(generateKeyset().publicKeys.encPk);
    f.server->directory.insert(mallory, entry);

    const auto recipient = lookup(f.sharing.data(), "mallory@example.com");
    QVERIFY(!recipient.verified);
    QString error;
    QVERIFY(!run(
        f.sharing.data(),
        [&](VaultSharing* s, VaultSharing::Done done) { s->grant(vaultId, recipient, "read", done); },
        &error));
    QVERIFY2(error.contains("signature"), qPrintable(error));
    QVERIFY(f.server->roleOf(vaultId, mallory).isEmpty());
}

void TestVaultSharing::testListsMembersWithFingerprints()
{
    Fixture f;
    installSyncFactory(f.server);
    QVERIFY(f.signUp());
    const auto vaultId = f.createVault("Family");
    QString bob;
    const auto bobKeys = f.server->addUser("bob@example.com", &bob);
    QVERIFY(run(f.sharing.data(), [&](VaultSharing* s, VaultSharing::Done done) {
        s->grant(vaultId, lookup(s, "bob@example.com"), "read", done);
    }));

    QList<VaultSharing::Member> members;
    bool finished = false;
    f.sharing->loadMembers(vaultId, [&](const QList<VaultSharing::Member>& list, const QString&) {
        members = list;
        finished = true;
    });
    QTRY_VERIFY(finished);
    QCOMPARE(members.size(), 2);
    QCOMPARE(members[0].principal, f.alice->principal());
    QCOMPARE(members[0].role, QString("manage"));
    QCOMPARE(members[0].keys.fingerprint(), fingerprint(f.alice->keyset().publicKeys));
    QCOMPARE(members[1].principal, bob);
    QCOMPARE(members[1].display, QString("bob@example.com"));
    QVERIFY(members[1].keys.verified);
    QCOMPARE(members[1].keys.fingerprint(), fingerprint(bobKeys.publicKeys));
    // 20 bytes as 10 groups of 4 hex digits
    QCOMPARE(members[1].keys.fingerprint().split(' ').size(), 10);
}

void TestVaultSharing::testRevokeRotatesTheKeyInOneStep()
{
    Fixture f;
    installSyncFactory(f.server);
    QVERIFY(f.signUp());
    const auto vaultId = f.createVault("Family");
    QString bob, carol;
    const auto bobKeys = f.server->addUser("bob@example.com", &bob);
    const auto carolKeys = f.server->addUser("carol@example.com", &carol);
    for (const auto& email : {QString("bob@example.com"), QString("carol@example.com")}) {
        QVERIFY(run(f.sharing.data(), [&](VaultSharing* s, VaultSharing::Done done) {
            s->grant(vaultId, lookup(s, email), "write", done);
        }));
    }
    const auto oldKey = f.alice->keyring().vaultKey(vaultId);
    const auto oldBlob = f.server->blobs.value(vaultId).data;

    QSignalSpy reloaded(f.alice.data(), &AccountManager::vaultsChanged);
    QString error;
    QVERIFY2(run(
                 f.sharing.data(),
                 [&](VaultSharing* s, VaultSharing::Done done) { s->revoke(vaultId, {bob}, done); },
                 &error),
             qPrintable(error));

    const auto vault = f.server->vaults.value(vaultId);
    QCOMPARE(vault.current, 2);
    QCOMPARE(vault.pending, -1);
    // Bob is gone, Carol and Alice hold only the new key
    QVERIFY(f.server->roleOf(vaultId, bob).isEmpty());
    QCOMPARE(f.server->roleOf(vaultId, carol), QString("write"));
    const auto newKey = f.server->vaultKeyFor(vaultId, carol, carolKeys.encSk, 2);
    QVERIFY(!newKey.empty());
    QVERIFY(newKey != oldKey);
    QVERIFY(f.server->vaultKeyFor(vaultId, bob, bobKeys.encSk, 2).empty());
    for (const auto& access : vault.access) {
        QCOMPARE(access.keyVersion, 2);
    }
    // The blob was re-keyed: the new key opens it, the old one (which Bob may have kept) doesn't
    const auto blob = f.server->blobs.value(vaultId).data;
    QVERIFY(blob != oldBlob);
    QVERIFY(VaultKeyring::readVault(blob, VaultKeyring::compositeKeyFor(newKey)));
    QVERIFY(!VaultKeyring::readVault(blob, VaultKeyring::compositeKeyFor(oldKey)));
    // The rotation ran its three steps in order
    const auto requests = f.server->requests.join('\n');
    const auto start = requests.indexOf("POST vaults/" + vaultId + "/keys\n");
    const auto upload = requests.indexOf("sync PUT vaults/" + vaultId, start);
    const auto activate = requests.indexOf("POST vaults/" + vaultId + "/keys/2/activate", upload);
    QVERIFY(start >= 0 && upload > start && activate > upload);
    // and Alice's keyring has the new key
    QTRY_VERIFY(f.alice->keyring().vault(vaultId).currentKeyVersion == 2);
    QVERIFY(f.alice->keyring().vaultKey(vaultId) == newKey);
}

void TestVaultSharing::testRotationRetriesWhenAnotherDeviceUploads()
{
    Fixture f;
    installSyncFactory(f.server);
    QVERIFY(f.signUp());
    const auto vaultId = f.createVault("Family");
    QString bob;
    f.server->addUser("bob@example.com", &bob);
    QVERIFY(run(f.sharing.data(), [&](VaultSharing* s, VaultSharing::Done done) {
        s->grant(vaultId, lookup(s, "bob@example.com"), "write", done);
    }));

    // Another device saves between the download and the re-keyed upload
    f.server->conflictsToInject = 1;
    QString error;
    QVERIFY2(run(
                 f.sharing.data(),
                 [&](VaultSharing* s, VaultSharing::Done done) { s->revoke(vaultId, {bob}, done); },
                 &error),
             qPrintable(error));
    QCOMPARE(f.server->requests.filter("sync PUT vaults/" + vaultId).size(), 3);
    QCOMPARE(f.server->vaults.value(vaultId).current, 2);
    QTRY_VERIFY(f.alice->keyring().vault(vaultId).currentKeyVersion == 2);
    QVERIFY(VaultKeyring::readVault(f.server->blobs.value(vaultId).data, f.alice->keyring().compositeKey(vaultId)));

    // A server that keeps refusing makes the rotation fail without activating the new key
    f.server->conflictsToInject = VaultSharing::MaxUploadAttempts;
    QVERIFY(!run(
        f.sharing.data(), [&](VaultSharing* s, VaultSharing::Done done) { s->rotate(vaultId, {}, done); }, &error));
    QVERIFY2(error.contains("Rotate it again"), qPrintable(error));
    QCOMPARE(f.server->vaults.value(vaultId).current, 2);
    QCOMPARE(f.server->vaults.value(vaultId).pending, 3);
}

void TestVaultSharing::testFinishesAPendingRotation()
{
    Fixture f;
    installSyncFactory(f.server);
    QVERIFY(f.signUp());
    const auto vaultId = f.createVault("Family");

    // A rotation that stopped before the upload
    f.server->conflictsToInject = VaultSharing::MaxUploadAttempts;
    QVERIFY(!run(f.sharing.data(), [&](VaultSharing* s, VaultSharing::Done done) { s->rotate(vaultId, {}, done); }));
    QCOMPARE(f.server->vaults.value(vaultId).pending, 2);
    // The old key still opens the blob, so nobody is locked out
    QVERIFY(VaultKeyring::readVault(f.server->blobs.value(vaultId).data, f.alice->keyring().compositeKey(vaultId, 1)));

    // Rotating again finishes it with the pending key instead of starting another
    QString error;
    QVERIFY2(
        run(
            f.sharing.data(), [&](VaultSharing* s, VaultSharing::Done done) { s->rotate(vaultId, {}, done); }, &error),
        qPrintable(error));
    QCOMPARE(f.server->vaults.value(vaultId).current, 2);
    QCOMPARE(f.server->vaults.value(vaultId).pending, -1);
    QTRY_VERIFY(f.alice->keyring().vault(vaultId).currentKeyVersion == 2);
    QVERIFY(VaultKeyring::readVault(f.server->blobs.value(vaultId).data, f.alice->keyring().compositeKey(vaultId)));
}

void TestVaultSharing::testMovesADatabaseToTheAccount()
{
    Fixture f;
    installSyncFactory(f.server);
    QVERIFY(f.signUp());

    // A local database with a password and a self-hosted sync server remote
    auto local = QSharedPointer<Database>::create();
    auto kdf = QSharedPointer<AesKdf>::create();
    kdf->setRounds(1);
    local->setKdf(kdf);
    auto password = QSharedPointer<CompositeKey>::create();
    password->addKey(QSharedPointer<PasswordKey>::create("local password"));
    local->setKey(password);
    local->rootGroup()->setName("Old name");
    auto* entry = new Entry();
    entry->setUuid(QUuid::createUuid());
    entry->setTitle("Bank");
    entry->setGroup(local->rootGroup());
    {
        RemoteSettings settings(local);
        auto* params = new RemoteParams();
        params->name = "home";
        params->type = RemoteParams::Type::SyncServer;
        params->serverUrl = SelfHostedUrl;
        params->serverDatabaseId = "personal";
        settings.addRemoteParams(params);
        auto* command = new RemoteParams();
        command->name = "scp";
        command->downloadCommand = "scp a b";
        settings.addRemoteParams(command);
        settings.saveSettings();
    }
    QByteArray before;
    QVERIFY(VaultKeyring::writeVault(local.data(), &before));

    QString vaultId, error;
    QSharedPointer<Database> vault;
    bool finished = false;
    f.sharing->moveToAccount(
        local, "Personal", [&](const QString& id, const QSharedPointer<Database>& db, const QString& e) {
            vaultId = id;
            vault = db;
            error = e;
            finished = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(finished, 10000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(vault);

    // The vault starts at key version 1 with Alice among the grantees
    const auto serverVault = f.server->vaults.value(vaultId);
    QCOMPARE(serverVault.current, 1);
    QCOMPARE(serverVault.access.size(), 1);
    QCOMPARE(serverVault.access.first().principal, f.alice->principal());
    QCOMPARE(serverVault.access.first().keyVersion, 1);
    QCOMPARE(serverVault.access.first().role, QString("manage"));
    // The vault name is encrypted with the vault key
    const auto vaultKey = f.server->vaultKeyFor(vaultId, f.alice->principal(), f.alice->keyset().encSk);
    QString name;
    QVERIFY(decryptVaultName(vaultKey, vaultId, fromBase64Url(serverVault.encryptedName), &name));
    QCOMPARE(name, QString("Personal"));

    // The upload is the database under the vault key, with the vault's KDF settings
    QVERIFY(f.server->requests.contains("sync PUT vaults/" + vaultId));
    auto uploaded =
        VaultKeyring::readVault(f.server->blobs.value(vaultId).data, VaultKeyring::compositeKeyFor(vaultKey));
    QVERIFY(uploaded);
    QCOMPARE(titles(uploaded.data()), QStringList{"Bank"});
    QCOMPARE(uploaded->rootGroup()->name(), QString("Personal"));
    auto argon = uploaded->kdf().dynamicCast<Argon2Kdf>();
    QVERIFY(argon);
    QCOMPARE(argon->type(), Argon2Kdf::Type::Argon2id);
    QCOMPARE(argon->memory(), VaultKeyring::VaultKdfMemoryKiB);
    QCOMPARE(argon->rounds(), VaultKeyring::VaultKdfIterations);
    QVERIFY(!VaultKeyring::readVault(f.server->blobs.value(vaultId).data, password));

    // It syncs as an account vault from now on; the sync server remote is gone, the command remote stays
    RemoteSettings settings(uploaded);
    settings.loadSettings();
    QStringList remotes;
    for (const auto* params : settings.getAllRemoteParams()) {
        remotes << params->name;
        if (params->type == RemoteParams::Type::AccountVault) {
            QCOMPARE(params->vaultId, vaultId);
            QCOMPARE(params->accountId, f.alice->accountId());
            QCOMPARE(params->serverUrl, SyncUrl);
        }
    }
    remotes.sort();
    QCOMPARE(remotes, QStringList({"KeePassXC account", "scp"}));

    // The original database wasn't touched
    QByteArray after;
    QVERIFY(VaultKeyring::writeVault(local.data(), &after));
    QVERIFY(VaultKeyring::readVault(after, password));
    QCOMPARE(local->rootGroup()->name(), QString("Old name"));
    QTRY_VERIFY(f.alice->keyring().contains(vaultId));
    QCOMPARE(f.alice->keyring().vault(vaultId).name, QString("Personal"));
}

void TestVaultSharing::testCreatesAnEmptyVault()
{
    Fixture f;
    installSyncFactory(f.server);
    QVERIFY(f.signUp());
    QString error;
    const auto vaultId = f.createVault("Work", &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // The upload went out only once the access token covered the new vault
    const auto put = f.server->requests.indexOf("sync PUT vaults/" + vaultId);
    QVERIFY(put > f.server->requests.indexOf("POST auth/refresh"));
    QCOMPARE(f.server->blobs.value(vaultId).revision, 1);
    QCOMPARE(f.alice->keyring().vault(vaultId).name, QString("Work"));
    QVERIFY(VaultKeyring::readVault(f.server->blobs.value(vaultId).data, f.alice->keyring().compositeKey(vaultId)));
}

void TestVaultSharing::testCreatesAServiceAccount()
{
    Fixture f;
    installSyncFactory(f.server);
    QVERIFY(f.signUp());
    QString admin;
    const auto adminKeys = f.server->addUser("admin@example.com", &admin);
    const auto orgId = f.server->addOrg("Acme", AliceEmail);
    f.server->addOrgMember(orgId, "admin@example.com", "admin");
    f.server->addUser("member@example.com");
    f.server->addOrgMember(orgId, "member@example.com", "member");

    ServiceAccountAdmin sas(f.alice.data());
    QList<ServiceAccountAdmin::Org> orgs;
    bool finished = false;
    sas.loadOrgs([&](const QList<ServiceAccountAdmin::Org>& list, const QString&) {
        orgs = list;
        finished = true;
    });
    QTRY_VERIFY(finished);
    QCOMPARE(orgs.size(), 1);
    QVERIFY(orgs.first().canAdminister());

    ServiceAccountAdmin::ServiceAccount account;
    QString token, error;
    finished = false;
    const auto expires = QDateTime::currentDateTimeUtc().addDays(90);
    sas.create(orgId,
               "CI deploy",
               expires,
               [&](const ServiceAccountAdmin::ServiceAccount& sa, const QString& t, const QString& e) {
                   account = sa;
                   token = t;
                   error = e;
                   finished = true;
               });
    QTRY_VERIFY(finished);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(token.startsWith("kpx_sa_"));
    QCOMPARE(account.name, QString("CI deploy"));

    ServiceAccountToken parsed;
    QVERIFY(parseServiceAccountToken(token, &parsed));
    QCOMPARE(parsed.saId, account.id);
    QCOMPARE(parsed.url, Issuer);
    // The server only knows the secret's hash and its prefix
    const auto stored = f.server->secrets.values();
    QCOMPARE(stored.size(), 1);
    QCOMPARE(stored.first().hash, parsed.secretHash());
    QCOMPARE(stored.first().prefix, toBase64Url(parsed.secret).left(8));
    QCOMPARE(stored.first().expiresAt.toString(), expires.toString(Qt::ISODate));
    // kek -> master -> keyset, as a CI runner would unwrap them
    SecureBytes master;
    QVERIFY(unwrap(parsed.kek, fromBase64Url(stored.first().wrappedMaster), saMasterAad(account.id), &master));
    Keyset keyset;
    QVERIFY(unwrapKeyset(master, f.server->serviceAccounts.value(account.id).json.value("keyset").toObject(), &keyset));
    QVERIFY(keyset.publicKeys.encPk == account.publicKeys.encPk);
    // The master key is sealed to the owner and the admin, not to a plain member
    const auto masterKeys = f.server->serviceAccounts.value(account.id).masterKeys;
    QCOMPARE(masterKeys.size(), 2);
    SecureBytes adminCopy;
    QVERIFY(unseal(adminKeys.encSk, fromBase64Url(masterKeys.value(admin)), saMasterAad(account.id), &adminCopy));
    QVERIFY(adminCopy == master);
    QVERIFY(masterKeys.contains(f.alice->principal()));

    // The token signs in
    auto* runner = createManager(f.server);
    QScopedPointer<AccountManager> guard(runner);
    runner->setServer(serverConfig());
    runner->signInServiceAccount(token);
    QTRY_COMPARE(runner->state(), AccountManager::State::Unlocked);
    QCOMPARE(runner->principal(), "sa:" + account.id);
}

void TestVaultSharing::testIssuesAndRevokesSecrets()
{
    Fixture f;
    installSyncFactory(f.server);
    QVERIFY(f.signUp());
    const auto orgId = f.server->addOrg("Acme", AliceEmail);
    ServiceAccountAdmin sas(f.alice.data());
    ServiceAccountAdmin::ServiceAccount account;
    bool finished = false;
    sas.create(
        orgId, "Backups", {}, [&](const ServiceAccountAdmin::ServiceAccount& sa, const QString&, const QString&) {
            account = sa;
            finished = true;
        });
    QTRY_VERIFY(finished);

    QString token, error;
    finished = false;
    sas.issueSecret(
        account.id, "laptop", {}, [&](const ServiceAccountAdmin::ServiceAccount&, const QString& t, const QString& e) {
            token = t;
            error = e;
            finished = true;
        });
    QTRY_VERIFY(finished);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(f.server->secrets.size(), 2);
    ServiceAccountToken parsed;
    QVERIFY(parseServiceAccountToken(token, &parsed));
    // The new secret wraps the same master key with its own kek
    QString secretId;
    for (const auto& secret : f.server->secrets) {
        if (secret.hash == parsed.secretHash()) {
            secretId = secret.id;
            QCOMPARE(secret.name, QString("laptop"));
            SecureBytes master;
            QVERIFY(unwrap(parsed.kek, fromBase64Url(secret.wrappedMaster), saMasterAad(account.id), &master));
        }
    }
    QVERIFY(!secretId.isEmpty());

    QList<ServiceAccountAdmin::Secret> secrets;
    finished = false;
    sas.listSecrets(account.id, [&](const QList<ServiceAccountAdmin::Secret>& list, const QString&) {
        secrets = list;
        finished = true;
    });
    QTRY_VERIFY(finished);
    QCOMPARE(secrets.size(), 2);

    finished = false;
    bool ok = false;
    sas.revokeSecret(account.id, secretId, [&](bool result, const QString&) {
        ok = result;
        finished = true;
    });
    QTRY_VERIFY(finished);
    QVERIFY(ok);
    QCOMPARE(f.server->secrets.size(), 1);

    // An expiry for the whole service account
    finished = false;
    const auto expires = QDateTime::currentDateTimeUtc().addDays(1);
    sas.setExpiry(account.id, expires, [&](bool result, const QString&) {
        ok = result;
        finished = true;
    });
    QTRY_VERIFY(finished);
    QVERIFY(ok);
    QCOMPARE(f.server->serviceAccounts.value(account.id).json.value("expires_at").toString(),
             expires.toString(Qt::ISODate));

    // Without a sealed copy of the master key, nobody can issue secrets
    f.server->serviceAccounts[account.id].masterKeys.clear();
    finished = false;
    sas.issueSecret(
        account.id, "", {}, [&](const ServiceAccountAdmin::ServiceAccount&, const QString& t, const QString& e) {
            token = t;
            error = e;
            finished = true;
        });
    QTRY_VERIFY(finished);
    QVERIFY(token.isEmpty());
    QVERIFY(error.contains("master key"));
}

void TestVaultSharing::testGrantsAServiceAccountAVault()
{
    Fixture f;
    installSyncFactory(f.server);
    QVERIFY(f.signUp());
    const auto vaultId = f.createVault("Deploy keys");
    const auto orgId = f.server->addOrg("Acme", AliceEmail);
    ServiceAccountAdmin sas(f.alice.data());
    ServiceAccountAdmin::ServiceAccount account;
    QString token;
    bool finished = false;
    sas.create(orgId, "CI", {}, [&](const ServiceAccountAdmin::ServiceAccount& sa, const QString& t, const QString&) {
        account = sa;
        token = t;
        finished = true;
    });
    QTRY_VERIFY(finished);

    bool ok = false;
    finished = false;
    sas.grantVault(account, vaultId, "manage", [&](bool result, const QString&) {
        ok = result;
        finished = true;
    });
    QTRY_VERIFY(finished);
    QVERIFY(!ok);

    finished = false;
    sas.grantVault(account, vaultId, "read", [&](bool result, const QString&) {
        ok = result;
        finished = true;
    });
    QTRY_VERIFY(finished);
    QVERIFY(ok);
    QCOMPARE(f.server->roleOf(vaultId, "sa:" + account.id), QString("read"));

    // Signed in with its token, the service account holds the vault key
    QScopedPointer<AccountManager> runner(createManager(f.server));
    runner->setServer(serverConfig());
    runner->signInServiceAccount(token);
    QTRY_VERIFY(runner->keyring().contains(vaultId));
    QVERIFY(runner->keyring().vaultKey(vaultId) == f.alice->keyring().vaultKey(vaultId));
    QCOMPARE(runner->keyring().vault(vaultId).role, QString("read"));
}

void TestVaultSharing::testPagesTheAuditLog()
{
    Fixture f;
    installSyncFactory(f.server);
    QVERIFY(f.signUp());
    const auto orgId = f.server->addOrg("Acme", AliceEmail);
    for (int i = 0; i < ServiceAccountAdmin::AuditPageSize + 5; ++i) {
        f.server->log(f.alice->principal(), "vault.access.grant", QString::number(i));
    }
    ServiceAccountAdmin sas(f.alice.data());
    QList<ServiceAccountAdmin::AuditEvent> events;
    QString next;
    bool finished = false;
    sas.auditLog(
        orgId, {}, [&](const QList<ServiceAccountAdmin::AuditEvent>& page, const QString& cursor, const QString&) {
            events = page;
            next = cursor;
            finished = true;
        });
    QTRY_VERIFY(finished);
    QCOMPARE(events.size(), ServiceAccountAdmin::AuditPageSize);
    QCOMPARE(events.first().target, QString::number(ServiceAccountAdmin::AuditPageSize + 4));
    QCOMPARE(events.first().action, QString("vault.access.grant"));
    QVERIFY(events.first().at.isValid());
    QVERIFY(!next.isEmpty());

    finished = false;
    sas.auditLog(
        orgId, next, [&](const QList<ServiceAccountAdmin::AuditEvent>& page, const QString& cursor, const QString&) {
            events = page;
            next = cursor;
            finished = true;
        });
    QTRY_VERIFY(finished);
    QCOMPARE(events.size(), 5);
    QVERIFY(next.isEmpty());
}
