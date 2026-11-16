#pragma once
#include <QString>

namespace EchoFps {
struct Result { bool ok{}; QString message; };
// Call only while the game and its updater are stopped.
Result enable(const QString &gameRoot);
Result disable(const QString &gameRoot);
}
