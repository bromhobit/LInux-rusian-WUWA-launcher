#include "fps_unlock.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <sqlite3.h>

namespace EchoFps {
namespace {
constexpr auto Backup = "echo_fps_original_v1";
constexpr auto Trigger = "echo_fps_keep_120_v1";
constexpr auto CreateBackup = "CREATE TABLE echo_fps_original_v1 (id INTEGER PRIMARY KEY CHECK(id=1), value)";
constexpr auto CreateTrigger =
    "CREATE TRIGGER echo_fps_keep_120_v1 AFTER UPDATE OF value ON LocalStorage "
    "WHEN NEW.key = 'CustomFrameRate' AND CAST(NEW.value AS TEXT) IS NOT '120' "
    "BEGIN UPDATE LocalStorage SET value = 120 WHERE key = 'CustomFrameRate'; END";

struct Database {
    sqlite3 *db{};
    ~Database() { if (db) sqlite3_close(db); }
};
struct Statement {
    sqlite3_stmt *stmt{};
    ~Statement() { if (stmt) sqlite3_finalize(stmt); }
};
bool execute(sqlite3 *db, const char *sql, QString *error) {
    char *message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc == SQLITE_OK) return true;
    *error = message ? QString::fromUtf8(message) : QString::fromUtf8(sqlite3_errmsg(db));
    sqlite3_free(message);
    return false;
}
bool prepare(sqlite3 *db, const char *sql, Statement *out, QString *error) {
    if (sqlite3_prepare_v2(db, sql, -1, &out->stmt, nullptr) == SQLITE_OK) return true;
    *error = QString::fromUtf8(sqlite3_errmsg(db));
    return false;
}
bool oneInteger(sqlite3 *db, const char *sql, qint64 *value, QString *error) {
    Statement s;
    if (!prepare(db, sql, &s, error)) return false;
    if (sqlite3_step(s.stmt) != SQLITE_ROW) { *error = QString::fromUtf8(sqlite3_errmsg(db)); return false; }
    *value = sqlite3_column_int64(s.stmt, 0);
    if (sqlite3_step(s.stmt) != SQLITE_DONE) { *error = "Неожиданный результат запроса."; return false; }
    return true;
}
QString databasePath(const QString &root, QString *error) {
    const QFileInfo rootInfo(root);
    if (!rootInfo.isDir() || rootInfo.canonicalFilePath().isEmpty()) {
        *error = "Папка клиента игры не найдена."; return {};
    }
    QString path = rootInfo.canonicalFilePath();
    for (const auto &part : QStringList{"Client", "Saved", "LocalStorage", "LocalStorage.db"}) {
        path = QDir(path).filePath(part);
        const QFileInfo info(path);
        if (info.isSymLink() || !info.exists()) { *error = "Файл настроек отсутствует или путь содержит символическую ссылку: " + path; return {}; }
        if (part == "LocalStorage.db" ? !info.isFile() : !info.isDir()) {
            *error = "Неожиданный тип объекта: " + path; return {};
        }
    }
    // A second database often indicates recovery after an unclean shutdown.
    const auto dbs = QDir(QFileInfo(path).absolutePath()).entryList({"LocalStorage*.db"}, QDir::Files | QDir::NoSymLinks);
    if (dbs.size() != 1 || dbs.first() != "LocalStorage.db") {
        *error = "Найдено несколько файлов LocalStorage. Проверьте настройки игры вручную."; return {};
    }
    return path;
}
bool openDatabase(const QString &root, Database *database, QString *error) {
    const QString path = databasePath(root, error);
    if (path.isEmpty()) return false;
    const QByteArray native = QFile::encodeName(path);
    if (sqlite3_open_v2(native.constData(), &database->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        *error = database->db ? QString::fromUtf8(sqlite3_errmsg(database->db)) : "Не удалось открыть базу настроек.";
        return false;
    }
    sqlite3_busy_timeout(database->db, 2000);
    if (!execute(database->db, "PRAGMA recursive_triggers=OFF", error)) return false;
    Statement check;
    if (!prepare(database->db, "PRAGMA quick_check", &check, error)) return false;
    if (sqlite3_step(check.stmt) != SQLITE_ROW || QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(check.stmt, 0))) != "ok" || sqlite3_step(check.stmt) != SQLITE_DONE) {
        *error = "Проверка целостности базы настроек не прошла."; return false;
    }
    return true;
}
bool namedObjectCount(sqlite3 *db, const char *name, qint64 *count, QString *error) {
    Statement s;
    if (!prepare(db, "SELECT count(*) FROM sqlite_master WHERE name=? COLLATE NOCASE", &s, error)) return false;
    sqlite3_bind_text(s.stmt, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(s.stmt) != SQLITE_ROW) { *error = QString::fromUtf8(sqlite3_errmsg(db)); return false; }
    *count = sqlite3_column_int64(s.stmt, 0);
    return true;
}
bool otherFpsTriggers(sqlite3 *db, QString *error) {
    Statement s;
    if (!prepare(db, "SELECT name, sql FROM sqlite_master WHERE type='trigger' AND tbl_name='LocalStorage' COLLATE NOCASE", &s, error)) return true;
    int rc;
    while ((rc = sqlite3_step(s.stmt)) == SQLITE_ROW) {
        const auto name = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(s.stmt, 0)));
        const auto sql = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(s.stmt, 1)));
        if (name.compare(Trigger, Qt::CaseInsensitive) != 0) {
            Q_UNUSED(sql)
            *error = "Обнаружен другой триггер базы настроек: " + name + ". Автоматическое изменение остановлено.";
            return true;
        }
    }
    if (rc != SQLITE_DONE) { *error = QString::fromUtf8(sqlite3_errmsg(db)); return true; }
    return false;
}
bool ownedState(sqlite3 *db, bool *owned, QString *error) {
    qint64 tableCount = 0, triggerCount = 0;
    if (!namedObjectCount(db, Backup, &tableCount, error) || !namedObjectCount(db, Trigger, &triggerCount, error)) return false;
    if (!tableCount && !triggerCount) { *owned = false; return true; }
    if (tableCount != 1 || triggerCount != 1) { *error = "Состояние разблокировки неполное; автоматическое изменение остановлено."; return false; }
    Statement table;
    if (!prepare(db, "SELECT sql FROM sqlite_master WHERE type='table' AND name='echo_fps_original_v1'", &table, error)) return false;
    if (sqlite3_step(table.stmt) != SQLITE_ROW || QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(table.stmt, 0))) != QString::fromLatin1(CreateBackup)) {
        *error = "Резервная таблица ECHO изменена; автоматическое изменение остановлено."; return false;
    }
    Statement trigger;
    if (!prepare(db, "SELECT sql FROM sqlite_master WHERE type='trigger' AND name='echo_fps_keep_120_v1'", &trigger, error)) return false;
    if (sqlite3_step(trigger.stmt) != SQLITE_ROW || QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(trigger.stmt, 0))) != QString::fromLatin1(CreateTrigger)) {
        *error = "Триггер ECHO изменён; автоматическое изменение остановлено."; return false;
    }
    qint64 backups = 0;
    if (!oneInteger(db, "SELECT count(*) FROM echo_fps_original_v1", &backups, error) || backups != 1 ||
        !oneInteger(db, "SELECT count(*) FROM echo_fps_original_v1 WHERE id=1", &backups, error) || backups != 1) {
        *error = "Резервное значение ECHO отсутствует или повреждено."; return false;
    }
    *owned = true;
    return true;
}
Result change(const QString &root, bool enable) {
    QString error;
    Database connection;
    if (!openDatabase(root, &connection, &error)) return {false, error};
    auto *db = connection.db;
    if (!execute(db, "BEGIN IMMEDIATE", &error)) return {false, "База занята: " + error};
    auto fail = [&](const QString &message) { QString ignored; execute(db, "ROLLBACK", &ignored); return Result{false, message}; };
    qint64 rows = 0;
    if (!oneInteger(db, "SELECT count(*) FROM LocalStorage WHERE key='CustomFrameRate'", &rows, &error) || rows != 1)
        return fail("Не найдена единственная настройка CustomFrameRate. Возможно, формат игры изменился.");
    if (otherFpsTriggers(db, &error)) return fail(error);
    bool owned = false;
    if (!ownedState(db, &owned, &error)) return fail(error);
    if (owned) {
        qint64 matches = 0;
        if (!oneInteger(db, "SELECT count(*) FROM LocalStorage WHERE key='CustomFrameRate' AND value=120", &matches, &error) || matches != 1)
            return fail("Значение FPS изменилось после применения ECHO. Автоматическое восстановление остановлено.");
    }
    if (enable && owned) { QString ignored; execute(db, "ROLLBACK", &ignored); return {true, "Разблокировка ECHO уже включена."}; }
    if (!enable && !owned) { QString ignored; execute(db, "ROLLBACK", &ignored); return {false, "Разблокировка ECHO не найдена; прежний лимит неизвестен."}; }
    if (enable) {
        if (!execute(db, CreateBackup, &error) ||
            !execute(db, "INSERT INTO echo_fps_original_v1 SELECT 1,value FROM LocalStorage WHERE key='CustomFrameRate'", &error) ||
            !execute(db, CreateTrigger, &error) ||
            !execute(db, "UPDATE LocalStorage SET value=120 WHERE key='CustomFrameRate'", &error) || sqlite3_changes(db) != 1)
            return fail("Не удалось включить 120 FPS: " + error);
    } else {
        if (!execute(db, "DROP TRIGGER echo_fps_keep_120_v1", &error) ||
            !execute(db, "UPDATE LocalStorage SET value=(SELECT value FROM echo_fps_original_v1 WHERE id=1) WHERE key='CustomFrameRate'", &error) || sqlite3_changes(db) != 1 ||
            !execute(db, "DROP TABLE echo_fps_original_v1", &error))
            return fail("Не удалось восстановить прежний лимит: " + error);
    }
    if (!execute(db, "COMMIT", &error)) return fail("Не удалось сохранить настройки: " + error);
    return {true, enable ? "Лимит 120 FPS записан. После запуска проверьте фактический FPS: совместимость с текущей версией игры не подтверждена." : "Прежнее значение лимита восстановлено."};
}
} // namespace
Result enable(const QString &gameRoot) { return change(gameRoot, true); }
Result disable(const QString &gameRoot) { return change(gameRoot, false); }
} // namespace EchoFps
