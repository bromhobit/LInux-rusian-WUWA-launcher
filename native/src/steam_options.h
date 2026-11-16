#pragma once

#include <QString>

namespace EchoSteamOptions {
struct MergeResult {
    bool ok{};
    bool alreadyConfigured{};
    QString options;
    QString error;
};

// Does not access or change Steam configuration. On failure the caller retains
// the original options unchanged.
MergeResult merge(const QString &existing, const QString &executable);
int runSteamOptionsTests();
}
