#pragma once
#include <QString>
#include <QStringList>

namespace EchoSteam {
struct SteamConfigRead {
    bool ok{};
    bool present{};
    QString options;
    QString error;
};
SteamConfigRead readSteamConfig(const QStringList &roots);
int runSteamConfigTests();
}
