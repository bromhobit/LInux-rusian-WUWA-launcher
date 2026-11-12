#pragma once

#include <QString>
#include <QList>
#include <QJsonObject>

namespace EchoInstall {
struct Result { bool ok{}; QString message; };
struct Inspection {
    bool owned{};
    bool healthy{};
    bool repairable{};
    bool reinstallable{};
    bool resourceVersionChanged{};
    bool selectedChannelDiffers{};
    bool packageDiffers{};
    QString channel;
    QString translationVersion;
    QString installedResourceVersion;
    QString currentResourceVersion;
    QString message;
};

QString sha1File(const QString &path, QString *error = nullptr);
QString detectResourceVersion(const QString &gameRoot);
QString translationMount(const QString &gameRoot, const QString &resourceVersion);
QString translationMountForInstall(const QString &gameRoot, const QString &resourceVersion, const QString &appDataPath);
QString englishMountPreflight(const QString &gameRoot, const QString &resourceVersion);
bool safeGameRoot(const QString &root, QString *error = nullptr);
struct GameProcess {
    qint64 pid{};
    QString name;
    bool updater{};
};
QList<GameProcess> blockingProcesses(const QString &gameRoot);
bool gameRunning(const QString &gameRoot);
bool terminateBackgroundUpdaters(const QString &gameRoot, QString *message);
bool transformMount(const QByteArray &original, const QByteArray &entry,
                    QByteArray *updated, bool remove, QString *error = nullptr);
Result install(const QString &gameRoot, const QString &resourceVersion,
               const QString &channel, const QString &translationVersion,
               const QString &cachedPak, const QString &xxh64,
               const QString &appDataPath);
Result uninstall(const QString &appDataPath);
QString installedStatus(const QString &appDataPath, const QString &gameRoot = {});
Inspection inspect(const QString &appDataPath, const QString &gameRoot,
                   const QString &currentResourceVersion, const QString &selectedChannel,
                   const QString &selectedXxh64 = {});
Result repair(const QString &appDataPath, const QString &currentResourceVersion);
int runAutoInstallTests();
}

