#include "fps_unlock.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <sqlite3.h>

namespace EchoFps {
namespace {
constexpr auto Backup = "echo_fps_original_v1";
constexpr auto Trigger = "echo_fps_keep_120_v1";
constexpr auto CreateBackup = "CREATE TABLE echo_fps_original_v1 (id INTEGER PRIMARY KEY CHECK(id=1), value)";
constexpr auto MenuBackup = "echo_fps_menu_v1";
constexpr auto CreateMenuBackup = "CREATE TABLE echo_fps_menu_v1 (key TEXT PRIMARY KEY, value NOT NULL)";
constexpr auto IniBackup = "echo_fps_ini_v1";
constexpr auto CreateIniBackup = "CREATE TABLE echo_fps_ini_v1 (id INTEGER PRIMARY KEY CHECK(id=1), original BLOB NOT NULL, applied BLOB NOT NULL)";
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
struct IniFile {
    QString path;
    QByteArray content;
    QList<QByteArray> lines;
    int line{-1};
    QByteArray original;
    QByteArray applied;
};
bool sameIniLimit(const QByteArray &left, const QByteArray &right) {
    static const QRegularExpression re(QStringLiteral(R"(^\s*FrameRateLimit\s*=\s*([+-]?(?:\d+(?:\.\d*)?|\.\d+))\s*(?:[;#].*)?\r?$)"), QRegularExpression::CaseInsensitiveOption);
    const auto a = re.match(QString::fromLatin1(left));
    const auto b = re.match(QString::fromLatin1(right));
    if (!a.hasMatch() || !b.hasMatch()) return false;
    bool okA = false, okB = false;
    const double valueA = a.captured(1).toDouble(&okA), valueB = b.captured(1).toDouble(&okB);
    return okA && okB && valueA == valueB;
}
QByteArray replaceIniLimit(const QByteArray &line, const QByteArray &target) {
    static const QRegularExpression re(QStringLiteral(R"(^(\s*FrameRateLimit\s*=\s*)([+-]?(?:\d+(?:\.\d*)?|\.\d+))(\s*(?:[;#].*)?\r?)$)"), QRegularExpression::CaseInsensitiveOption);
    const auto current = re.match(QString::fromLatin1(line));
    const auto saved = re.match(QString::fromLatin1(target));
    if (!current.hasMatch() || !saved.hasMatch()) return {};
    return (current.captured(1) + saved.captured(2) + current.captured(3)).toLatin1();
}
bool readIni(const QString &root, IniFile *ini, QString *error) {
    QString path = QFileInfo(root).canonicalFilePath();
    for (const auto &part : QStringList{"Client", "Saved", "Config", "WindowsNoEditor", "GameUserSettings.ini"}) {
        path = QDir(path).filePath(part);
        const QFileInfo info(path);
        if (!info.exists() || info.isSymLink() || (part == "GameUserSettings.ini" ? !info.isFile() : !info.isDir())) {
            *error = "Файл настроек INI отсутствует или путь содержит ссылку: " + path; return false;
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        *error = "Не удалось прочитать GameUserSettings.ini или файл слишком большой."; return false;
    }
    ini->path = path;
    ini->content = file.readAll();
    if (file.error() != QFile::NoError) { *error = file.errorString(); return false; }
    ini->lines = ini->content.split('\n');
    bool inSection = false;
    int sectionCount = 0, count = 0;
    const QRegularExpression keyRe(QStringLiteral(R"(^(\s*FrameRateLimit\s*=\s*)([+-]?(?:\d+(?:\.\d*)?|\.\d+))(\s*(?:[;#].*)?\r?)$)"), QRegularExpression::CaseInsensitiveOption);
    for (int i = 0; i < ini->lines.size(); ++i) {
        const QByteArray line = ini->lines.at(i);
        const QByteArray trimmed = line.trimmed();
        if (trimmed.startsWith('[') && trimmed.endsWith(']')) {
            inSection = trimmed.compare("[/Script/Engine.GameUserSettings]", Qt::CaseInsensitive) == 0;
            if (inSection) ++sectionCount;
            continue;
        }
        if (!inSection || !trimmed.toLower().startsWith("frameratelimit")) continue;
        const QString text = QString::fromLatin1(line);
        const auto match = keyRe.match(text);
        if (!match.hasMatch()) { *error = "Не удалось разобрать FrameRateLimit в INI."; return false; }
        ++count;
        ini->line = i;
        ini->original = line;
        ini->applied = (match.captured(1) + "120" + match.captured(3)).toLatin1();
    }
    if (sectionCount != 1 || count != 1) { *error = "В INI ожидается одна секция GameUserSettings и одно значение FrameRateLimit."; return false; }
    return true;
}
bool writeIni(IniFile *ini, const QByteArray &expected, const QByteArray &replacement, QString *error) {
    if (ini->lines.at(ini->line) != expected) { *error = "FrameRateLimit был изменён извне."; return false; }
    if (expected == replacement) return true;
    QFile current(ini->path);
    if (QFileInfo(ini->path).isSymLink() || !current.open(QIODevice::ReadOnly) || current.readAll() != ini->content || current.error() != QFile::NoError) {
        *error = "Файл INI изменился во время операции."; return false;
    }
    ini->lines[ini->line] = replacement;
    QByteArray bytes;
    for (int i = 0; i < ini->lines.size(); ++i) { if (i) bytes += '\n'; bytes += ini->lines.at(i); }
    QSaveFile output(ini->path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
        *error = "Не удалось безопасно записать INI: " + output.errorString(); return false;
    }
    ini->content = bytes;
    return true;
}
bool readIniBackup(sqlite3 *db, bool *exists, QByteArray *original, QByteArray *applied, QString *error) {
    qint64 count = 0;
    if (!namedObjectCount(db, IniBackup, &count, error)) return false;
    if (!count) { *exists = false; return true; }
    if (count != 1) { *error = "Резервная таблица INI неоднозначна."; return false; }
    Statement table;
    if (!prepare(db, "SELECT sql FROM sqlite_master WHERE type='table' AND name='echo_fps_ini_v1'", &table, error)) return false;
    if (sqlite3_step(table.stmt) != SQLITE_ROW || QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(table.stmt, 0))) != QString::fromLatin1(CreateIniBackup)) {
        *error = "Резервная таблица INI изменена."; return false;
    }
    Statement value;
    if (!prepare(db, "SELECT id,original,applied FROM echo_fps_ini_v1", &value, error)) return false;
    if (sqlite3_step(value.stmt) != SQLITE_ROW || sqlite3_column_int(value.stmt, 0) != 1 ||
        sqlite3_column_type(value.stmt, 1) != SQLITE_BLOB || sqlite3_column_type(value.stmt, 2) != SQLITE_BLOB) {
        *error = "Резервное значение INI повреждено."; return false;
    }
    *original = QByteArray(static_cast<const char *>(sqlite3_column_blob(value.stmt, 1)), sqlite3_column_bytes(value.stmt, 1));
    *applied = QByteArray(static_cast<const char *>(sqlite3_column_blob(value.stmt, 2)), sqlite3_column_bytes(value.stmt, 2));
    if (original->isEmpty() || applied->isEmpty() || original->size() > 1024 || applied->size() > 1024 || sqlite3_step(value.stmt) != SQLITE_DONE) {
        *error = "Резервное значение INI повреждено."; return false;
    }
    *exists = true;
    return true;
}
bool saveIniBackup(sqlite3 *db, const IniFile &ini, QString *error) {
    if (!execute(db, CreateIniBackup, error)) return false;
    Statement s;
    if (!prepare(db, "INSERT INTO echo_fps_ini_v1 VALUES(1,?,?)", &s, error)) return false;
    sqlite3_bind_blob(s.stmt, 1, ini.original.constData(), static_cast<int>(ini.original.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(s.stmt, 2, ini.applied.constData(), static_cast<int>(ini.applied.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(s.stmt) != SQLITE_DONE) { *error = QString::fromUtf8(sqlite3_errmsg(db)); return false; }
    return true;
}
bool menuBackupState(sqlite3 *db, bool *exists, QString *error) {
    qint64 count = 0;
    if (!namedObjectCount(db, MenuBackup, &count, error)) return false;
    if (!count) { *exists = false; return true; }
    Statement schema;
    if (count != 1 || !prepare(db, "SELECT sql FROM sqlite_master WHERE type='table' AND name='echo_fps_menu_v1'", &schema, error) ||
        sqlite3_step(schema.stmt) != SQLITE_ROW ||
        QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(schema.stmt, 0))) != QString::fromLatin1(CreateMenuBackup)) {
        *error = "Таблица скопированных настроек ECHO изменена."; return false;
    }
    if (!oneInteger(db, "SELECT count(*) FROM echo_fps_menu_v1 WHERE key NOT IN ('MenuData','PlayMenuInfo') OR typeof(value) NOT IN ('text','blob')", &count, error) || count != 0 ||
        !oneInteger(db, "SELECT count(*) FROM echo_fps_menu_v1", &count, error) || count < 1 || count > 2) {
        *error = "Таблица скопированных настроек ECHO повреждена."; return false;
    }
    *exists = true;
    return true;
}
bool fillMissingMenuValues(sqlite3 *db, const QString &root, bool hasBackup, QString *error) {
    QStringList missing;
    for (const auto &key : {QStringLiteral("MenuData"), QStringLiteral("PlayMenuInfo")}) {
        Statement check;
        if (!prepare(db, "SELECT count(*) FROM LocalStorage WHERE key=?", &check, error)) return false;
        const QByteArray utf8 = key.toUtf8();
        sqlite3_bind_text(check.stmt, 1, utf8.constData(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(check.stmt) != SQLITE_ROW) { *error = QString::fromUtf8(sqlite3_errmsg(db)); return false; }
        const int rows = sqlite3_column_int(check.stmt, 0);
        if (rows > 1) { *error = "Настройка " + key + " неоднозначна."; return false; }
        if (!rows) {
            // A value copied previously and subsequently deleted is a user's change.
            if (hasBackup) {
                Statement previous;
                if (!prepare(db, "SELECT count(*) FROM echo_fps_menu_v1 WHERE key=?", &previous, error)) return false;
                sqlite3_bind_text(previous.stmt, 1, utf8.constData(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(previous.stmt) != SQLITE_ROW) { *error = QString::fromUtf8(sqlite3_errmsg(db)); return false; }
                if (sqlite3_column_int(previous.stmt, 0)) continue;
            }
            missing.append(key);
        }
    }
    if (missing.isEmpty()) return true;
    const QString sourceRoot = QFileInfo(QDir(root).absoluteFilePath("..")).canonicalFilePath();
    const QString sourcePath = databasePath(sourceRoot, error);
    if (sourcePath.isEmpty()) { *error = "Для отсутствующих настроек не найдена база родительского клиента: " + *error; return false; }
    if (QFileInfo(sourcePath).canonicalFilePath() == QFileInfo(databasePath(root, error)).canonicalFilePath()) {
        *error = "Исходная и целевая базы совпадают."; return false;
    }
    Database source;
    const QByteArray native = QFile::encodeName(sourcePath);
    if (sqlite3_open_v2(native.constData(), &source.db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        *error = "Не удалось прочитать настройки родительского клиента."; return false;
    }
    sqlite3_busy_timeout(source.db, 2000);
    if (!execute(source.db, "BEGIN", error)) return false;
    Statement integrity;
    if (!prepare(source.db, "PRAGMA quick_check", &integrity, error) || sqlite3_step(integrity.stmt) != SQLITE_ROW ||
        QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(integrity.stmt, 0))) != "ok" || sqlite3_step(integrity.stmt) != SQLITE_DONE) {
        *error = "База родительского клиента не прошла проверку целостности."; return false;
    }
    if (!hasBackup && !execute(db, CreateMenuBackup, error)) return false;
    for (const auto &key : missing) {
        const QByteArray utf8 = key.toUtf8();
        Statement read;
        if (!prepare(source.db, "SELECT value FROM LocalStorage WHERE key=?", &read, error)) return false;
        sqlite3_bind_text(read.stmt, 1, utf8.constData(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(read.stmt) != SQLITE_ROW ||
            (sqlite3_column_type(read.stmt, 0) != SQLITE_TEXT && sqlite3_column_type(read.stmt, 0) != SQLITE_BLOB) ||
            sqlite3_column_bytes(read.stmt, 0) <= 0 || sqlite3_column_bytes(read.stmt, 0) > 4 * 1024 * 1024) {
            *error = "В родительском клиенте отсутствует корректная настройка " + key + "."; return false;
        }
        const QByteArray value(static_cast<const char *>(sqlite3_column_blob(read.stmt, 0)), sqlite3_column_bytes(read.stmt, 0));
        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(value, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            *error = "Настройка " + key + " родительского клиента не является JSON-объектом."; return false;
        }
        Statement backup;
        if (!prepare(db, "INSERT INTO echo_fps_menu_v1 (key,value) VALUES (?,?)", &backup, error)) return false;
        sqlite3_bind_text(backup.stmt, 1, utf8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_value(backup.stmt, 2, sqlite3_column_value(read.stmt, 0));
        if (sqlite3_step(backup.stmt) != SQLITE_DONE) { *error = QString::fromUtf8(sqlite3_errmsg(db)); return false; }
        if (sqlite3_step(read.stmt) != SQLITE_DONE) { *error = "Настройка " + key + " в родительском клиенте неоднозначна."; return false; }
        Statement insert;
        if (!prepare(db, "INSERT INTO LocalStorage (key,value) SELECT key,value FROM echo_fps_menu_v1 WHERE key=? AND NOT EXISTS (SELECT 1 FROM LocalStorage WHERE key=?)", &insert, error)) return false;
        sqlite3_bind_text(insert.stmt, 1, utf8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert.stmt, 2, utf8.constData(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert.stmt) != SQLITE_DONE || sqlite3_changes(db) != 1) { *error = "Не удалось добавить настройку " + key + "."; return false; }
    }
    return true;
}
bool restoreMenuValues(sqlite3 *db, int *preserved, QString *error) {
    qint64 originalCount = 0;
    if (!oneInteger(db, "SELECT count(*) FROM echo_fps_menu_v1", &originalCount, error)) return false;
    if (!execute(db, "DELETE FROM LocalStorage WHERE key IN (SELECT key FROM echo_fps_menu_v1) AND EXISTS (SELECT 1 FROM echo_fps_menu_v1 b WHERE b.key=LocalStorage.key AND typeof(b.value)=typeof(LocalStorage.value) AND CAST(b.value AS BLOB)=CAST(LocalStorage.value AS BLOB))", error)) return false;
    *preserved = static_cast<int>(originalCount) - sqlite3_changes(db);
    return execute(db, "DROP TABLE echo_fps_menu_v1", error);
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
    if (!enable && !owned) return fail("Разблокировка ECHO не найдена; прежний лимит неизвестен.");
    bool hasMenuBackup = false;
    if (!menuBackupState(db, &hasMenuBackup, &error)) return fail(error);
    if (!owned && hasMenuBackup) return fail("Обнаружена незавершённая таблица скопированных настроек.");
    IniFile ini;
    if (!readIni(root, &ini, &error)) return fail(error);
    bool hasIniBackup = false;
    QByteArray original, applied;
    if (!readIniBackup(db, &hasIniBackup, &original, &applied, &error)) return fail(error);
    if (!owned && hasIniBackup) return fail("Найдена незавершённая или чужая резервная копия INI.");
    if (hasIniBackup && !sameIniLimit(ini.original, original) && !sameIniLimit(ini.original, applied))
        return fail("FrameRateLimit был изменён извне; автоматическое восстановление остановлено.");
    if (enable) {
        if (!fillMissingMenuValues(db, root, hasMenuBackup, &error)) return fail(error);
        if (!owned && (!execute(db, CreateBackup, &error) ||
            !execute(db, "INSERT INTO echo_fps_original_v1 SELECT 1,value FROM LocalStorage WHERE key='CustomFrameRate'", &error) ||
            !execute(db, CreateTrigger, &error) ||
            !execute(db, "UPDATE LocalStorage SET value=120 WHERE key='CustomFrameRate'", &error) || sqlite3_changes(db) != 1))
            return fail("Не удалось включить 120 FPS: " + error);
        // Older versions did not record the INI limit. If it is already 120,
        // its original value is unknown and must not be invented.
        const bool unknownLegacyIni = owned && !hasIniBackup && sameIniLimit(ini.original, ini.applied);
        if (!hasIniBackup && !unknownLegacyIni) {
            if (!saveIniBackup(db, ini, &error)) return fail("Не удалось сохранить прежний лимит INI: " + error);
            original = ini.original; applied = ini.applied;
        }
        // Commit the recovery record first. If writing INI is interrupted, a
        // later enable or disable can finish safely using these two values.
        if (!execute(db, "COMMIT", &error)) return fail("Не удалось сохранить настройки базы: " + error);
        if (!unknownLegacyIni && sameIniLimit(ini.original, original) && !sameIniLimit(ini.original, applied) &&
            !writeIni(&ini, ini.original, replaceIniLimit(ini.original, applied), &error))
            return {false, "База подготовлена, но INI не изменён: " + error + " Повторите включение или восстановление."};
        return {true, unknownLegacyIni ? "Лимит 120 FPS установлен. Прежнее значение INI неизвестно: старая версия ECHO его не сохранила." : "Лимит 120 FPS установлен. Фактическая частота зависит от производительности и настроек графики."};
    }
    // A backup-less state is from an earlier version which did not change INI.
    if (hasIniBackup && sameIniLimit(ini.original, applied) && !sameIniLimit(ini.original, original) &&
        !writeIni(&ini, ini.original, replaceIniLimit(ini.original, original), &error)) return fail(error);
    int preservedMenu = 0;
    if ((hasMenuBackup && !restoreMenuValues(db, &preservedMenu, &error)) ||
        !execute(db, "DROP TRIGGER echo_fps_keep_120_v1", &error) ||
        !execute(db, "UPDATE LocalStorage SET value=(SELECT value FROM echo_fps_original_v1 WHERE id=1) WHERE key='CustomFrameRate'", &error) || sqlite3_changes(db) != 1 ||
        !execute(db, "DROP TABLE echo_fps_original_v1", &error) ||
        (hasIniBackup && !execute(db, "DROP TABLE echo_fps_ini_v1", &error)))
        return fail("Не удалось восстановить прежний лимит: " + error + " Повторите восстановление.");
    if (!execute(db, "COMMIT", &error)) return fail("Не удалось сохранить восстановление: " + error + " Повторите восстановление.");
    if (!hasIniBackup)
        return {true, "Значение FPS в базе восстановлено. Прежний лимит INI неизвестен и не изменён."};
    return {true, preservedMenu ? "Прежний лимит восстановлен. Изменённые или удалённые настройки меню сохранены." : "Прежний лимит восстановлен."};
}
} // namespace
Result enable(const QString &gameRoot) { return change(gameRoot, true); }
Result disable(const QString &gameRoot) { return change(gameRoot, false); }
} // namespace EchoFps
