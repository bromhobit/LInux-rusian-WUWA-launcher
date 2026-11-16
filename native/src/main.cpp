#include <QApplication>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QScrollArea>
#include <QScrollBar>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSet>
#include <QTabWidget>
#include <QStackedWidget>
#include <QToolButton>
#include <QTextStream>
#include <QTimer>
#include <QVersionNumber>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QUuid>
#include <QWidget>
#include <xxhash.h>
#include <algorithm>
#include <functional>
#include <unistd.h>
#include <vector>
#include "auto_install.h"
#include "steam_options.h"
#include "steam_config.h"
#include "controller.h"

namespace {
class DialogLocalizer final : public QObject {
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if(event->type()!=QEvent::Show)return false;
        auto *box=qobject_cast<QMessageBox *>(object);
        if(!box)return false;
        for(auto *button:box->buttons()){
            switch(box->standardButton(button)){
            case QMessageBox::Yes: button->setText("Да");break;
            case QMessageBox::No: button->setText("Нет");break;
            case QMessageBox::Ok: button->setText("Понятно");break;
            case QMessageBox::Cancel: button->setText("Отмена");break;
            case QMessageBox::Close: button->setText("Закрыть");break;
            case QMessageBox::Retry: button->setText("Повторить");break;
            default: break;
            }
        }
        return false;
    }
};
constexpr qint64 MaxPayloadBytes = 8LL * 1024 * 1024 * 1024;
constexpr qint64 MaxManifestBytes = 128 * 1024;
constexpr qint64 MaxDownloadBytes = 4LL * 1024 * 1024 * 1024;
constexpr auto ChannelIndexUrl = "https://gitlab.com/ECHOLaunch/updates/-/raw/main/wwmain/main2.json";
struct Proton { QString name; QString path; };
struct InstallItem { QString source; QString destination; bool overwrite{}; };
struct TranslationChannel { QString id, name, description, manifestUrl; };
struct TranslationManifest { QString version, textHash, textUrl, textFileName; };

bool trustedGitlabRawUrl(const QUrl &url) {
    return url.scheme() == "https" && url.host() == "gitlab.com" && url.port(-1) == -1 &&
           url.userInfo().isEmpty() && url.path().startsWith("/ECHOLaunch/updates/-/raw/main/") &&
           url.query().isEmpty() && url.fragment().isEmpty();
}

bool safePakName(const QString &name) {
    static const QRegularExpression re("^pakchunk[0-9]+-WindowsNoEditor_[0-9]+_P\\.pak$",
                                       QRegularExpression::CaseInsensitiveOption);
    return re.match(name).hasMatch() && QFileInfo(name).fileName() == name;
}

bool parseChannelIndex(const QByteArray &data, QList<TranslationChannel> *channels, QString *error) {
    if (data.isEmpty() || data.size() > MaxManifestBytes) { *error = "Размер списка каналов некорректен."; return false; }
    QJsonParseError parseError; const auto doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) { *error = "Список каналов содержит неверный JSON."; return false; }
    const auto root = doc.object(); const auto array = root.value("TranslationChannels");
    if (!array.isArray() || array.toArray().size() > 8) { *error = "В списке отсутствуют каналы перевода."; return false; }
    QList<TranslationChannel> parsed; QSet<QString> seen;
    for (const auto &item : array.toArray()) {
        if (!item.isObject()) continue;
        const auto o = item.toObject(); const QString id=o.value("Id").toString(), name=o.value("Name").toString();
        const QString expected = "https://gitlab.com/ECHOLaunch/updates/-/raw/main/wwmain/channels/manifest_"+id+".json";
        if (!o.value("Enabled").toBool() || !QSet<QString>{"over","aniwie","qwikki"}.contains(id) || seen.contains(id) || name.isEmpty() || name.size()>40 || o.value("ManifestUrl").toString()!=expected || !trustedGitlabRawUrl(QUrl(expected))) continue;
        seen.insert(id); parsed << TranslationChannel{id,name,o.value("Description").toString(),expected};
    }
    if (parsed.isEmpty()) { *error = "Не найдено ни одного допустимого канала OVERLINK, Aniwie или qwikki."; return false; }
    *channels=parsed; return true;
}

bool parseTranslationManifest(const QByteArray &data, const QString &channelId, TranslationManifest *manifest, QString *error) {
    if (data.isEmpty() || data.size() > MaxManifestBytes) { *error = "Размер манифеста перевода некорректен."; return false; }
    QJsonParseError parseError; const auto doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) { *error = "Манифест перевода содержит неверный JSON."; return false; }
    const auto o=doc.object(); const QString version=o.value("Version").toString(), hash=o.value("TextHash").toString();
    const QString name=o.value("TextFileName").toString(), url=o.value("TextUrl").toString();
    const QString expectedPrefix="https://gitlab.com/ECHOLaunch/updates/-/raw/main/"+channelId+"/text/";
    static const QRegularExpression versionRe("^[0-9]+(?:\\.[0-9]+){0,3}$"), hashRe("^[0-9a-fA-F]{16}$");
    if (!QSet<QString>{"over","aniwie","qwikki"}.contains(channelId) || !versionRe.match(version).hasMatch() || !hashRe.match(hash).hasMatch() || !safePakName(name) || url != expectedPrefix+name || !trustedGitlabRawUrl(QUrl(url))) {
        *error="Манифест не прошёл проверку версии, имени файла, хеша или разрешённого HTTPS адреса."; return false;
    }
    TranslationManifest result{version,hash,url,name};
    *manifest=result; return true;
}

QString canonicalOrAbsolute(const QString &path) {
    const QFileInfo info(path);
    return info.exists() ? info.canonicalFilePath() : QDir::cleanPath(info.absoluteFilePath());
}

QByteArray fileHash(const QString &path, QString *error = nullptr) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray block = file.read(4 * 1024 * 1024);
        if (block.isEmpty() && file.error() != QFile::NoError) { if (error) *error = file.errorString(); return {}; }
        hash.addData(block);
    }
    return hash.result();
}

QString xxh64File(const QString &path, QString *error = nullptr) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return {}; }
    XXH64_state_t *state = XXH64_createState();
    if (!state || XXH64_reset(state, 0) != XXH_OK) {
        if (state) XXH64_freeState(state);
        if (error) *error = "Не удалось создать состояние XXH64.";
        return {};
    }
    bool ok = true;
    while (!file.atEnd()) {
        const QByteArray block = file.read(4 * 1024 * 1024);
        if (block.isEmpty() && file.error() != QFile::NoError) { ok = false; if (error) *error = file.errorString(); break; }
        if (XXH64_update(state, block.constData(), static_cast<size_t>(block.size())) != XXH_OK) {
            ok = false; if (error) *error = "Ошибка вычисления XXH64."; break;
        }
    }
    const XXH64_hash_t digest = ok ? XXH64_digest(state) : 0;
    XXH64_freeState(state);
    return ok ? QString::number(digest, 16).rightJustified(16, '0') : QString();
}

bool verifyAndPromotePackage(const QString &partPath, const QString &finalPath,
                             const QString &expectedHash, QString *error) {
    const QFileInfo info(partPath);
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 || info.size() > MaxDownloadBytes) {
        *error = "Размер временного пакета недопустим."; return false;
    }
    QString hashError; const QString actual=xxh64File(partPath,&hashError);
    if (actual.isEmpty()) { *error="Не удалось вычислить XXH64: "+hashError; return false; }
    if (actual.compare(expectedHash,Qt::CaseInsensitive)!=0) {
        *error="XXH64 не совпал с манифестом. Ожидалось "+expectedHash.toLower()+", получено "+actual+"."; return false;
    }
    if (QFileInfo::exists(finalPath)) { *error="Файл назначения уже существует; он не будет заменён."; return false; }
    if (!QFile::rename(partPath,finalPath)) { *error="Не удалось атомарно переместить проверенный пакет в кеш."; return false; }
    return true;
}

bool copyVerified(const QString &source, const QString &destination, QString *error) {
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) { *error = "Не удалось прочитать " + source + ": " + in.errorString(); return false; }
    QSaveFile out(destination);
    out.setDirectWriteFallback(false);
    if (!out.open(QIODevice::WriteOnly)) { *error = "Не удалось создать временный файл рядом с " + destination + ": " + out.errorString(); return false; }
    QCryptographicHash sourceHash(QCryptographicHash::Sha256), writtenHash(QCryptographicHash::Sha256);
    while (!in.atEnd()) {
        const QByteArray block = in.read(4 * 1024 * 1024);
        if (block.isEmpty() && in.error() != QFile::NoError) { *error = "Ошибка чтения источника: " + in.errorString(); out.cancelWriting(); return false; }
        sourceHash.addData(block);
        if (out.write(block) != block.size()) { *error = "Ошибка записи временного файла: " + out.errorString(); out.cancelWriting(); return false; }
        writtenHash.addData(block);
    }
    if (sourceHash.result() != writtenHash.result()) { *error = "Контрольная сумма временной копии не совпала."; out.cancelWriting(); return false; }
    if (!out.commit()) { *error = "Не удалось атомарно заменить файл: " + out.errorString(); return false; }
    QString verifyError;
    if (fileHash(destination, &verifyError) != sourceHash.result()) {
        QFile::remove(destination); *error = "Проверка записанного файла не прошла: " + verifyError; return false;
    }
    return true;
}

bool makePlan(const QString &pakPath, const QString &sigPath, const QString &target,
              QList<InstallItem> *plan, QString *error) {
    plan->clear();
    const QString pak = QFileInfo(pakPath).absoluteFilePath();
    const QFileInfo pakInfo(pak);
    if (!pakInfo.exists() || !pakInfo.isFile() || pakInfo.isSymLink() || pakInfo.suffix().compare("pak", Qt::CaseInsensitive) != 0 || pakInfo.size() <= 0 || pakInfo.size() > MaxPayloadBytes) {
        *error = "Выберите существующий непустой файл .pak размером не более 8 ГиБ."; return false;
    }
    const QFileInfo targetInfo(target);
    if (!targetInfo.exists() || !targetInfo.isDir() || targetInfo.isSymLink()) {
        *error = "Выберите существующую папку локализации. Символические ссылки как папка назначения запрещены."; return false;
    }
    const QString targetRoot = targetInfo.canonicalFilePath();
    if (targetRoot.isEmpty()) { *error = "Не удалось определить реальный путь папки назначения."; return false; }
    auto append = [&](const QString &source, const QString &expectedSuffix) -> bool {
        const QFileInfo info(source);
        if (!info.exists() || !info.isFile() || info.isSymLink() || info.suffix().compare(expectedSuffix, Qt::CaseInsensitive) != 0 || info.size() <= 0 || info.size() > MaxPayloadBytes) {
            *error = "Файл " + expectedSuffix.toUpper() + " отсутствует, пуст, является ссылкой или превышает 8 ГиБ."; return false;
        }
        const QString dest = QDir(targetRoot).filePath(info.fileName());
        if (canonicalOrAbsolute(info.absoluteFilePath()) == canonicalOrAbsolute(dest)) {
            *error = "Источник совпадает с целевым файлом: " + info.fileName(); return false;
        }
        const QFileInfo destInfo(dest);
        if (destInfo.isSymLink()) { *error = "Целевой файл является символической ссылкой: " + dest; return false; }
        if (destInfo.exists() && !destInfo.isFile()) { *error = "Вместо целевого файла существует объект другого типа: " + dest; return false; }
        const QString cleanDest = QDir::cleanPath(QFileInfo(dest).absoluteFilePath());
        if (QFileInfo(cleanDest).absolutePath() != QDir::cleanPath(targetRoot)) { *error = "Путь назначения вышел за выбранную папку."; return false; }
        plan->append({info.absoluteFilePath(), cleanDest, destInfo.exists()});
        return true;
    };
    if (!append(pak, "pak")) return false;
    if (!sigPath.trimmed().isEmpty()) {
        const QFileInfo pakFile(pakPath), sigFile(sigPath);
        if (sigFile.completeBaseName().compare(pakFile.completeBaseName(), Qt::CaseInsensitive) != 0) {
            *error = "Имена .pak и .sig должны совпадать (кроме расширения)."; return false;
        }
        if (!append(sigPath, "sig")) return false;
    }
    return true;
}

bool installPlan(const QList<InstallItem> &plan, const QString &backupRoot,
                 QString *backupLocation, QString *error) {
    if (plan.isEmpty()) { *error = "Нет файлов для установки."; return false; }
    if (QFileInfo(backupRoot).isSymLink()) { *error = "Папка резервных копий является символической ссылкой."; return false; }
    if (!QDir().mkpath(backupRoot)) { *error = "Не удалось создать папку резервных копий: " + backupRoot; return false; }
    const QString transaction = QDir(backupRoot).filePath(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(transaction)) { *error = "Не удалось создать каталог резервной копии: " + transaction; return false; }
    struct Applied { QString destination; QString backup; bool hadOriginal{}; };
    QList<Applied> applied;
    auto rollback = [&]() {
        QStringList failures;
        for (auto it = applied.crbegin(); it != applied.crend(); ++it) {
            QString rollbackError;
            if (it->hadOriginal) {
                if (!copyVerified(it->backup, it->destination, &rollbackError)) failures << (it->destination + ": " + rollbackError);
            } else if (QFileInfo(it->destination).exists() && !QFile::remove(it->destination)) {
                failures << (it->destination + ": не удалось удалить новый файл");
            }
        }
        return failures;
    };
    for (const auto &item : plan) {
        const QFileInfo now(item.destination);
        if (now.isSymLink() || (now.exists() && !now.isFile())) {
            *error = "Целевой путь изменился или стал ссылкой: " + item.destination;
            const auto failures = rollback();
            if (!failures.isEmpty()) *error += "\nОшибка отката: " + failures.join("; ");
            return false;
        }
        Applied entry{item.destination, {}, now.exists()};
        if (entry.hadOriginal) {
            entry.backup = QDir(transaction).filePath(QFileInfo(item.destination).fileName());
            QString backupError;
            if (!copyVerified(item.destination, entry.backup, &backupError)) {
                *error = "Не удалось проверить резервную копию " + item.destination + ": " + backupError;
                const auto failures = rollback();
                if (!failures.isEmpty()) *error += "\nОшибка отката: " + failures.join("; ");
                return false;
            }
        }
        applied << entry;
        QString installError;
        if (!copyVerified(item.source, item.destination, &installError)) {
            *error = "Не удалось установить " + item.destination + ": " + installError;
            const auto failures = rollback();
            if (!failures.isEmpty()) *error += "\nОшибка отката: " + failures.join("; ");
            return false;
        }
    }
    *backupLocation = transaction;
    return true;
}

QStringList steamRoots() {
    QStringList roots;
    const auto envRoot = qEnvironmentVariable("STEAM_ROOT");
    if (!envRoot.isEmpty()) roots << envRoot;
    const auto home = QDir::homePath();
    roots << home + "/.steam/steam" << home + "/.local/share/Steam" << home + "/.steam/root";
    QStringList existing;
    for (const auto &root : roots) {
        QFileInfo info(root);
        if (info.isDir()) { const auto canonical = info.canonicalFilePath(); if (!existing.contains(canonical)) existing << canonical; }
    }
    return existing;
}

QStringList libraries(const QStringList &roots) {
    QStringList result = roots;
    for (const auto &root : roots) {
        QFile f(root + "/steamapps/libraryfolders.vdf");
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QRegularExpression re("\\\"path\\\"\\s+\\\"([^\\\"]+)\\\"");
        auto it = re.globalMatch(QString::fromUtf8(f.readAll()));
        while (it.hasNext()) result << it.next().captured(1).replace("\\\\", "\\");
    }
    result.removeDuplicates(); return result;
}

constexpr const char *WutheringAppId = "3513350";
struct SteamGame { QString manifest; QString installDir; QString gameDir; QString discoveryError; };

// Steam owns the outer launcher directory; the actual client may be there or
// in a subdirectory created by Kuro's launcher. Only use paths with both a
// launcher and versioned resource tables, and never follow nested symlinks.
SteamGame resolveSteamGame(const QString &manifest, const QString &installDir) {
    struct Candidate { QString path; bool hasClient{}; };
    QList<Candidate> candidates;
    QStringList level{installDir};
    for (int depth=0; depth<=2 && !level.isEmpty(); ++depth) {
        QStringList next;
        for (const auto &path : level) {
            const bool launcher = QFileInfo(path+"/Wuthering Waves.exe").isFile() ||
                                  QFileInfo(path+"/launcher.exe").isFile();
            if (launcher && !EchoInstall::detectResourceVersion(path).isEmpty()) {
                candidates.append({path, QFileInfo(path+"/Client/Binaries/Win64/Client-Win64-Shipping.exe").isFile()});
            }
            if (depth==2) continue;
            const QDir dir(path);
            for (const auto &name : dir.entryList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::NoSymLinks)) {
                // These trees are not installation roots and can be enormous.
                if (name=="Client" || name=="Engine" || name=="kr_game_cache") continue;
                next.append(dir.filePath(name));
            }
        }
        level=next;
    }
    QList<Candidate> withClient;
    for (const auto &candidate : candidates)
        if (candidate.hasClient) withClient.append(candidate);
    const auto &usable = withClient.isEmpty()?candidates:withClient;
    if (usable.size()==1)
        return {manifest, installDir, QFileInfo(usable.first().path).canonicalFilePath(), {}};
    if (usable.isEmpty())
        return {manifest, installDir, {}, "Игра в Steam найдена, но готовые файлы клиента и ресурсы ещё не обнаружены."};
    return {manifest, installDir, {}, "В папке Steam найдены несколько клиентов с ресурсами. Автоматический выбор остановлен."};
}

QString manifestValue(const QString &contents, const QString &key) {
    const QRegularExpression re("\\\"" + QRegularExpression::escape(key) + "\\\"\\s+\\\"([^\\\"]+)\\\"");
    const auto match = re.match(contents);
    return match.hasMatch() ? match.captured(1) : QString();
}

QList<SteamGame> discoverWutheringInstallations(const QStringList &roots) {
    QList<SteamGame> found;
    QStringList seenManifests;
    for (const auto &library : libraries(roots)) {
        const QString manifest = library + "/steamapps/appmanifest_" + WutheringAppId + ".acf";
        QFile file(manifest);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString contents = QString::fromUtf8(file.readAll());
        if (manifestValue(contents, "appid") != WutheringAppId) continue;
        const QString installDir = manifestValue(contents, "installdir");
        if (installDir.isEmpty() || installDir == "." || installDir == ".." || installDir.contains('/') || installDir.contains('\\')) continue;
        const QString gameDir = QDir(library + "/steamapps/common").filePath(installDir);
        const QString canonicalInstall=QFileInfo(gameDir).canonicalFilePath();
        const QString canonicalManifest=QFileInfo(manifest).canonicalFilePath();
        if (canonicalInstall.isEmpty() || canonicalManifest.isEmpty() || seenManifests.contains(canonicalManifest)) continue;
        seenManifests << canonicalManifest;
        found << resolveSteamGame(canonicalManifest, canonicalInstall);
    }
    return found;
}

// Optional Steam launch wrapper. The ordinary Steam launch remains available
// for updates. Only redirect when the launcher, client and quality pack are
// unambiguous; keep all existing Proton and per-game wrapper arguments.
bool directSteamArguments(const QStringList &input,const QList<SteamGame> &games,
                          QStringList *output,QString *reason) {
    *output=input;
    if(qEnvironmentVariable("ECHO_OFFICIAL_LAUNCHER")=="1"){
        *reason="official launcher requested";return false;
    }
    const QString appId=qEnvironmentVariable("SteamAppId");
    if((!appId.isEmpty()&&appId!=WutheringAppId)||games.size()!=1||games.first().gameDir.isEmpty()){
        *reason="Steam game or client is ambiguous";return false;
    }
    const auto &game=games.first();
    const QString launcher=QFileInfo(game.installDir+"/launcher.exe").canonicalFilePath();
    if(launcher.isEmpty()){*reason="Steam launcher not found";return false;}
    int index=-1;
    for(int i=1;i<input.size();++i) {
        if(QFileInfo(input.at(i)).canonicalFilePath()==launcher) {
            if(index>=0){*reason="multiple launcher arguments";return false;}
            index=i;
        }
    }
    if(index<0){*reason="launcher not present in Steam command";return false;}
    const QFileInfo shipping(game.gameDir+"/Client/Binaries/Win64/Client-Win64-Shipping.exe");
    const QString executable=shipping.canonicalFilePath();
    if(!shipping.isFile()||shipping.isSymLink()||executable.isEmpty()||
       !executable.startsWith(game.gameDir+"/Client/")){
        *reason="game executable not found";return false;
    }
    const QString resourceVersion=EchoInstall::detectResourceVersion(game.gameDir);
    QFile config(game.gameDir+"/launcherDownloadConfig.json");
    if(resourceVersion.isEmpty()||!config.open(QIODevice::ReadOnly)){
        *reason="installed resource version unavailable";return false;
    }
    const auto bundles=QJsonDocument::fromJson(config.readAll()).object().value("bundles").toObject();
    QStringList qualities;
    for(auto it=bundles.constBegin();it!=bundles.constEnd();++it){
        const QString key=it.key().toLower();
        if((key=="sd"||key=="hd"||key=="uhd")&&
           it.value().toObject().value("version").toString()==resourceVersion)
            qualities<<key;
    }
    if(qualities.size()!=1){*reason="installed quality pack is ambiguous";return false;}
    const QString qualityArg="-krqlv="+qualities.first();
    bool provided=false;
    for(const auto &arg:input) {
        if(arg.startsWith("-krqlv=",Qt::CaseInsensitive)){
            if(arg.compare(qualityArg,Qt::CaseInsensitive)!=0){
                *reason="requested quality differs from the installed pack";return false;
            }
            provided=true;
        }
    }
    (*output)[index]=executable;
    if(!provided)output->append(qualityArg);
    *reason="direct game; quality "+qualities.first();
    return true;
}

bool takeDirectIntent(const QList<QByteArray> &input,QList<QByteArray> *clean) {
    int count=0;clean->clear();
    for(const auto &arg:input){if(arg=="--echo-native-direct")++count;else clean->append(arg);}
    return count==1;
}

int runSteamWrapper(int argc,char **argv) {
    QStringList command;
    QList<QByteArray> raw, originalBytes;
    for(int i=2;i<argc;++i)raw<<QByteArray(argv[i]);
    const bool direct=takeDirectIntent(raw,&originalBytes);
    for(const auto &arg:originalBytes)command<<QFile::decodeName(arg);
    if(command.isEmpty()){fprintf(stderr,"ECHO: Steam command is empty.\n");return 2;}
    QStringList next=command;QString reason="official launcher";
    if(direct && qEnvironmentVariable("SteamAppId")==WutheringAppId)
        directSteamArguments(command,discoverWutheringInstallations(steamRoots()),&next,&reason);
    fprintf(stderr,"ECHO Steam: %s\n",qPrintable(reason));
    std::vector<QByteArray> storage;
    storage.reserve(static_cast<size_t>(next.size()));
    for(qsizetype i=0;i<next.size();++i){
        if(i<command.size()&&next.at(i)==command.at(i))
            storage.emplace_back(originalBytes.at(i)); // Preserve original non-UTF-8 bytes after removing the marker.
        else
            storage.emplace_back(QFile::encodeName(next.at(i)));
    }
    std::vector<char*> args;
    args.reserve(storage.size()+1);
    for(auto &arg:storage)args.push_back(arg.data());
    args.push_back(nullptr);
    ::execvp(args[0],args.data());
    perror("ECHO: unable to start Steam command");
    return 127;
}

bool isShippingExecutable(const QString &path) {
    return QFileInfo(path).fileName().compare("Client-Win64-Shipping.exe", Qt::CaseInsensitive) == 0;
}

bool isSupportedStandaloneLauncher(const QString &path) {
    return QFileInfo(path).fileName().compare("launcher.exe", Qt::CaseInsensitive) == 0;
}

QString steamExecutable() {
    const QString configured = qEnvironmentVariable("STEAM_EXECUTABLE");
    if (!configured.isEmpty() && QFileInfo(configured).isExecutable()) return QFileInfo(configured).absoluteFilePath();
    if (QFileInfo("/usr/bin/steam").isExecutable()) return "/usr/bin/steam";
    return QStandardPaths::findExecutable("steam");
}

QStringList steamLaunchArguments() { return {"-applaunch", WutheringAppId, "--echo-native-direct"}; }

// Steam evaluates launch options as a shell command. Always quote the entire
// path, including spaces and apostrophes; AppImages must use their stable path
// rather than the transient mounted executable.
QString steamWrapperCommand(const QString &path) {
    if(path.isEmpty() || path.contains('\n') || path.contains('\r') || path.contains(QChar::Null)) return {};
    QString quoted=path;
    quoted.replace("'", "'\\''");
    return "'"+quoted+"' --steam-wrap %command%";
}

QProcessEnvironment sanitizedSteamEnvironment(QProcessEnvironment env = QProcessEnvironment::systemEnvironment()) {
    const bool fromSteam = !env.value("SteamAppId").isEmpty() || !env.value("SteamGameId").isEmpty() ||
                           !env.value("STEAM_COMPAT_APP_ID").isEmpty() || !env.value("SteamClientLaunch").isEmpty();
    const char *const stripVars[] = {
        "SteamAppId", "SteamGameId", "SteamOverlayGameId",
        "STEAM_COMPAT_APP_ID", "STEAM_COMPAT_DATA_PATH",
        "STEAM_COMPAT_CLIENT_INSTALL_PATH", "SteamClientLaunch"
    };
    for (const char *name : stripVars) env.remove(QString::fromLatin1(name));
    if(fromSteam)env.remove("LD_PRELOAD");
    return env;
}

struct SteamLaunchOptionsCheck {
    bool ok{true};
    QString wrapperPath;
    QString error;
};

QString extractVdfAppLaunchOptions(const QString &contents, const QString &appId) {
    const QRegularExpression appRe("\\\"" + QRegularExpression::escape(appId) + "\\\"\\s*\\{");
    QRegularExpressionMatchIterator it = appRe.globalMatch(contents);
    QString result; bool found=false;
    while (it.hasNext()) {
        const auto match = it.next();
        int pos = match.capturedEnd();
        int depth = 1;
        bool inQuotes = false;
        int blockEnd = -1;
        for (int i = pos; i < contents.size(); ++i) {
            const QChar ch = contents.at(i);
            if (ch == '\\' && inQuotes && i + 1 < contents.size()) { ++i; continue; }
            if (ch == '"') { inQuotes = !inQuotes; continue; }
            if (!inQuotes) {
                if (ch == '{') ++depth;
                else if (ch == '}' && --depth == 0) { blockEnd = i; break; }
            }
        }
        if (blockEnd > pos) {
            const QString block = contents.mid(pos, blockEnd - pos);
            const QRegularExpression optRe("\\\"LaunchOptions\\\"\\s+\\\"((?:[^\\\"\\\\]|\\\\.)*)\\\"");
            auto opts=optRe.globalMatch(block);
            if (opts.hasNext()) {
                const auto optMatch=opts.next();
                if(found||opts.hasNext())return {};
                QString val = optMatch.captured(1);
                val.replace("\\\\", "\\");
                val.replace("\\\"", "\"");
                result=val;found=true;
            }
        }
    }
    return result;
}

// Validate only a single, literal wrapper path followed by %command%.  Shell
// pipelines, variables, arguments and multi-account configs belong to Steam.
QString simpleWrapperFromLaunchOptions(const QString &options,const QString &homeDir=QDir::homePath()) {
    if(options.contains('\n')||options.contains('\r'))return {};
    static const QRegularExpression suffix(R"(\s+%command%\s*$)",QRegularExpression::CaseInsensitiveOption);
    const auto match=suffix.match(options);
    if(!match.hasMatch())return {};
    QString path=options.left(match.capturedStart()).trimmed();
    const bool quoted=path.size()>=2&&((path.front()=='"'&&path.back()=='"')||(path.front()=='\''&&path.back()=='\''));
    if(quoted)
        path=path.mid(1,path.size()-2);
    else if(path.contains(QRegularExpression(R"(\s)")))return {};
    // Reject expansions, quoting tricks and shell operators even inside quotes.
    static const QRegularExpression shellMeta(R"meta([\\$`;&|<>(){}*?\[\]"'])meta");
    if(path.isEmpty()||shellMeta.match(path).hasMatch())return {};
    if(path.startsWith("~/"))return quoted?QString():QDir::cleanPath(homeDir+path.mid(1));
    return QDir::isAbsolutePath(path)?QDir::cleanPath(path):QString();
}

// The loginusers file uses SteamID64 while userdata uses 32-bit account IDs.
// Only choose a user if the file unambiguously identifies one.
QString steamLoginAccount(const QString &contents) {
    static const QRegularExpression accountRe(R"re("([0-9]{17})"\s*\{)re");
    static const QRegularExpression recentRe(R"re("MostRecent"\s+"([^"]*)")re");
    constexpr quint64 SteamIdBase=76561197960265728ULL;
    QStringList accounts,recent;
    auto matches=accountRe.globalMatch(contents);
    while(matches.hasNext()){
        const auto match=matches.next();
        bool valid=false;
        const quint64 id=match.captured(1).toULongLong(&valid);
        if(!valid||id<=SteamIdBase||id-SteamIdBase>4294967295ULL)return {};
        int depth=1,end=-1;
        bool inQuotes=false;
        for(int i=match.capturedEnd();i<contents.size();++i){
            const QChar ch=contents.at(i);
            if(ch=='\\'&&inQuotes&&i+1<contents.size()){++i;continue;}
            if(ch=='"'){inQuotes=!inQuotes;continue;}
            if(!inQuotes){if(ch=='{')++depth;else if(ch=='}'&&--depth==0){end=i;break;}}
        }
        if(end<0)return {};
        const QString account=QString::number(id-SteamIdBase);
        if(accounts.contains(account))return {};
        accounts<<account;
        const QString block=contents.mid(match.capturedEnd(),end-match.capturedEnd());
        auto flags=recentRe.globalMatch(block);
        if(flags.hasNext()){
            const auto flag=flags.next();
            if(flags.hasNext())return {};
            if(flag.captured(1)!="0"&&flag.captured(1)!="1")return {};
            if(flag.captured(1)=="1")recent<<account;
        }
    }
    if(accounts.size()==1)return accounts.first();
    if(recent.size()==1)return recent.first();
    return {};
}

SteamLaunchOptionsCheck inspectSteamLaunchOptions(const QStringList &roots,const QString &appId=WutheringAppId,
                                                  const QString &homeDir=QDir::homePath()) {
    QSet<QString> configs,loginFiles;
    QHash<QString,QSet<QString>> byAccount;
    for(const QString &root:roots){
        const QFileInfo login(root+"/config/loginusers.vdf");
        if(login.exists()){
            const QString canonical=login.canonicalFilePath();
            if(!login.isFile()||canonical.isEmpty())return {};
            loginFiles.insert(canonical);
        }
        QDir userdata(root+"/userdata");
        for(const QString &uid:userdata.entryList(QDir::Dirs|QDir::NoDotAndDotDot)){
            const QFileInfo file(userdata.filePath(uid+"/config/localconfig.vdf"));
            if(file.isFile()){
                const QString canonical=file.canonicalFilePath();
                if(!canonical.isEmpty()){configs.insert(canonical);byAccount[uid].insert(canonical);}
            }
        }
    }
    QString chosen;
    if(loginFiles.isEmpty()){
        if(configs.size()!=1)return {};
        chosen=*configs.cbegin();
    }else{
        QSet<QString> active;
        for(const QString &path:loginFiles){
            QFile login(path);
            if(!login.open(QIODevice::ReadOnly|QIODevice::Text))return {};
            const QString account=steamLoginAccount(QString::fromUtf8(login.readAll()));
            if(account.isEmpty())return {};
            active.insert(account);
        }
        if(active.size()!=1)return {};
        const auto matching=byAccount.value(*active.cbegin());
        if(matching.size()!=1)return {};
        chosen=*matching.cbegin();
    }
    QFile file(chosen);
    if(!file.open(QIODevice::ReadOnly|QIODevice::Text))return {};
    const QString options=extractVdfAppLaunchOptions(QString::fromUtf8(file.readAll()),appId);
    const QString wrapper=simpleWrapperFromLaunchOptions(options,homeDir);
    if(wrapper.isEmpty())return {};
    const QFileInfo info(wrapper);
    if(info.isFile()&&info.isExecutable())return {true,wrapper,{}};
    const QString why=info.exists()
        ? QString("Скрипт запуска Steam недоступен или не является исполняемым: %1\n\nПроверьте параметры запуска Wuthering Waves в Steam.").arg(wrapper)
        : QString("Не найден скрипт запуска Steam: %1\n\nВосстановите его или измените параметры запуска Wuthering Waves в Steam.").arg(wrapper);
    return {false,wrapper,why};
}
bool launchStandaloneProton(QProcess *process, const QString &proton, const QString &launcher,
                            const QString &compat, const QString &clientRoot, QString *error) {
    if (isShippingExecutable(launcher)) {
        *error = "Прямой запуск Client-Win64-Shipping.exe запрещён. Запускайте Wuthering Waves через Steam, чтобы Steam использовал штатную цепочку запуска.";
        return false;
    }
    if (!isSupportedStandaloneLauncher(launcher)) {
        *error = "Для отдельного запуска через Proton выберите launcher.exe. Совместимость такого запуска не гарантируется.";
        return false;
    }
    if (!QFileInfo(launcher).isFile() || !QFileInfo(proton).isExecutable()) {
        *error = "Проверьте путь к launcher.exe и исполняемому скрипту Proton.";
        return false;
    }
    if (compat.isEmpty() || compat == QDir::rootPath() || compat == QDir::homePath()) {
        *error = "Compatdata должен быть отдельным пользовательским каталогом.";
        return false;
    }
    if (!QDir().mkpath(compat)) { *error = "Не удалось создать каталог compatdata: " + compat; return false; }
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("STEAM_COMPAT_DATA_PATH", compat);
    if (!clientRoot.isEmpty()) env.insert("STEAM_COMPAT_CLIENT_INSTALL_PATH", clientRoot);
    env.insert("STEAM_COMPAT_APP_ID", "0"); env.insert("SteamAppId", "0"); env.insert("SteamGameId", "0");
    process->setProgram(proton); process->setArguments({"run", launcher}); process->setProcessEnvironment(env);
    return true;
}

[[maybe_unused]] QList<Proton> discoverProton() {
    QList<Proton> out; const auto roots = steamRoots();
    for (const auto &root : roots) {
        QDir compat(root + "/compatibilitytools.d");
        for (const auto &entry : compat.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString p = compat.filePath(entry + "/proton"); if (QFileInfo(p).isExecutable()) out << Proton{entry, QFileInfo(p).absoluteFilePath()};
        }
    }
    for (const auto &library : libraries(roots)) {
        QDir common(library + "/steamapps/common");
        for (const auto &entry : common.entryList(QStringList{"Proton*"}, QDir::Dirs, QDir::Name)) {
            const QString p = common.filePath(entry + "/proton"); if (QFileInfo(p).isExecutable()) out << Proton{entry, QFileInfo(p).absoluteFilePath()};
        }
    }
    std::sort(out.begin(), out.end(), [](const Proton &a, const Proton &b) { return a.name.toLower() < b.name.toLower(); });
    QList<Proton> unique; for (const auto &item : out) { bool seen=false; for (const auto &u: unique) if (u.path==item.path) seen=true; if(!seen) unique << item; }
    return unique;
}

QString guessGameExe(const QString &dir) {
    const QStringList names{"launcher.exe", "Wuthering Waves.exe", "WutheringWaves.exe", "Client-Win64-Shipping.exe"};
    for (const auto &name : names) { const auto found=QDir(dir).entryList(QStringList{name},QDir::Files,QDir::Name); if(!found.isEmpty()) return QDir(dir).absoluteFilePath(found.first()); }
    return {};
}

class Window final : public QWidget {
public:
    explicit Window(bool smoke = false, bool autoCheck = false) : settings("ECHO Native", "ECHO Native") {
        setWindowTitle("ECHO · Русификатор Wuthering Waves"); setObjectName("rootWindow"); resize(940, 680); setMinimumSize(800, 600);
        settingsPath = settings.fileName();
        appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        logPath = appDataPath + "/logs/run.log";
        network = new QNetworkAccessManager(this);
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        auto *pages = new QStackedWidget(this); pages->setObjectName("pageStack");
        auto *topBar = new QWidget; topBar->setObjectName("topBar");
        auto *topLayout = new QHBoxLayout(topBar); topLayout->setContentsMargins(24,10,24,10); topLayout->setSpacing(8);
        auto *brand = new QVBoxLayout; brand->setSpacing(0);
        auto *brandName = new QLabel("ECHO"); brandName->setObjectName("brandName"); brand->addWidget(brandName);
        auto *brandSubtitle = new QLabel("РУСИФИКАТОР WUTHERING WAVES"); brandSubtitle->setObjectName("sideSubtitle"); brand->addWidget(brandSubtitle);
        topLayout->addLayout(brand,1);
        auto *localizerNav = new QPushButton("Перевод"); localizerNav->setObjectName("navButton"); localizerNav->setCheckable(true); localizerNav->setChecked(true);
        auto *settingsNav = new QPushButton("Настройки"); settingsNav->setObjectName("navButton"); settingsNav->setCheckable(true);
        topLayout->addWidget(localizerNav); topLayout->addWidget(settingsNav);
        root->addWidget(topBar); root->addWidget(pages,1);

        auto *homeScroll = new QScrollArea;
        homeScroll->setWidgetResizable(true);
        homeScroll->setFrameShape(QFrame::NoFrame);
        auto *home = new QWidget;
        home->setObjectName("homePage");
        auto *l = new QVBoxLayout(home);
        l->setContentsMargins(24, 20, 24, 14);
        l->setSpacing(16);

        auto *heading = new QLabel("Текстовый перевод"); heading->setObjectName("pageTitle"); l->addWidget(heading);
        auto *subtitle = new QLabel("Перевод интерфейса, заданий и диалогов");
        subtitle->setObjectName("secondaryText"); l->addWidget(subtitle);

        auto *gameCard = new QWidget; gameCard->setObjectName("gameCard");
        auto *gameLayout = new QHBoxLayout(gameCard); gameLayout->setContentsMargins(18,14,18,14); gameLayout->setSpacing(16);
        auto *gameInfo = new QVBoxLayout; gameInfo->setSpacing(4);
        auto *gameTitle = new QLabel("WUTHERING WAVES"); gameTitle->setObjectName("gameTitle"); gameInfo->addWidget(gameTitle);
        auto *gameStatusRow = new QHBoxLayout; gameStatusRow->setSpacing(8);
        gameDot = new QLabel("●"); gameDot->setObjectName("statusDot"); gameStatusRow->addWidget(gameDot,0,Qt::AlignTop);
        statusLabel = new QLabel; statusLabel->setObjectName("gameStatus"); statusLabel->setWordWrap(true); statusLabel->setMinimumWidth(0); gameStatusRow->addWidget(statusLabel,1);
        gameInfo->addLayout(gameStatusRow);
        gameLayout->addLayout(gameInfo,1);
        launchBtn = new QPushButton("Играть через Steam");
        launchBtn->setObjectName("playGameButton");
        launchBtn->setMinimumSize(180,52);
        launchBtn->setAccessibleName("Играть в Wuthering Waves");
        launchBtn->setToolTip("Запустить Wuthering Waves штатным способом");
        gameLayout->addWidget(launchBtn,0,Qt::AlignVCenter);
        l->addWidget(gameCard);

        auto *channelCard = new QFrame; channelCard->setObjectName("surfaceCard");
        auto *channelLayout = new QVBoxLayout(channelCard); channelLayout->setContentsMargins(18, 16, 18, 18); channelLayout->setSpacing(10);
        auto *channelHeadingRow = new QHBoxLayout;
        auto *channelTitle = new QLabel("Состояние перевода"); channelTitle->setObjectName("sectionTitle"); channelHeadingRow->addWidget(channelTitle,1);
        versionStatus = new QLabel("Не проверено"); versionStatus->setObjectName("versionValue"); channelHeadingRow->addWidget(versionStatus,0,Qt::AlignRight);
        channelLayout->addLayout(channelHeadingRow);
        channelStatus = new QLabel("Выберите канал и проверьте наличие обновления."); channelStatus->setObjectName("inlineStatus"); channelStatus->setWordWrap(true); channelLayout->addWidget(channelStatus);
        auto *channelLabel = new QLabel("Канал перевода"); channelLabel->setObjectName("secondaryText"); channelLayout->addWidget(channelLabel);
        channelBox = new QComboBox(channelCard); channelBox->setObjectName("channelSelector"); channelBox->hide();
        auto *channelCards = new QWidget; auto *cardsLayout = new QHBoxLayout(channelCards); cardsLayout->setContentsMargins(0,2,0,2); cardsLayout->setSpacing(8);
        auto *cardGroup = new QButtonGroup(this); cardGroup->setExclusive(true);
        const QList<QPair<QString,QString>> channelNames{{"over","OVERLINK"},{"aniwie","Aniwie"},{"qwikki","qwikki"}};
        for(const auto &pair:channelNames){auto *card=new QPushButton(pair.second);card->setObjectName("channelCard");card->setCheckable(true);card->setMinimumHeight(44);card->setAccessibleName("Канал перевода "+pair.second);card->setProperty("channelId",pair.first);cardGroup->addButton(card);cardsLayout->addWidget(card,1);connect(card,&QPushButton::clicked,this,[this,pair]{const int index=channelBox->findData(pair.first);if(index>=0)channelBox->setCurrentIndex(index);});}
        channelLayout->addWidget(channelCards);
        l->addWidget(channelCard);

        progressWrap = new QWidget; progressWrap->setObjectName("transferProgress"); progressWrap->hide(); auto *progressLayout = new QVBoxLayout(progressWrap); progressLayout->setContentsMargins(0, 0, 0, 0); progressLayout->setSpacing(8);
        auto *progressHeader = new QHBoxLayout;
        progressLabel = new QLabel("Готово к проверке"); progressLabel->setObjectName("progressLabel"); progressHeader->addWidget(progressLabel, 1);
        cancelRequestBtn = new QPushButton("Отменить"); cancelRequestBtn->setObjectName("textButton"); cancelRequestBtn->setEnabled(false); progressHeader->addWidget(cancelRequestBtn);
        progressLayout->addLayout(progressHeader);
        progress = new QProgressBar; progress->setObjectName("downloadProgress"); progress->setRange(0, 100); progress->setValue(0); progress->setTextVisible(false); progress->setFixedHeight(8); progressLayout->addWidget(progress);
        l->addWidget(progressWrap);

        auto *actions = new QHBoxLayout; actions->setSpacing(10);
        autoInstallBtn = new QPushButton("Установить перевод"); autoInstallBtn->setObjectName("primaryButton"); autoInstallBtn->setMinimumHeight(48); autoInstallBtn->setEnabled(false); autoInstallBtn->setAccessibleName("Установить или восстановить текстовый перевод");
        checkVersionBtn = new QPushButton("Проверить обновления"); checkVersionBtn->setObjectName("checkUpdatesButton"); checkVersionBtn->setMinimumHeight(48);
        actions->addWidget(autoInstallBtn, 1); actions->addWidget(checkVersionBtn, 0); l->addLayout(actions);
        installAvailability = new QLabel;
        installAvailability->setObjectName("installAvailability");
        installAvailability->setWordWrap(true);
        installAvailability->hide();
        l->addWidget(installAvailability);
        stopBackgroundBtn=new QPushButton("Завершить фоновые процессы обновлятора…");
        stopBackgroundBtn->setObjectName("secondaryButton");
        stopBackgroundBtn->hide();
        l->addWidget(stopBackgroundBtn,0,Qt::AlignLeft);
        auto *moreActions=new QHBoxLayout;downloadBtn=new QPushButton("Скачать текст отдельно");downloadBtn->setObjectName("textButton");downloadBtn->setEnabled(false);verifyBtn=new QPushButton("Проверить файлы");verifyBtn->setObjectName("textButton");verifyBtn->hide();repairBtn=new QPushButton("Восстановить Mount");repairBtn->setObjectName("textButton");repairBtn->hide();uninstallBtn=new QPushButton("Удалить перевод");uninstallBtn->setObjectName("textButton");uninstallBtn->hide();moreActions->addStretch(1);moreActions->addWidget(verifyBtn,0,Qt::AlignRight);moreActions->addWidget(repairBtn,0,Qt::AlignRight);moreActions->addWidget(uninstallBtn,0,Qt::AlignRight);l->addLayout(moreActions);l->addStretch(1);
        refreshChannelsBtn = new QPushButton("Обновить список каналов"); refreshChannelsBtn->setObjectName("textButton"); refreshChannelsBtn->setCursor(Qt::PointingHandCursor);

        auto *manualSection = new QWidget; manualSection->setObjectName("manualSection"); auto *manualSectionLayout = new QVBoxLayout(manualSection); manualSectionLayout->setContentsMargins(0,0,0,0); manualSectionLayout->setSpacing(8);
        auto *manualToggle = new QToolButton; manualToggle->setObjectName("manualDisclosure"); manualToggle->setText("Ручная установка файлов"); manualToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); manualToggle->setArrowType(Qt::RightArrow); manualToggle->setCheckable(true); manualToggle->setChecked(false); manualToggle->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed); manualToggle->setMinimumHeight(40); manualToggle->setCursor(Qt::PointingHandCursor); manualSectionLayout->addWidget(manualToggle,0,Qt::AlignLeft);
        auto *manualBody = new QWidget; manualBody->setObjectName("manualBody"); manualBody->hide(); auto *manualBodyLayout = new QVBoxLayout(manualBody); manualBodyLayout->setContentsMargins(14,12,14,14); manualBodyLayout->setSpacing(10); manualSectionLayout->addWidget(manualBody);
        connect(manualToggle,&QToolButton::toggled,this,[manualToggle,manualBody](bool open){manualToggle->setArrowType(open?Qt::DownArrow:Qt::RightArrow);manualBody->setVisible(open);});
        auto *manualHint = new QLabel("Только если вы самостоятельно проверили папку назначения. Перед записью приложение покажет план и запросит подтверждение."); manualHint->setObjectName("secondaryText"); manualHint->setWordWrap(true); manualBodyLayout->addWidget(manualHint);
        auto *fileFormWidget = new QWidget; auto *fileForm = new QFormLayout(fileFormWidget); fileForm->setContentsMargins(0, 0, 0, 0); fileForm->setHorizontalSpacing(12); fileForm->setVerticalSpacing(8);
        pakEdit = new QLineEdit; pakEdit->setPlaceholderText("Проверенный или выбранный .pak"); auto *pakRow = browseRow(pakEdit, "Выбрать…", [this]{return QFileDialog::getOpenFileName(this,"Выберите PAK","","PAK (*.pak)",nullptr,QFileDialog::DontUseNativeDialog);}); fileForm->addRow("PAK",pakRow);
        sigEdit = new QLineEdit; sigEdit->setPlaceholderText("Необязательный файл с тем же именем"); auto *sigRow = browseRow(sigEdit,"Выбрать…",[this]{return QFileDialog::getOpenFileName(this,"Выберите SIG","","SIG (*.sig)",nullptr,QFileDialog::DontUseNativeDialog);}); fileForm->addRow("SIG",sigRow);
        targetEdit = new QLineEdit; targetEdit->setPlaceholderText("Выберите папку явно"); auto *targetRow = browseRow(targetEdit,"Выбрать…",[this]{return QFileDialog::getExistingDirectory(this,"Выберите папку локализации","",QFileDialog::ShowDirsOnly|QFileDialog::DontUseNativeDialog);}); fileForm->addRow("Папка",targetRow); manualBodyLayout->addWidget(fileFormWidget);
        preview = new QPlainTextEdit; preview->setObjectName("installPreview"); preview->setReadOnly(true); preview->setMinimumHeight(100); preview->setMaximumHeight(150); preview->setPlaceholderText("Предпросмотр путей и замен появится после выбора файлов."); manualBodyLayout->addWidget(preview);
        auto *installActions = new QHBoxLayout; auto *previewBtn = new QPushButton("Обновить предпросмотр"); previewBtn->setObjectName("secondaryButton"); installBtn = new QPushButton("Установить выбранные файлы…"); installBtn->setObjectName("secondaryButton"); installActions->addWidget(previewBtn); installActions->addWidget(installBtn); manualBodyLayout->addLayout(installActions);
        homeScroll->setWidget(home);
        pages->addWidget(homeScroll);

        auto *settingsScroll = new QScrollArea; settingsScroll->setWidgetResizable(true); settingsScroll->setFrameShape(QFrame::NoFrame);
        auto *settingsPage = new QWidget; settingsPage->setObjectName("settingsPage"); auto *settingsLayout = new QVBoxLayout(settingsPage); settingsLayout->setContentsMargins(24,18,24,22); settingsLayout->setSpacing(14);
        auto *settingsHeader = new QHBoxLayout; auto *settingsTitle = new QLabel("Настройки и помощь"); settingsTitle->setObjectName("pageTitle"); settingsHeader->addWidget(settingsTitle,1);
        settingsLayout->addLayout(settingsHeader);
        auto *steamCard=new QFrame;steamCard->setObjectName("surfaceCard");
        auto *steamLayout=new QVBoxLayout(steamCard);steamLayout->setContentsMargins(16,14,16,16);steamLayout->setSpacing(8);
        auto *steamTitle=new QLabel("Прямой запуск через Steam");steamTitle->setObjectName("sectionTitle");steamLayout->addWidget(steamTitle);
        auto *steamHint=new QLabel("Сверьте текущие параметры с полем в Steam: Wuthering Waves → Свойства → Общие → Параметры запуска. ECHO подготовит строку с вашими параметрами. Скопируйте её в Steam. Кнопка ECHO запустит игру напрямую, а запуск из Steam откроет официальный лаунчер. Если клиент или качество определить не удастся, откроется официальный лаунчер.");
        steamHint->setWordWrap(true);steamHint->setObjectName("secondaryText");steamLayout->addWidget(steamHint);
        const auto saved=EchoSteam::readSteamConfig(steamRoots());
        auto *steamStatus=new QLabel(saved.ok?"Последние сохранённые параметры Steam. Сверьте их с открытым клиентом перед копированием.":"Не удалось прочитать параметры Steam. Скопируйте текущую строку из Steam в поле ниже.");
        steamStatus->setWordWrap(true);steamStatus->setObjectName("secondaryText");steamLayout->addWidget(steamStatus);
        auto *currentLabel=new QLabel("Текущие параметры Steam");steamLayout->addWidget(currentLabel);
        auto *currentOptions=new QLineEdit(saved.ok?saved.options:QString());currentOptions->setObjectName("currentSteamOptions");currentOptions->setPlaceholderText("Текущие параметры запуска Steam (если есть)");steamLayout->addWidget(currentOptions);
        auto *resultLabel=new QLabel("Команда для Steam");steamLayout->addWidget(resultLabel);
        auto *steamCommand=new QLineEdit;steamCommand->setReadOnly(true);steamCommand->setObjectName("steamWrapperCommand");steamLayout->addWidget(steamCommand);
        auto *steamActions=new QHBoxLayout;
        auto *copySteamCommand=new QPushButton("Скопировать объединённую команду");copySteamCommand->setObjectName("copySteamWrapperCommand");steamActions->addWidget(copySteamCommand);
        auto *openSteamSettings=new QPushButton("Открыть свойства в Steam");steamActions->addWidget(openSteamSettings);steamActions->addStretch();steamLayout->addLayout(steamActions);
        auto *noSteamOptions=new QPushButton("В Steam поле пустое");noSteamOptions->setObjectName("noSteamOptions");steamLayout->addWidget(noSteamOptions,0,Qt::AlignLeft);
        auto prepareSteam=[currentOptions,steamCommand,copySteamCommand,steamStatus]{
            const QString appImage=qEnvironmentVariable("APPIMAGE");
            const QString executable=appImage.isEmpty()?QCoreApplication::applicationFilePath():appImage;
            const auto result=EchoSteamOptions::merge(currentOptions->text(),executable);
            steamCommand->setText(result.ok?result.options:QString());
            steamCommand->setCursorPosition(0);
            copySteamCommand->setText("Скопировать объединённую команду");
            copySteamCommand->setEnabled(result.ok);
            if(!result.ok)steamStatus->setText(result.error);
            else steamStatus->setText("Сверьте исходную строку с текущими параметрами в Steam. Готовая строка сохраняет исходные параметры.");
        };
        connect(currentOptions,&QLineEdit::textChanged,this,[prepareSteam]{prepareSteam();});prepareSteam();
        connect(noSteamOptions,&QPushButton::clicked,this,[currentOptions,prepareSteam]{currentOptions->clear();prepareSteam();});
        if(!saved.ok){steamStatus->setText(saved.error+" Скопируйте текущую строку из Steam в поле выше.");steamCommand->clear();copySteamCommand->setEnabled(false);}
        connect(copySteamCommand,&QPushButton::clicked,this,[steamCommand,copySteamCommand]{QApplication::clipboard()->setText(steamCommand->text());copySteamCommand->setText("Команда скопирована");});
        connect(openSteamSettings,&QPushButton::clicked,this,[]{QDesktopServices::openUrl(QUrl("steam://gameproperties/3513350"));});
        settingsLayout->addWidget(steamCard);
        // Settings sections share the same card/disclosure language as the main page.
        auto *diagnosticsGroup = new QFrame;
        diagnosticsGroup->setObjectName("surfaceCard");
        auto *diagLayout = new QVBoxLayout(diagnosticsGroup);
        diagLayout->setContentsMargins(16,12,16,14);
        diagLayout->setSpacing(8);
        auto *diagnosticsToggle = new QToolButton;
        diagnosticsToggle->setObjectName("sectionDisclosure");
        diagnosticsToggle->setText("Диагностика");
        diagnosticsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        diagnosticsToggle->setArrowType(Qt::RightArrow);
        diagnosticsToggle->setCheckable(true);
        diagnosticsToggle->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
        diagLayout->addWidget(diagnosticsToggle);
        auto *diagBody = new QWidget; diagBody->setObjectName("diagnosticsBody");
        auto *diagBodyLayout = new QVBoxLayout(diagBody);
        diagBodyLayout->setContentsMargins(0,4,0,0);
        diagBodyLayout->setSpacing(8);
        auto *diagButtons=new QHBoxLayout;diagButtons->setSpacing(8);
        auto *refreshBtn=new QPushButton("Обновить");
        auto *openLogBtn=new QPushButton("Открыть лог");
        auto *resetBtn=new QPushButton("Сбросить настройки");
        diagButtons->addWidget(refreshBtn);diagButtons->addWidget(openLogBtn);diagButtons->addWidget(resetBtn);
        diagBodyLayout->addLayout(diagButtons);
        diagnostic=new QPlainTextEdit;diagnostic->setReadOnly(true);diagnostic->setMinimumHeight(180);
        diagBodyLayout->addWidget(diagnostic);diagLayout->addWidget(diagBody);
        diagBody->hide();
        connect(diagnosticsToggle,&QToolButton::toggled,this,[diagnosticsToggle,diagBody](bool expanded){
            diagnosticsToggle->setArrowType(expanded?Qt::DownArrow:Qt::RightArrow);
            diagBody->setVisible(expanded);
        });
        settingsLayout->addWidget(diagnosticsGroup);

        auto *advanced = new QFrame;advanced->setObjectName("surfaceCard");
        auto *advancedLayout = new QVBoxLayout(advanced);
        advancedLayout->setContentsMargins(16,14,16,16);advancedLayout->setSpacing(8);
        auto *advancedTitle = new QLabel("Дополнительно");
        advancedTitle->setObjectName("sectionTitle");
        advancedLayout->addWidget(advancedTitle);
        advancedLayout->addWidget(downloadBtn,0,Qt::AlignLeft);
        advancedLayout->addWidget(refreshChannelsBtn,0,Qt::AlignLeft);
        advancedLayout->addWidget(manualSection);
        settingsLayout->addWidget(advanced);settingsLayout->addStretch(1);
        settingsScroll->setWidget(settingsPage); pages->addWidget(settingsScroll);
        connect(localizerNav,&QPushButton::clicked,this,[pages,localizerNav,settingsNav]{pages->setCurrentIndex(0);localizerNav->setChecked(true);settingsNav->setChecked(false);});
        connect(settingsNav,&QPushButton::clicked,this,[pages,localizerNav,settingsNav]{pages->setCurrentIndex(1);settingsNav->setChecked(true);localizerNav->setChecked(false);});

        loadSettings(); loadChannelCache();
        for(auto *button:cardGroup->buttons())button->setChecked(button->property("channelId").toString()==channelBox->currentData().toString());
        refreshStatus(); updatePreview(); refreshDiagnostics();
        auto persistInstallInputs=[this]{ saveInstallSettings(); updatePreview(); };
        connect(pakEdit,&QLineEdit::editingFinished,this,persistInstallInputs); connect(sigEdit,&QLineEdit::editingFinished,this,persistInstallInputs); connect(targetEdit,&QLineEdit::editingFinished,this,persistInstallInputs);
        for (auto *edit : {pakEdit,sigEdit,targetEdit}) connect(edit,&QLineEdit::textChanged,this,[this]{updatePreview();});
        connect(previewBtn,&QPushButton::clicked,this,[this]{updatePreview();});
        connect(installBtn,&QPushButton::clicked,this,[this]{installSelected();});
        connect(refreshChannelsBtn,&QPushButton::clicked,this,[this]{fetchChannelIndex();});
        connect(checkVersionBtn,&QPushButton::clicked,this,[this]{checkSelectedChannel();});
        connect(autoInstallBtn,&QPushButton::clicked,this,[this]{
            if(selectedInstallState().repairable) repairInstalledMount();
            else beginAutoInstall();
        });
        connect(stopBackgroundBtn,&QPushButton::clicked,this,[this]{
            const auto games=discoverWutheringInstallations(steamRoots());
            if(games.size()!=1)return;
            const auto processes=EchoInstall::blockingProcesses(games.first().gameDir);
            if(processes.isEmpty()){refreshAutoInstallControls();return;}
            for(const auto &p:processes)if(!p.updater){
                QMessageBox::information(this,"Игра работает","Сначала закройте игру или основной лаунчер обычным способом.");
                return;
            }
            const QString warning=QStringLiteral(R"MSG(Обнаружены фоновые процессы обновления Wuthering Waves без основного игрового окна.
Их принудительное завершение может прервать скачивание или установку обновлений.
Продолжать безопасно только если обновление зависло и вы не хотите его ждать.

Завершить эти процессы?)MSG");
            if(QMessageBox::warning(this,"Завершить обновление?",warning,QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
            QString message;
            EchoInstall::terminateBackgroundUpdaters(games.first().gameDir,&message);
            channelStatus->setText(message);
            refreshAutoInstallControls();
        });
        connect(uninstallBtn,&QPushButton::clicked,this,[this]{removeTranslation();});
        connect(verifyBtn,&QPushButton::clicked,this,[this]{verifyInstalledFiles();});
        connect(repairBtn,&QPushButton::clicked,this,[this]{repairInstalledMount();});
        connect(channelBox,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{updateChannelStatus();});
        connect(channelBox,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,cardGroup](int){const QString id=channelBox->currentData().toString();for(auto *button:cardGroup->buttons())button->setChecked(button->property("channelId").toString()==id);});
        connect(cancelRequestBtn,&QPushButton::clicked,this,[this]{if(manifestReply){networkCancelled=true;channelStatus->setText("Запрос отменён пользователем; временный файл будет удалён.");manifestReply->abort();}});
        connect(downloadBtn,&QPushButton::clicked,this,[this]{downloadSelectedPackage();});
        connect(launchBtn,&QPushButton::clicked,this,[this]{launchGame();});
        connect(refreshBtn,&QPushButton::clicked,this,[this]{refreshStatus();refreshDiagnostics();});
        connect(openLogBtn,&QPushButton::clicked,this,[this]{QFile f(logPath);if(f.open(QIODevice::ReadOnly|QIODevice::Text))diagnostic->setPlainText(QString::fromUtf8(f.readAll()));else QMessageBox::information(this,"Лог","Лог запуска пока не создан.");});
        connect(resetBtn,&QPushButton::clicked,this,[this]{settings.clear();settings.sync();pakEdit->clear();sigEdit->clear();targetEdit->clear();saveInstallSettings();refreshStatus();refreshDiagnostics();updatePreview();});
        auto *gameWatch=new QTimer(this);
        connect(gameWatch,&QTimer::timeout,this,[this]{refreshAutoInstallControls();});
        gameWatch->start(3000);
        updateChannelStatus(); refreshAutoInstallControls();
        if(autoCheck){startupAutoCheck=true;QTimer::singleShot(1500,this,[this]{fetchChannelIndex();});auto *updateTimer=new QTimer(this);connect(updateTimer,&QTimer::timeout,this,[this]{if(!busyNetwork)checkSelectedChannel();});updateTimer->start(30*60*1000);}
        if(smoke) smokeChecks();
    }
private:
    QSettings settings; QString settingsPath,appDataPath,logPath; QLineEdit *pakEdit{},*sigEdit{},*targetEdit{}; QComboBox *channelBox{}; QLabel *statusLabel{},*gameDot{},*progressLabel{},*channelStatus{},*versionStatus{},*installAvailability{}; QPushButton *launchBtn{},*installBtn{},*refreshChannelsBtn{},*checkVersionBtn{},*cancelRequestBtn{},*downloadBtn{},*autoInstallBtn{},*uninstallBtn{},*verifyBtn{},*repairBtn{},*stopBackgroundBtn{}; QPlainTextEdit *diagnostic{},*preview{}; QProgressBar *progress{}; QWidget *progressWrap{}; QNetworkAccessManager *network{}; QNetworkReply *manifestReply{}; QTimer *requestTimer{}; QFile *downloadFile{}; QString downloadPartPath{},downloadFinalPath{},downloadExpectedHash{},downloadName{},downloadChannel{}; QList<TranslationChannel> channels; QHash<QString,TranslationManifest> manifests; bool busyNetwork{},networkCancelled{},downloadingPackage{},pendingAutoInstall{},startupAutoCheck{}; QString pendingInstallChannel,pendingInstallGame,pendingInstallResourceVersion,pendingInstallTranslationVersion;
    QString requestedChannel;
    void setChannels(const QList<TranslationChannel> &items){channels=items;const QString selected=settings.value("localizer/channel","over").toString();channelBox->clear();for(const auto &c:channels)channelBox->addItem(c.name,c.id);int index=channelBox->findData(selected);channelBox->setCurrentIndex(index<0?0:index);updateChannelStatus();}
    void loadChannelCache(){
        QString error; const QByteArray cached=settings.value("localizer/channelIndex").toByteArray();
        if(!cached.isEmpty()&&parseChannelIndex(cached,&channels,&error)){setChannels(channels);channelStatus->setText("Офлайн: загружен последний проверенный список каналов.");}
        else {QByteArray known=R"({"TranslationChannels":[{"Id":"over","Name":"OVERLINK","Description":"Перевод OVERLINK","ManifestUrl":"https://gitlab.com/ECHOLaunch/updates/-/raw/main/wwmain/channels/manifest_over.json","Enabled":true},{"Id":"aniwie","Name":"Aniwie","Description":"Перевод Aniwie","ManifestUrl":"https://gitlab.com/ECHOLaunch/updates/-/raw/main/wwmain/channels/manifest_aniwie.json","Enabled":true},{"Id":"qwikki","Name":"qwikki","Description":"Перевод qwikki","ManifestUrl":"https://gitlab.com/ECHOLaunch/updates/-/raw/main/wwmain/channels/manifest_qwikki.json","Enabled":true}]})";if(parseChannelIndex(known,&channels,&error)){setChannels(channels);channelStatus->setText("Офлайн: используются встроенные адреса каналов из проверенного списка проекта.");}else channelStatus->setText("Не удалось загрузить список каналов: "+error);}
        for(const auto &c:channels){const QByteArray raw=settings.value("localizer/manifest/"+c.id).toByteArray();TranslationManifest m;if(!raw.isEmpty()&&parseTranslationManifest(raw,c.id,&m,&error))manifests.insert(c.id,m);}
        updateChannelStatus();
    }
    void finishRequest(QNetworkReply *reply,const QByteArray &body,const QString &purpose){
        busyNetwork=false;manifestReply=nullptr;if(requestTimer)requestTimer->stop();cancelRequestBtn->setEnabled(false);progress->setRange(0,100);progress->setValue(0);progressWrap->hide();
        if(networkCancelled){networkCancelled=false;pendingAutoInstall=false;startupAutoCheck=false;channelBox->setEnabled(true);reply->deleteLater();return;}
        const int code=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto redirect=reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        QString error;
        if(!redirect.isEmpty()) error="Сервер перенаправил запрос; перенаправления запрещены для безопасной проверки источника.";
        else if(reply->error()!=QNetworkReply::NoError||code<200||code>=300)error="Ошибка HTTPS запроса ("+QString::number(code)+"): "+reply->errorString();
        else if(purpose=="index") {QList<TranslationChannel> parsed;if(!parseChannelIndex(body,&parsed,&error)){}else{channels=parsed;settings.setValue("localizer/channelIndex",body);settings.sync();setChannels(parsed);channelStatus->setText("Список каналов обновлён с официального GitLab; манифест перевода ещё не проверен.");reply->deleteLater();if(startupAutoCheck){startupAutoCheck=false;QTimer::singleShot(0,this,[this]{checkSelectedChannel();});}return;}}
        else {const QString id=requestedChannel;TranslationManifest manifest;if(!parseTranslationManifest(body,id,&manifest,&error)){}else{
            manifests.insert(id,manifest);settings.setValue("localizer/manifest/"+id,body);settings.sync();updateChannelStatus();reply->deleteLater();
            if(pendingAutoInstall)QTimer::singleShot(0,this,[this]{beginAutoInstall();});
            else if(channelBox->currentData().toString()==id && QFileInfo::exists(appDataPath+"/translation-install.json")){
                const auto games=discoverWutheringInstallations(steamRoots());
                const QString rv=games.size()==1?gameResourceVersion(games.first().gameDir):QString();
                const auto state=selectedInstallState();
                const QString cached=appDataPath+"/downloads/"+id+"/"+manifest.version+"/"+manifest.textHash.toLower()+"/"+manifest.textFileName;
                if(!rv.isEmpty()&&normalizedVersion(rv).section('.',0,1)==normalizedVersion(manifest.version).section('.',0,1)&&
                   (!state.healthy||state.packageDiffers||state.resourceVersionChanged||state.selectedChannelDiffers)&&!QFileInfo::exists(cached))
                    QTimer::singleShot(0,this,[this,id]{if(!busyNetwork&&!pendingAutoInstall&&channelBox->currentData().toString()==id)downloadSelectedPackage();});
            }
            return;
        }}
        if(pendingAutoInstall){pendingAutoInstall=false;channelBox->setEnabled(true);}updateChannelStatus();channelStatus->setText(error+" Используется кэш, если он есть.");reply->deleteLater();if(startupAutoCheck){startupAutoCheck=false;QTimer::singleShot(0,this,[this]{checkSelectedChannel();});}
    }
    void finishPackageDownload(QNetworkReply *reply) {
        if (requestTimer) requestTimer->stop();
        const bool wasCancelled = networkCancelled;
        networkCancelled = false; busyNetwork = false; downloadingPackage = false; manifestReply = nullptr;
        cancelRequestBtn->setEnabled(false); progress->setRange(0, 100); progressWrap->hide(); downloadBtn->setEnabled(manifests.contains(channelBox->currentData().toString()));
        QString error;
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        if (downloadFile && downloadFile->isOpen()) {
            const QByteArray tail = reply->readAll();
            if (!tail.isEmpty() && downloadFile->write(tail) != tail.size()) error = "Ошибка записи последнего блока загрузки: " + downloadFile->errorString();
            if (!downloadFile->flush() && error.isEmpty()) error = "Не удалось сбросить временный пакет на диск: " + downloadFile->errorString();
            downloadFile->close();
        }
        if (wasCancelled) error = "Загрузка отменена.";
        else if (!redirect.isEmpty()) error = "Сервер перенаправил загрузку; перенаправления запрещены.";
        else if (reply->error() != QNetworkReply::NoError || code < 200 || code >= 300) error = "Ошибка HTTPS загрузки ("+QString::number(code)+"): "+reply->errorString();
        if (error.isEmpty() && !verifyAndPromotePackage(downloadPartPath,downloadFinalPath,downloadExpectedHash,&error)) {}
        if (downloadFile) { delete downloadFile; downloadFile = nullptr; }
        if (!error.isEmpty()) {
            pendingAutoInstall=false;
            channelBox->setEnabled(true);
            QFile::remove(downloadPartPath);
            channelStatus->setText(error+" Временный файл удалён; существующие файлы не изменены."); progressLabel->setText("Загрузка не завершена");
            progress->setValue(0);
        } else {
            if (downloadName.endsWith(".pak", Qt::CaseInsensitive)) { pakEdit->setText(downloadFinalPath); saveInstallSettings(); updatePreview(); }
            channelStatus->setText("Скачан и проверен · XXH64 совпал с манифестом. Файл сохранён в кеше приложения."); progressLabel->setText("Скачан и проверен");
            progress->setValue(100);
            if(pendingAutoInstall)QTimer::singleShot(0,this,[this]{installCachedTranslation();});
        }
        downloadPartPath.clear(); downloadFinalPath.clear(); reply->deleteLater(); refreshDiagnostics();
    }
    void downloadSelectedPackage() {
        auto stopPending=[this](){if(pendingAutoInstall){pendingAutoInstall=false;channelBox->setEnabled(true);}};
        if (busyNetwork) { stopPending();channelStatus->setText("Предыдущий сетевой запрос ещё выполняется."); return; }
        const QString id = channelBox->currentData().toString();
        const auto it = manifests.constFind(id);
        if (it == manifests.cend()) { stopPending();channelStatus->setText("Сначала нажмите «Проверить версию»."); return; }
        const QString urlText = it->textUrl;
        const QString name = it->textFileName;
        const QString expectedHash = it->textHash;
        const QUrl url(urlText);
        if (!safePakName(name) || !QRegularExpression("^[0-9a-fA-F]{16}$").match(expectedHash).hasMatch() || !trustedGitlabRawUrl(url)) {
            stopPending();channelStatus->setText("Загрузка заблокирована: пакет не прошёл проверку имени, хеша или разрешённого адреса."); return;
        }
        const QString cacheDir = appDataPath+"/downloads/"+id+"/"+it->version+"/"+expectedHash.toLower();
        if (!QDir().mkpath(cacheDir) || QFileInfo(cacheDir).isSymLink()) { stopPending();channelStatus->setText("Не удалось безопасно создать каталог кеша приложения."); return; }
        const QString finalPath = QDir(cacheDir).filePath(name);
        if (QFileInfo::exists(finalPath)) {
            if (QFileInfo(finalPath).isSymLink()) { stopPending();channelStatus->setText("Путь кеша является символической ссылкой; он не будет прочитан или заменён."); return; }
            QString hashError; const QString cachedHash=xxh64File(finalPath,&hashError);
            if (cachedHash.compare(expectedHash,Qt::CaseInsensitive)==0) {
                pakEdit->setText(finalPath); saveInstallSettings(); updatePreview();
                channelStatus->setText("Кешированный пакет повторно проверен XXH64: "+finalPath); progress->setValue(100); if(pendingAutoInstall)QTimer::singleShot(0,this,[this]{installCachedTranslation();}); return;
            }
            const QString quarantine=finalPath+".invalid-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
            if(!QFile::rename(finalPath,quarantine)){stopPending();channelStatus->setText("Повреждённый файл кеша не удалось сохранить отдельно; загрузка остановлена.");return;}
        }
        downloadPartPath=QDir(cacheDir).filePath("."+name+"."+QUuid::createUuid().toString(QUuid::WithoutBraces)+".part");
        downloadFinalPath=finalPath; downloadExpectedHash=expectedHash.toLower(); downloadName=name; downloadChannel=id;
        downloadFile=new QFile(downloadPartPath,this);
        if (!downloadFile->open(QIODevice::WriteOnly|QIODevice::NewOnly)) { stopPending();channelStatus->setText("Не удалось создать временный файл загрузки: "+downloadFile->errorString()); delete downloadFile; downloadFile=nullptr; return; }
        QNetworkRequest request(url); request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
        request.setHeader(QNetworkRequest::UserAgentHeader,"ECHO-Native/0.1 verified-package-download");
        manifestReply=network->get(request); manifestReply->setReadBufferSize(1024*1024); busyNetwork=true; downloadingPackage=true; networkCancelled=false;
        cancelRequestBtn->setEnabled(true); progress->setRange(0,100); progress->setValue(0);progressWrap->show();
        channelStatus->setText("Загрузка текстового перевода во временный кеш…"); progressLabel->setText("Загрузка перевода…");
        auto *reply=manifestReply; requestTimer=new QTimer(this); requestTimer->setSingleShot(true);
        connect(requestTimer,&QTimer::timeout,this,[this,reply]{if(reply&&reply==manifestReply){channelStatus->setText("Тайм-аут загрузки; временный файл будет удалён.");reply->abort();}}); requestTimer->start(10*60*1000);
        connect(reply,&QNetworkReply::metaDataChanged,this,[this,reply]{const QVariant length=reply->header(QNetworkRequest::ContentLengthHeader);if(length.isValid()&&length.toLongLong()>MaxDownloadBytes){channelStatus->setText("Размер пакета превышает лимит 4 ГиБ; загрузка остановлена.");reply->abort();}});
        connect(reply,&QIODevice::readyRead,this,[this,reply]{
            if (!downloadFile || !downloadFile->isOpen()) { reply->abort(); return; }
            const QByteArray block=reply->readAll();
            if (block.size()>0 && (downloadFile->size()+block.size()>MaxDownloadBytes || downloadFile->write(block)!=block.size())) {
                channelStatus->setText(downloadFile->size()+block.size()>MaxDownloadBytes?"Пакет превысил лимит 4 ГиБ; загрузка остановлена.":"Ошибка записи временного пакета: "+downloadFile->errorString()); reply->abort(); return;
            }
        });
        connect(reply,&QNetworkReply::downloadProgress,this,[this](qint64 received,qint64 total){
            if(total>MaxDownloadBytes){channelStatus->setText("Размер пакета превышает лимит 4 ГиБ; загрузка остановлена.");if(manifestReply)manifestReply->abort();return;}
            if(total>0){progress->setRange(0,100);progress->setValue(static_cast<int>(std::min<qint64>(100,received*100/total)));channelStatus->setText("Загрузка: "+QLocale().formattedDataSize(received)+" / "+QLocale().formattedDataSize(total));progressLabel->setText("Загрузка перевода · "+QString::number(std::min<qint64>(100,received*100/total))+"%");}
            else {channelStatus->setText("Загрузка: "+QLocale().formattedDataSize(received));progressLabel->setText("Загрузка перевода · "+QLocale().formattedDataSize(received));}
        });
        connect(reply,&QNetworkReply::finished,this,[this,reply]{finishPackageDownload(reply);});
    }
    void requestManifest(const QUrl &url,const QString &purpose){
        if(busyNetwork){channelStatus->setText("Предыдущий сетевой запрос ещё выполняется.");return;}
        if(!trustedGitlabRawUrl(url)){channelStatus->setText("Запрос заблокирован: адрес не входит в разрешённый HTTPS путь GitLab.");return;}
        QNetworkRequest request(url);request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);request.setHeader(QNetworkRequest::UserAgentHeader,"ECHO-Native/0.1 manifest-check");
        manifestReply=network->get(request);busyNetwork=true;cancelRequestBtn->setEnabled(true);progressWrap->show();progress->setRange(0,0);auto *reply=manifestReply;requestTimer=new QTimer(this);requestTimer->setSingleShot(true);connect(requestTimer,&QTimer::timeout,this,[this,reply]{if(reply&&reply==manifestReply){channelStatus->setText("Тайм-аут HTTPS запроса; кэшированное состояние сохранено.");reply->abort();}});requestTimer->start(15000);
        connect(reply,&QNetworkReply::downloadProgress,this,[this](qint64 received,qint64 total){if(total>0&&total<=MaxManifestBytes){progress->setRange(0,100);progress->setValue(static_cast<int>(std::min<qint64>(100,received*100/total)));channelStatus->setText("Получение манифеста: "+QLocale().formattedDataSize(received)+" / "+QLocale().formattedDataSize(total));}});
        connect(reply,&QIODevice::readyRead,this,[this,reply]{if(reply->bytesAvailable()>MaxManifestBytes){channelStatus->setText("Ответ превысил лимит манифеста; запрос остановлен.");reply->abort();}});
        connect(reply,&QNetworkReply::finished,this,[this,reply,purpose]{const QByteArray body=reply->readAll();finishRequest(reply,body,purpose);});
    }
    void fetchChannelIndex(){channelStatus->setText("Получение списка каналов…");requestManifest(QUrl(ChannelIndexUrl),"index");}
    void checkSelectedChannel(){if(busyNetwork){channelStatus->setText("Предыдущий сетевой запрос ещё выполняется.");return;}const QString id=channelBox->currentData().toString();for(const auto &c:channels)if(c.id==id){requestedChannel=id;channelStatus->setText("Проверка манифеста "+c.name+"…");requestManifest(QUrl(c.manifestUrl),"channel");return;}channelStatus->setText("Сначала обновите список каналов.");}
    QString installedResourceVersion()const {
        for(const auto &game:discoverWutheringInstallations(steamRoots())) {
            const QString found=EchoInstall::detectResourceVersion(game.gameDir);
            if(!found.isEmpty())return found;
        }
        return {};
    }
    QString gameResourceVersion(const QString &gameRoot)const {
        return gameRoot.isEmpty()?QString():EchoInstall::detectResourceVersion(gameRoot);
    }
    static QString normalizedVersion(QString version){while(version.endsWith(".0"))version.chop(2);return version;}
    EchoInstall::Inspection selectedInstallState() const {
        const auto games=discoverWutheringInstallations(steamRoots());
        const QString id=channelBox->currentData().toString();const auto it=manifests.constFind(id);
        if(games.size()!=1||games.first().gameDir.isEmpty())return {};
        return EchoInstall::inspect(appDataPath,games.first().gameDir,gameResourceVersion(games.first().gameDir),id,it==manifests.cend()?QString():it->textHash);
    }
    void verifyInstalledFiles(){
        const auto games=discoverWutheringInstallations(steamRoots());if(games.size()!=1){channelStatus->setText(games.isEmpty()?"Проверка: Steam-установка Wuthering Waves не найдена.":"Проверка остановлена: найдено несколько установок игры.");return;}
        const auto state=selectedInstallState();channelStatus->setText(state.message);refreshAutoInstallControls();
    }
    void repairInstalledMount(){
        const auto games=discoverWutheringInstallations(steamRoots());if(games.size()!=1){channelStatus->setText("Восстановление доступно только при одной обнаруженной Steam-установке.");return;}
        const auto state=selectedInstallState();if(!state.repairable){channelStatus->setText(state.message);refreshAutoInstallControls();return;}
        const auto result=EchoInstall::repair(appDataPath,state.currentResourceVersion);
        logLine("Translation repair: "+result.message);
        refreshAutoInstallControls();
        channelStatus->setText(result.ok?"Перевод восстановлен. Можно играть.":"Не удалось восстановить перевод: "+result.message);
        versionStatus->setText(result.ok?"Перевод восстановлен":"Восстановление не выполнено");
    }
    void refreshAutoInstallControls(){
        const auto games=discoverWutheringInstallations(steamRoots());
        EchoInstall::Inspection state;if(games.size()==1)state=selectedInstallState();
        verifyBtn->setVisible(state.owned);verifyBtn->setEnabled(state.owned&&!busyNetwork);
        repairBtn->setVisible(false);repairBtn->setEnabled(state.repairable&&!busyNetwork);
        uninstallBtn->setVisible(state.healthy||state.repairable);uninstallBtn->setEnabled((state.healthy||state.repairable)&&!busyNetwork);
        if(gameDot)
            gameDot->setStyleSheet(games.isEmpty()?"color: #d17474;":(state.owned&&!state.healthy?"color: #d2a25f;":"color: #83b89b;"));
        QString primary="Установить перевод";
        if(state.owned&&state.resourceVersionChanged)primary="Установить для новой версии";
        else if(state.healthy){if(state.selectedChannelDiffers)primary="Сменить перевод";else if(state.packageDiffers)primary="Обновить перевод";else primary="Перевод установлен";}
        else if(state.repairable)primary="Восстановить перевод";
        else if(state.reinstallable)primary="Установить перевод повторно";
        else if(state.owned&&!state.healthy)primary="Файлы требуют проверки";
        autoInstallBtn->setText(primary);
        autoInstallBtn->setVisible(!(state.healthy&&!state.resourceVersionChanged&&!state.selectedChannelDiffers&&!state.packageDiffers));
        autoInstallBtn->setEnabled(!busyNetwork&&games.size()==1&&(state.resourceVersionChanged||state.repairable||state.reinstallable||!state.owned||(state.healthy&&(state.selectedChannelDiffers||state.packageDiffers))));
        QString reason;
        stopBackgroundBtn->hide();
        if(games.size()>1)
            reason="Найдено несколько установок Wuthering Waves.";
        else if(games.isEmpty())
            reason="Не обнаружена Steam-установка Wuthering Waves.";
        else if(games.first().gameDir.isEmpty())
            reason=games.first().discoveryError;
        else {
            const auto processes=EchoInstall::blockingProcesses(games.first().gameDir);
            if(!processes.isEmpty()){
                bool onlyUpdates=true;
                QStringList details;
                for(const auto &p:processes){
                    details<<p.name;
                    if(!p.updater)onlyUpdates=false;
                }
                reason=(onlyUpdates?"Окно игры закрыто, но работают фоновые процессы обновлятора: ":
                        "Игра или лаунчер всё ещё работает: ")+details.join("; ")+".";
                stopBackgroundBtn->setVisible(onlyUpdates);
                stopBackgroundBtn->setEnabled(onlyUpdates&&!busyNetwork);
            } else {
                const QString rv=gameResourceVersion(games.first().gameDir);
                reason=EchoInstall::englishMountPreflight(games.first().gameDir,rv);
            }
        }
        if(busyNetwork)reason="Выполняется сетевой запрос; дождитесь завершения или нажмите «Отменить».";
        if(games.size()==1){const QString gameRoot=games.first().gameDir;const QString rv=gameResourceVersion(gameRoot);statusLabel->setText(gameRoot.isEmpty()?games.first().discoveryError:"Wuthering Waves · "+rv+" · "+(state.owned?state.message:"Steam игра обнаружена"));}
        else statusLabel->setText(games.isEmpty()?"Wuthering Waves не найдена в библиотеках Steam.":"Найдено несколько установок Wuthering Waves.");
        if(!reason.isEmpty())autoInstallBtn->setEnabled(false);
        installAvailability->setVisible(!reason.isEmpty());
        installAvailability->setText(reason);
        autoInstallBtn->setToolTip(reason.isEmpty()?"Установить текстовый перевод.":reason);
    }
    void beginAutoInstall(){
        auto clearPending=[this](){pendingAutoInstall=false;pendingInstallChannel.clear();pendingInstallGame.clear();pendingInstallResourceVersion.clear();pendingInstallTranslationVersion.clear();channelBox->setEnabled(true);};
        if(busyNetwork){channelStatus->setText("Дождитесь завершения текущего запроса.");return;}
        const auto games=discoverWutheringInstallations(steamRoots());if(games.size()!=1){clearPending();channelStatus->setText(games.isEmpty()?"Установка недоступна: не найден Steam manifest AppID 3513350.":"Установка заблокирована: найдено несколько игровых установок Steam.");return;}
        if(games.first().gameDir.isEmpty()){clearPending();channelStatus->setText(games.first().discoveryError);return;}
        if(EchoInstall::gameRunning(games.first().gameDir)){
            clearPending();
            const QString why="Wuthering Waves продолжает выполнять фоновые процессы. Причина показана под кнопкой установки. Если остался только зависший обновлятор, завершите его отдельной кнопкой после проверки.";
            channelStatus->setText(why);
            refreshAutoInstallControls();
            QMessageBox::information(this,"Установка пока недоступна",why);
            return;
        }
        const QString id=pendingAutoInstall&&!pendingInstallChannel.isEmpty()?pendingInstallChannel:channelBox->currentData().toString();if(!pendingAutoInstall){pendingAutoInstall=true;pendingInstallChannel=id;pendingInstallGame=games.first().gameDir;pendingInstallResourceVersion=gameResourceVersion(games.first().gameDir);channelBox->setEnabled(false);checkSelectedChannel();return;}
        if(!manifests.contains(id)){clearPending();channelStatus->setText("Не удалось получить актуальный манифест перевода.");return;}
        const QString version=gameResourceVersion(games.first().gameDir);const auto m=manifests.value(id);
        const QString mountProblem=EchoInstall::englishMountPreflight(games.first().gameDir,version);
        if(!mountProblem.isEmpty()){clearPending();channelStatus->setText(mountProblem);refreshAutoInstallControls();return;}
        if(normalizedVersion(version).section('.',0,1)!=normalizedVersion(m.version).section('.',0,1)){
            clearPending();channelStatus->setText(QString("Версия ресурсов игры %1 отличается от перевода %2; установка остановлена.").arg(version,m.version));return;
        }
        if(pendingAutoInstall&&(!pendingInstallGame.isEmpty()&&(pendingInstallGame!=games.first().gameDir||pendingInstallResourceVersion!=version))){clearPending();channelStatus->setText("Установка остановлена: игра или версия ресурсов изменились после начала проверки.");return;}
        pendingInstallChannel=id;pendingInstallGame=games.first().gameDir;pendingInstallResourceVersion=version;pendingInstallTranslationVersion=m.version;
        const QString resource=games.first().gameDir+"/Client/Saved/Resources/"+version;
        const QString pak=resource+"/Lang_en/RU/pakchunk0-WindowsNoEditor_10100_P.pak";
        const QString mount=resource+"/Mount/"+EchoInstall::translationMountForInstall(games.first().gameDir,version,appDataPath);
        const QString prompt=QString("Канал: %1 v%2\nИгра: %3\n\nPAK будет установлен в:\n%4\n\nMount будет обновлён:\n%5\n\nФайл будет загружен при необходимости и проверен XXH64. В Mount добавится экспериментальная запись с приоритетом 10100. Игра должна быть закрыта. Продолжить?").arg(channelBox->currentText(),m.version,games.first().gameDir,pak,mount);
        if(QMessageBox::question(this,"Подтвердите установку перевода",prompt,QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes){clearPending();channelStatus->setText("Установка отменена. Файлы игры не изменены.");return;}
        pendingAutoInstall=true;channelBox->setEnabled(false);downloadSelectedPackage();
    }
    void installCachedTranslation(){
        pendingAutoInstall=false;const auto games=discoverWutheringInstallations(steamRoots());const QString id=pendingInstallChannel;const QString expectedGame=pendingInstallGame,expectedResource=pendingInstallResourceVersion,expectedTranslation=pendingInstallTranslationVersion;
        if(games.size()!=1||games.first().gameDir.isEmpty()||!manifests.contains(id)||games.first().gameDir!=expectedGame){channelBox->setEnabled(true);channelStatus->setText("Установка остановлена: Steam install или манифест изменились.");return;}
        const auto m=manifests.value(id);const QString version=gameResourceVersion(games.first().gameDir);const QString root=games.first().gameDir+"/Client/Saved/Resources/"+version;
        const QString mountProblem=EchoInstall::englishMountPreflight(games.first().gameDir,version);
        if(!mountProblem.isEmpty()){channelBox->setEnabled(true);channelStatus->setText(mountProblem);refreshAutoInstallControls();return;}
        const QString cache=appDataPath+"/downloads/"+id+"/"+m.version+"/"+m.textHash.toLower();const QString pak=cache+"/"+m.textFileName;
        QString err;if(EchoInstall::gameRunning(games.first().gameDir)){channelBox->setEnabled(true);channelStatus->setText("Установка остановлена: закройте игру.");return;}
        if(version!=expectedResource||m.version!=expectedTranslation||version.isEmpty()||normalizedVersion(version).section('.',0,1)!=normalizedVersion(m.version).section('.',0,1)||!QFileInfo(pak).isFile()||xxh64File(pak,&err).compare(m.textHash,Qt::CaseInsensitive)!=0){channelBox->setEnabled(true);channelStatus->setText("Проверенный пакет, выбранный канал или версия ресурсов изменились: "+err);return;}
        auto result=EchoInstall::install(games.first().gameDir,version,id,m.version,pak,m.textHash,appDataPath);
        channelBox->setEnabled(true);pendingInstallChannel.clear();pendingInstallGame.clear();pendingInstallResourceVersion.clear();pendingInstallTranslationVersion.clear();channelStatus->setText(result.message);versionStatus->setText(result.ok?EchoInstall::installedStatus(appDataPath,games.first().gameDir):"Установка не завершена");refreshAutoInstallControls();refreshDiagnostics();
    }
    void removeTranslation(){
        const auto games=discoverWutheringInstallations(steamRoots());if(games.size()!=1){channelStatus->setText("Удаление заблокировано: требуется ровно одна найденная установка Steam.");return;}
        if(EchoInstall::gameRunning(games.first().gameDir)){channelStatus->setText("Закройте Wuthering Waves и повторите удаление.");return;}
        const auto answer=QMessageBox::question(this,"Удалить перевод?","Будет удалён только проверенный PAK ECHO и его собственная строка в таблице ресурсов. Остальные строки сохраняются; перед изменением создаётся резервная копия.",QMessageBox::Yes|QMessageBox::No,QMessageBox::No);
        if(answer!=QMessageBox::Yes)return;
        const auto result=EchoInstall::uninstall(appDataPath);channelStatus->setText(result.message);if(result.ok)versionStatus->setText("Перевод удалён");refreshAutoInstallControls();refreshDiagnostics();
    }
    void updateChannelStatus(){
        refreshAutoInstallControls();
        const QString id=channelBox->currentData().toString();
        if(!id.isEmpty()){settings.setValue("localizer/channel",id);settings.sync();}
        const auto it=manifests.constFind(id);
        downloadBtn->setEnabled(it!=manifests.cend()&&!busyNetwork);
        const QString installed=installedResourceVersion();
        const auto games=discoverWutheringInstallations(steamRoots());
        const auto state=selectedInstallState();
        if(state.owned){
            if(state.resourceVersionChanged||state.packageDiffers||state.selectedChannelDiffers){versionStatus->setText("Доступно обновление");channelStatus->setText("Доступна новая версия перевода. Проверьте совместимость и нажмите «Обновить перевод».");}
            else {versionStatus->setText(state.healthy?"Установлен · "+state.channel+" v"+state.translationVersion:(state.repairable?"Нужна проверка Mount":(state.reinstallable?"Требуется повторная установка":"Нарушена целостность")));channelStatus->setText(it==manifests.cend()?state.message+" · проверка обновления доступна вручную.":state.message);}
            return;
        }
        if(it==manifests.cend()){
            versionStatus->setText("Не проверено");
            if(installed.isEmpty()) channelStatus->setText("Выберите канал и проверьте наличие обновления.");
            else channelStatus->setText("Ресурсы игры обнаружены · версия "+installed+". Проверьте канал, чтобы узнать версию перевода.");
            return;
        }
        if(installed.isEmpty()){
            versionStatus->setText(it->version);
            channelStatus->setText("Перевод доступен · версия "+it->version+". Ресурсы игры автоматически не обнаружены.");
            return;
        }
        const QString local=normalizedVersion(installed), remote=normalizedVersion(it->version);
        const auto localParts=QVersionNumber::fromString(local), remoteParts=QVersionNumber::fromString(remote);
        const bool likelyCompatible=localParts.segmentCount()>=2&&remoteParts.segmentCount()>=2&&localParts.segmentAt(0)==remoteParts.segmentAt(0)&&localParts.segmentAt(1)==remoteParts.segmentAt(1);
        versionStatus->setText(it->version+"  ·  игра "+installed);
        channelStatus->setText(likelyCompatible?"Перевод доступен · версия игры совместима по major/minor. Нажмите «Установить перевод» для установки в игру.":"Версии игры и перевода различаются. Пакет можно скачать, применение не подтверждено.");
    }

    QWidget *browseRow(QLineEdit *edit,const QString &button,const std::function<QString()> &picker){
        auto *row=new QWidget;
        auto *layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);layout->setSpacing(6);
        auto *pick=new QPushButton(button);pick->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Preferred);
        edit->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
        layout->addWidget(edit,1);layout->addWidget(pick);
        connect(pick,&QPushButton::clicked,this,[edit,picker]{const auto p=picker();if(!p.isEmpty())edit->setText(p);});
        return row;
    }
    void loadSettings(){pakEdit->setText(settings.value("localizer/pak").toString());sigEdit->setText(settings.value("localizer/sig").toString());targetEdit->setText(settings.value("localizer/target").toString());}
    void saveInstallSettings(){settings.setValue("localizer/pak",pakEdit->text());settings.setValue("localizer/sig",sigEdit->text());settings.setValue("localizer/target",targetEdit->text());settings.sync();}
    void refreshStatus(){
        const auto games=discoverWutheringInstallations(steamRoots());
        QStringList problems;
        if(games.isEmpty())problems<<"не найдена установленная игра в библиотеках Steam";
        if(steamExecutable().isEmpty())problems<<"не найден Steam";
        const QString version=installedResourceVersion();
        statusLabel->setText(problems.isEmpty()?"Игра найдена в Steam · ресурсы "+(version.isEmpty()?QString("не определены"):"v"+version):"Требуется настройка: "+problems.join("; "));
        launchBtn->setText("Играть через Steam");
        launchBtn->setToolTip("Запустить Wuthering Waves через Steam");
        launchBtn->setEnabled(problems.isEmpty());
    }
    bool currentPlan(QList<InstallItem>*plan,QString*error)const{return makePlan(pakEdit->text(),sigEdit->text(),targetEdit->text(),plan,error);}
    void updatePreview(){QList<InstallItem> plan;QString error;if(!currentPlan(&plan,&error)){preview->setPlainText("План установки пока недоступен.\n"+error);installBtn->setEnabled(false);return;}QStringList lines;lines<<"Папка, выбранная вами: " + canonicalOrAbsolute(targetEdit->text());for(const auto &item:plan){const QFileInfo src(item.source);lines<<"\nИСТОЧНИК: "+item.source+"\nРазмер: "+QLocale().formattedDataSize(src.size())+"\nSHA-256: "+QString::fromLatin1(fileHash(item.source).toHex())+"\nНАЗНАЧЕНИЕ: "+item.destination+"\nДействие: "+(item.overwrite?"будет создана резервная копия и файл заменён":"будет создан новый файл");}preview->setPlainText(lines.join("\n"));installBtn->setEnabled(true);}
    void installSelected(){QList<InstallItem> plan;QString error;if(!currentPlan(&plan,&error)){QMessageBox::warning(this,"Нельзя установить",error);updatePreview();return;}updatePreview();const QString confirmedPreview=preview->toPlainText();const auto answer=QMessageBox::question(this,"Подтвердите установку","Будут изменены только указанные ниже файлы внутри выбранной папки:\n\n"+confirmedPreview+"\n\nПродолжить?",QMessageBox::Yes|QMessageBox::No,QMessageBox::No);if(answer!=QMessageBox::Yes)return;
        // Revalidate paths and hashes after the modal prompt to catch changes before writing.
        if(!currentPlan(&plan,&error)){QMessageBox::warning(this,"План изменился",error);updatePreview();return;}
        updatePreview();if(preview->toPlainText()!=confirmedPreview){QMessageBox::warning(this,"Файлы изменились","Пути, размеры, контрольные суммы или состояние назначения изменились после предварительного просмотра. Проверьте план и подтвердите заново.");return;}
        progress->setRange(0,0);progressLabel->setText("Создание резервных копий и установка…");installBtn->setEnabled(false);qApp->processEvents();QString backup,errorMessage;
        if(!installPlan(plan,appDataPath+"/backups/localizer",&backup,&errorMessage)){progress->setRange(0,100);progress->setValue(0);progressLabel->setText("Установка остановлена; см. сообщение об ошибке.");QMessageBox::critical(this,"Установка не завершена",errorMessage);updatePreview();return;}
        progress->setRange(0,100);progress->setValue(100);progressLabel->setText("Файлы записаны и проверены. Резервная копия: "+backup);QMessageBox::information(this,"Установка завершена","Проверка SHA-256 пройдена.\nРезервные копии (если были замены):\n"+backup);updatePreview();refreshDiagnostics();}
    void logLine(const QString &line){QDir().mkpath(QFileInfo(logPath).absolutePath());QFile f(logPath);if(f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text)){QTextStream out(&f);out<<line<<'\n';}}
    void launchGame(){
        {
            const auto installs=discoverWutheringInstallations(steamRoots());
            const QString steam=steamExecutable();
            if(installs.isEmpty()||steam.isEmpty()){QMessageBox::warning(this,"Steam не готов","Не найден Steam с установленной Wuthering Waves (appmanifest_3513350.acf). Проверьте библиотеки Steam.");refreshStatus();return;}
            // Restore the verified ECHO entry if the game update removed it.
            // The repair routine checks ownership, hash and running processes
            // and backs up the original table before changing it.
            if(installs.size()==1){
                const auto state=selectedInstallState();
                if(state.repairable){
                    const auto restored=EchoInstall::repair(appDataPath,state.currentResourceVersion);
                    logLine("Translation repair before launch: "+restored.message);
                    refreshAutoInstallControls();
                    if(!restored.ok){
                        channelStatus->setText("Не удалось восстановить перевод: "+restored.message+" Игра не запущена.");
                        versionStatus->setText("Восстановление не выполнено");
                        return;
                    }
                    channelStatus->setText("Перевод восстановлен. Запускаем игру.");
                    versionStatus->setText("Перевод восстановлен");
                }
            }
            logLine("\n=== "+QDateTime::currentDateTime().toString(Qt::ISODate)+" ===");logLine("Launch mode: Steam-managed");logLine("Steam: "+steam);logLine("AppID: 3513350");logLine("Install: "+installs.first().installDir);logLine("Client: "+installs.first().gameDir);
            const auto launchCheck = inspectSteamLaunchOptions(steamRoots(), WutheringAppId);
            if (!launchCheck.ok) {
                logLine("Launch options warning: " + launchCheck.error);
                channelStatus->setText("Возможно, есть проблема с параметрами запуска Steam. "+launchCheck.error);
            }
            QString error;
            QProcess process;
            process.setProgram(steam);
            process.setArguments(steamLaunchArguments());
            process.setProcessEnvironment(sanitizedSteamEnvironment());
            qint64 pid = 0;
            if (!process.startDetached(&pid)) {
                error = "Не удалось передать запуск Steam. Убедитесь, что клиент Steam запущен.";
                logLine(error);
                QMessageBox::critical(this, "Ошибка запуска", error);
                return;
            }
            statusLabel->setText(launchCheck.ok?"Запрос запуска передан Steam (AppID 3513350).":"Запрос передан Steam. Проверьте предупреждение о параметрах запуска.");
            logLine(QString("Steam launch request submitted (PID: %1).").arg(pid));
            refreshDiagnostics();
            return;
        }
    }
    QString recentLog()const{QFile f(logPath);if(!f.open(QIODevice::ReadOnly|QIODevice::Text))return"(лог запуска пока не создан)";auto s=QString::fromUtf8(f.readAll());if(s.size()>12000)s=s.right(12000);return s;}
    void refreshDiagnostics(){diagnostic->setPlainText("Журнал ECHO Native\nЛог запуска: "+logPath+"\n\n"+recentLog());}

public:
    bool uiChecks(QString *failure=nullptr) const {
        auto fail=[&](const QString &why){if(failure)*failure=why;return false;};
        if(!findChild<QComboBox*>("channelSelector"))return fail("channel selector missing");
        auto *primary=findChild<QPushButton*>("primaryButton");
        if(!primary)return fail("primary install action missing");
        if(busyNetwork && primary->isEnabled())return fail("primary install action should be disabled during network requests");
        if(!findChild<QLabel*>("installAvailability"))return fail("install prerequisite hint missing");
        if(!findChild<QProgressBar*>("downloadProgress"))return fail("progress bar missing");
        if(!findChild<QPushButton*>("secondaryButton"))return fail("secondary actions missing");
        auto *pages=findChild<QStackedWidget*>("pageStack");if(!pages||pages->count()!=2)return fail("two-page navigation missing");bool translationNav=false,settingsNav=false;for(auto *b:findChildren<QPushButton*>("navButton")){translationNav|=b->text()=="Перевод";settingsNav|=b->text()=="Настройки";}if(!translationNav||!settingsNav)return fail("top navigation labels missing");
        auto *manualDisclosure=findChild<QToolButton*>("manualDisclosure");auto *manualBody=findChild<QWidget*>("manualBody");if(!manualDisclosure||manualDisclosure->isChecked()||manualDisclosure->arrowType()!=Qt::RightArrow)return fail("manual installation disclosure is not initially closed");if(!manualBody||!manualBody->isHidden())return fail("manual installation contents are not hidden");
        auto *transfer=findChild<QWidget*>("transferProgress");if(!transfer||!transfer->isHidden())return fail("transfer progress is visible while idle");bool cancelContained=false;for(auto *button:findChildren<QPushButton*>())if(button->text()=="Отменить"&&button->parentWidget()==transfer)cancelContained=true;if(!cancelContained)return fail("cancel action is not inside the hidden transfer area");
        const auto widgets=findChildren<QWidget*>();
        for(const auto *widget:widgets){
            QString text;
            if(auto *label=qobject_cast<const QLabel*>(widget))text=label->text();
            else if(auto *button=qobject_cast<const QPushButton*>(widget))text=button->text();
            else if(auto *box=qobject_cast<const QGroupBox*>(widget))text=box->title();
            if(text.contains("voice",Qt::CaseInsensitive)||text.contains("озвуч",Qt::CaseInsensitive)||text.contains("голос",Qt::CaseInsensitive))return fail("non-text translation control is visible");
        }
        bool hasTitle=false;for(const auto *label:findChildren<QLabel*>("pageTitle"))if(label->text()=="Текстовый перевод")hasTitle=true;if(!hasTitle)return fail("main purpose label missing");
        auto *homePlay=pages->widget(0)->findChild<QPushButton*>("playGameButton");
        if(!homePlay||homePlay!=launchBtn||homePlay->text()!="Играть через Steam")return fail("Play button is missing from the main page");
        if(pages->widget(1)->findChild<QPushButton*>("playGameButton"))return fail("Steam launch button is duplicated in settings");
        const auto *wrapperField=pages->widget(1)->findChild<QLineEdit*>("steamWrapperCommand");
        const auto *wrapperCopy=pages->widget(1)->findChild<QPushButton*>("copySteamWrapperCommand");
        if(!wrapperField||!wrapperField->isReadOnly()||!wrapperCopy||
           (wrapperCopy->isEnabled()&&!wrapperField->text().endsWith(" --steam-wrap %command%")))return fail("Steam wrapper setup is missing or invalid");
        if(pages->widget(0)->findChild<QPushButton*>("textButton") == downloadBtn ||
           pages->widget(0)->findChild<QWidget*>("manualSection"))return fail("advanced tools leaked onto the main page");
        if(pages->widget(1)->findChild<QPushButton*>("textButton") != downloadBtn ||
           pages->widget(1)->findChild<QWidget*>("manualSection") != manualDisclosure->parentWidget())
            return fail("advanced tools are missing from Settings");
        int playCount=0;for(auto *button:findChildren<QPushButton*>())if(button->objectName()=="playGameButton")++playCount;
        if(playCount!=1)return fail("Expected exactly one Steam play button");
        bool hasCheck=false,hasDownload=false,hasInstall=false,hasVerify=false;for(auto *button:findChildren<QPushButton*>()){hasCheck|=button->text()=="Проверить обновления";hasDownload|=button->text()=="Скачать текст отдельно";hasInstall|=button->text().contains("Установить")||button->text().contains("Обновить")||button->text().contains("Сменить");hasVerify|=button->text()=="Проверить файлы";}if(!hasCheck||!hasDownload||!hasInstall||!hasVerify)return fail("translation management action labels missing");
        if(homePlay->minimumHeight()<48)return fail("main-page Play action is not prominent");
        if(homePlay->accessibleName().isEmpty()||primary->accessibleName().isEmpty())return fail("primary actions require accessible names");
        for(auto *b:pages->widget(0)->findChildren<QPushButton*>("channelCard"))if(b->text().isEmpty())return fail("channel card has no label");
        if(channelBox->count()<1||channelBox->itemText(0).contains("Перевод",Qt::CaseInsensitive))return fail("channel selector repeats an inaccurate description");
        return true;
    }
    void applyScreenshotState(const QString &state) {
        if(state=="settings") { for(auto *button:findChildren<QPushButton*>("navButton")) if(button->text()=="Настройки") button->click(); return; }
        if(state=="healthy") { statusLabel->setText("Игра найдена · ресурсы 3.7.0"); channelStatus->setText("Русский перевод установлен и готов к игре."); versionStatus->setText("OVERLINK · v3.7"); autoInstallBtn->setText("Перевод установлен"); autoInstallBtn->setEnabled(false); autoInstallBtn->hide(); uninstallBtn->show(); }
        else if(state=="repair") { statusLabel->setText("Игра найдена · ресурсы 3.7.0"); channelStatus->setText("Перевод требует восстановления. Файлы перевода сохранены."); versionStatus->setText("Нужно восстановить"); autoInstallBtn->show(); autoInstallBtn->setText("Восстановить перевод"); autoInstallBtn->setEnabled(true); }
        else if(state=="missing") { statusLabel->setText("Игра найдена · ресурсы 3.7.0"); channelStatus->setText("Выберите перевод, чтобы установить его в игру."); versionStatus->setText("Не установлен"); autoInstallBtn->show(); autoInstallBtn->setText("Установить перевод"); autoInstallBtn->setEnabled(true); }
    }
    void smokeChecks(){if(!guessGameExe("/not/a/game").isEmpty())qFatal("smoke: unexpected game EXE discovery");if(!QFileInfo(settingsPath).isAbsolute()||!settingsPath.contains("ECHO Native"))qFatal("smoke: settings path is not app scoped");if(installBtn->isEnabled())qFatal("smoke: installer enabled without explicit sources");QString error;if(!uiChecks(&error))qFatal("smoke: %s",qPrintable(error));fprintf(stdout,"PASS: Qt UI construction, Steam launch, collapsed manual install, app-scoped settings\n");}
private:
};

bool writeBytes(const QString &path, const QByteArray &bytes) {
    QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

int runInstallerTests() {
    QTemporaryDir temp;
    if (!temp.isValid()) { fprintf(stderr,"FAIL: temporary directory unavailable\n"); return 1; }
    const QString root=temp.path(), src= root+"/source", target=root+"/target", backups=root+"/backups";
    QDir().mkpath(src); QDir().mkpath(target);
    const QString pak=src+"/rus.pak", sig=src+"/rus.sig", outPak=target+"/rus.pak", outSig=target+"/rus.sig";
    if (!writeBytes(pak,"mock-pak-content") || !writeBytes(sig,"mock-signature")) { fprintf(stderr,"FAIL: could not make mock files\n"); return 1; }
    QList<InstallItem> plan; QString error, backup;
    if (!makePlan(pak,sig,target,&plan,&error) || plan.size()!=2 || plan[0].overwrite) { fprintf(stderr,"FAIL: valid preview rejected: %s\n",qPrintable(error)); return 1; }
    if (!installPlan(plan,backups,&backup,&error) || QFile(outPak).size()!=16 || QFile(outSig).size()!=14) { fprintf(stderr,"FAIL: initial install: %s\n",qPrintable(error)); return 1; }
    if (!writeBytes(pak,"new-pak") || !writeBytes(outPak,"old-pak") || !writeBytes(outSig,"old-sig")) { fprintf(stderr,"FAIL: could not prepare overwrite case\n"); return 1; }
    if (!makePlan(pak,sig,target,&plan,&error) || !plan[0].overwrite || !plan[1].overwrite) { fprintf(stderr,"FAIL: overwrite preview failed: %s\n",qPrintable(error)); return 1; }
    if (!installPlan(plan,backups,&backup,&error) || QFile(outPak).size()!=7 || QFile(outSig).size()!=14) { fprintf(stderr,"FAIL: overwrite install: %s\n",qPrintable(error)); return 1; }
    QFile oldPak(backup+"/rus.pak"), oldSig(backup+"/rus.sig");
    if (!oldPak.open(QIODevice::ReadOnly) || oldPak.readAll()!="old-pak" || !oldSig.open(QIODevice::ReadOnly) || oldSig.readAll()!="old-sig") { fprintf(stderr,"FAIL: overwrite backups do not preserve originals\n"); return 1; }
    if (makePlan(pak,src+"/other.sig",target,&plan,&error)) { fprintf(stderr,"FAIL: mismatched pair accepted\n"); return 1; }
    const QString empty=src+"/empty.pak"; if (!writeBytes(empty,"") || makePlan(empty,{},target,&plan,&error)) { fprintf(stderr,"FAIL: empty source accepted\n"); return 1; }
    const QString linkTarget=root+"/target-link"; if (!QFile::link(target,linkTarget) || makePlan(pak,{},linkTarget,&plan,&error)) { fprintf(stderr,"FAIL: symlink target accepted\n"); return 1; }
    if (makePlan(outPak,{},target,&plan,&error)) { fprintf(stderr,"FAIL: source equal to destination accepted\n"); return 1; }
    // Force the second copy in a two-file transaction to fail after the first was replaced.
    if (!writeBytes(outPak,"rollback-before") || !writeBytes(outSig,"rollback-sig")) { fprintf(stderr,"FAIL: rollback fixture setup failed\n"); return 1; }
    if (!makePlan(pak,sig,target,&plan,&error)) { fprintf(stderr,"FAIL: rollback plan rejected\n"); return 1; }
    QFile::remove(sig);
    if (installPlan(plan,backups,&backup,&error)) { fprintf(stderr,"FAIL: missing source unexpectedly installed\n"); return 1; }
    QFile restored(outPak); if (!restored.open(QIODevice::ReadOnly) || restored.readAll()!="rollback-before") { fprintf(stderr,"FAIL: failed transaction did not roll back first file\n"); return 1; }
    fprintf(stdout,"PASS: mock PAK/SIG install, overwrite backups, pair/empty/source-equals-target/symlink rejection, failed-transaction rollback\n");
    return 0;
}

int runManifestTests() {
    const QString base=QStringLiteral(ECHO_SOURCE_DIR)+"/research/manifests/";
    QFile indexFile(base+"wwmain-main2.json");if(!indexFile.open(QIODevice::ReadOnly)){fprintf(stderr,"FAIL: cannot read channel index fixture\n");return 1;}
    QList<TranslationChannel> channels;QString error;
    if(!parseChannelIndex(indexFile.readAll(),&channels,&error)||channels.size()!=3){fprintf(stderr,"FAIL: channel index parse: %s\n",qPrintable(error));return 1;}
    QSet<QString> ids;for(const auto &c:channels)ids.insert(c.id);
    if(ids!=QSet<QString>{"over","aniwie","qwikki"}){fprintf(stderr,"FAIL: channel IDs mismatch\n");return 1;}
    const QStringList expected{"over","aniwie","qwikki"};
    for(int i=0;i<expected.size();++i){QFile f(base+"manifest_"+expected[i]+".json");if(!f.open(QIODevice::ReadOnly)){fprintf(stderr,"FAIL: cannot read channel manifest fixture\n");return 1;}TranslationManifest m;if(!parseTranslationManifest(f.readAll(),expected[i],&m,&error)||m.version!="3.7"){fprintf(stderr,"FAIL: channel manifest %s: %s\n",qPrintable(expected[i]),qPrintable(error));return 1;}}
    const QByteArray malicious=R"({"TranslationChannels":[{"Id":"over","Name":"bad","ManifestUrl":"https://evil.example/manifest_over.json","Enabled":true}]})";
    if(parseChannelIndex(malicious,&channels,&error)){fprintf(stderr,"FAIL: untrusted channel URL accepted\n");return 1;}
    QFile over(base+"manifest_over.json");if(!over.open(QIODevice::ReadOnly)){fprintf(stderr,"FAIL: cannot read OVERLINK fixture\n");return 1;}QByteArray badManifest=over.readAll();badManifest.replace("https://gitlab.com/ECHOLaunch/updates/-/raw/main/over/text/","https://evil.example/");TranslationManifest bad;
    if(parseTranslationManifest(badManifest,"over",&bad,&error)){fprintf(stderr,"FAIL: untrusted package URL accepted\n");return 1;}
    if(trustedGitlabRawUrl(QUrl("http://gitlab.com/ECHOLaunch/updates/-/raw/main/a"))||trustedGitlabRawUrl(QUrl("https://gitlab.com.evil.example/ECHOLaunch/updates/-/raw/main/a"))){fprintf(stderr,"FAIL: URL allowlist accepted invalid origin\n");return 1;}
    fprintf(stdout,"PASS: all three official text manifest fixtures, JSON/schema/URL allowlists; XXH64-verified package transfer is enabled\n");return 0;
}

int runDownloadTests() {
    QString error;
    if (xxh64File(QStringLiteral(ECHO_SOURCE_DIR)+"/research/packages/pakchunk0-WindowsNoEditor_10000_P.pak",&error)!="60f5ad3fd24f0244") {
        fprintf(stderr,"FAIL: real OVERLINK PAK XXH64 did not match manifest: %s\n",qPrintable(error)); return 1;
    }
    QTemporaryDir temp; if(!temp.isValid()){fprintf(stderr,"FAIL: download test temporary directory unavailable\n");return 1;}
    const QString part=temp.path()+"/sample.part", final=temp.path()+"/verified.pak", wrong=temp.path()+"/wrong.part", wrongFinal=temp.path()+"/wrong.pak";
    if(!writeBytes(part,"verified mock bytes")){fprintf(stderr,"FAIL: valid package fixture setup\n");return 1;}
    const QString known=xxh64File(part,&error);
    if(known.isEmpty()||!verifyAndPromotePackage(part,final,known,&error)||!QFileInfo(final).isFile()||QFileInfo::exists(part)){
        fprintf(stderr,"FAIL: valid package was not atomically promoted: %s\n",qPrintable(error));return 1;
    }
    if(!writeBytes(wrong,"tampered mock bytes")){fprintf(stderr,"FAIL: mismatch fixture setup\n");return 1;}
    if(verifyAndPromotePackage(wrong,wrongFinal,known,&error)||!QFileInfo(wrong).isFile()||QFileInfo::exists(wrongFinal)){
        fprintf(stderr,"FAIL: mismatched package was accepted or modified\n");return 1;
    }
    const QString empty=temp.path()+"/empty";if(!writeBytes(empty,"")||verifyAndPromotePackage(empty,temp.path()+"/empty.pak",known,&error)){
        fprintf(stderr,"FAIL: empty file was accepted as a package\n");return 1;
    }
    fprintf(stdout,"PASS: real 70,949,699-byte OVERLINK PAK XXH64=60f5ad3fd24f0244; verified atomic promotion, mismatch rejection, and empty package rejection\n");return 0;
}

int runLaunchTests() {
    if(EchoSteamOptions::runSteamOptionsTests()!=0 || EchoSteam::runSteamConfigTests()!=0)return 1;
    const QByteArray opaque=QByteArray::fromHex("ff80");
    QList<QByteArray> clean;
    const QList<QByteArray> ordinary{"proton",opaque,"launcher.exe"};
    if(takeDirectIntent(ordinary,&clean)||clean!=ordinary){fprintf(stderr,"FAIL: ordinary Steam launch changed\n");return 1;}
    auto requested=ordinary;requested<<"--echo-native-direct";
    if(!takeDirectIntent(requested,&clean)||clean!=ordinary){fprintf(stderr,"FAIL: ECHO launch intent not consumed\n");return 1;}
    requested<<"--echo-native-direct";
    if(takeDirectIntent(requested,&clean)||clean!=ordinary){fprintf(stderr,"FAIL: duplicate launch intent must fall back\n");return 1;}
    const QString trickyPath="/tmp/ECHO user's $(not-a-command).AppImage";
    const QString wrapper=steamWrapperCommand(trickyPath);
    QProcess shell;
    QString verify=wrapper;verify.replace("%command%", "sentinel");
    shell.start("/bin/sh", {"-c", "set -- "+verify+"; printf '%s\\n' \"$@\""});
    if(!shell.waitForFinished() || shell.exitCode()!=0 ||
       shell.readAllStandardOutput()!=QFile::encodeName(trickyPath)+"\n--steam-wrap\nsentinel\n" ||
       !steamWrapperCommand("/bad\npath").isEmpty()){
        fprintf(stderr,"FAIL: Steam wrapper command quoting\n");return 1;
    }
    QTemporaryDir temp;
    if (!temp.isValid()) { fprintf(stderr,"FAIL: launch test temporary directory unavailable\n"); return 1; }
    const QString root=temp.path(), library=root+"/library", apps=library+"/steamapps", game=apps+"/common/Wuthering Waves";
    const QString oldMount=game+"/Client/Saved/Resources/3.7.0/Mount";
    if(!QDir().mkpath(oldMount))return 1;
    if (!writeBytes(apps+"/appmanifest_3513350.acf", "\"AppState\" { \"appid\" \"3513350\" \"installdir\" \"Wuthering Waves\" }\n") ||
        !writeBytes(game+"/launcher.exe","launcher") || !writeBytes(oldMount+"/MountResource.txt","::Mount::\n::Del::\n")) {
        fprintf(stderr,"FAIL: Steam manifest fixture setup failed\n"); return 1;
    }
    const auto detected=discoverWutheringInstallations({library});
    if (detected.size()!=1 || detected.first().gameDir!=QFileInfo(game).canonicalFilePath()) { fprintf(stderr,"FAIL: Steam appmanifest discovery failed\n"); return 1; }
    // Steam release may ship Wuthering Waves.exe without launcher.exe.
    QFile::remove(game+"/launcher.exe");
    if (!writeBytes(game+"/Wuthering Waves.exe","steam entry") ||
        discoverWutheringInstallations({library}).size()!=1) {
        fprintf(stderr,"FAIL: official Steam EXE layout not detected\n"); return 1;
    }
    if (!writeBytes(game+"/launcher.exe","launcher")) return 1;
    // Version 3.8 can leave stale resources in the Steam launcher directory.
    const QString nested=game+"/Wuthering Waves Game", nestedMount=nested+"/Client/Saved/Resources/3.8.0/Mount";
    const QString nestedExe=nested+"/Client/Binaries/Win64/Client-Win64-Shipping.exe";
    if(!QDir().mkpath(nestedMount)||!QDir().mkpath(QFileInfo(nestedExe).absolutePath())||
       !writeBytes(nested+"/Wuthering Waves.exe","client")||!writeBytes(nestedExe,"client")||
       !writeBytes(nestedMount+"/MountResource.txt","::Mount::\n::Del::\n"))return 1;
    const auto resolved=discoverWutheringInstallations({library,library});
    if(resolved.size()!=1||resolved.first().installDir!=QFileInfo(game).canonicalFilePath()||
       resolved.first().gameDir!=QFileInfo(nested).canonicalFilePath()||
       EchoInstall::detectResourceVersion(resolved.first().gameDir)!="3.8.0"){
        fprintf(stderr,"FAIL: nested client not selected over stale launcher resources\n");return 1;
    }
    if(!writeBytes(nested+"/launcherDownloadConfig.json",
       "{\"bundles\":{\"HD\":{\"version\":\"3.8.0\",\"resourcePacks\":[\"common\",\"hd\"]}}}"))return 1;
    const QStringList steamCommand{"/fake/proton","waitforexitandrun",game+"/launcher.exe","-SkipSplash"};
    QStringList redirected;QString why;
    if(!directSteamArguments(steamCommand,resolved,&redirected,&why)||
       redirected!=QStringList({"/fake/proton","waitforexitandrun",nestedExe,"-SkipSplash","-krqlv=hd"})){
        fprintf(stderr,"FAIL: Steam direct command or quality selection: %s\n",qPrintable(why));return 1;
    }
    QStringList conflicting=steamCommand;conflicting<<"-krqlv=uhd";
    if(directSteamArguments(conflicting,resolved,&redirected,&why)||redirected!=conflicting){
        fprintf(stderr,"FAIL: conflicting quality must fall back to the official launcher\n");return 1;
    }
    if(!writeBytes(nested+"/launcherDownloadConfig.json",
       "{\"bundles\":{\"HD\":{\"version\":\"3.8.0\"},\"UHD\":{\"version\":\"3.8.0\"}}}")||
       directSteamArguments(steamCommand,resolved,&redirected,&why)||redirected!=steamCommand){
        fprintf(stderr,"FAIL: ambiguous quality must fall back to the official launcher\n");return 1;
    }
    const QString competing=game+"/Other copy", competingMount=competing+"/Client/Saved/Resources/3.8.0/Mount";
    const QString competingExe=competing+"/Client/Binaries/Win64/Client-Win64-Shipping.exe";
    if(!QDir().mkpath(competingMount)||!QDir().mkpath(QFileInfo(competingExe).absolutePath())||
       !writeBytes(competing+"/Wuthering Waves.exe","client")||!writeBytes(competingExe,"client")||
       !writeBytes(competingMount+"/MountResource.txt","::Mount::\n::Del::\n"))return 1;
    const auto ambiguous=discoverWutheringInstallations({library});
    if(ambiguous.size()!=1||!ambiguous.first().gameDir.isEmpty()||ambiguous.first().discoveryError.isEmpty()){
        fprintf(stderr,"FAIL: ambiguous clients must not be selected for modification\n");return 1;
    }
    if(!QDir(competing).removeRecursively())return 1;
    const QString shipping=game+"/Client-Win64-Shipping.exe", proton=root+"/proton", compat=root+"/compatdata";
    if (!writeBytes(shipping,"shipping") || !writeBytes(proton,"#!/bin/sh\nexit 0\n") || !QFile::setPermissions(proton,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner)) { fprintf(stderr,"FAIL: launch fixture setup failed\n"); return 1; }
    QProcess process; QString error;
    if (launchStandaloneProton(&process,proton,shipping,compat,library,&error) || !error.contains("Client-Win64-Shipping.exe") || QFileInfo::exists(compat)) { fprintf(stderr,"FAIL: internal shipping EXE was not safely refused\n"); return 1; }
    if (!isSupportedStandaloneLauncher(game+"/launcher.exe") || isSupportedStandaloneLauncher(shipping)) { fprintf(stderr,"FAIL: standalone launcher validation failed\n"); return 1; }
    if (steamLaunchArguments()!=QStringList({"-applaunch","3513350","--echo-native-direct"})) { fprintf(stderr,"FAIL: Steam launch arguments mismatch\n"); return 1; }
    if (!launchStandaloneProton(&process,proton,game+"/launcher.exe",compat,library,&error) || process.program()!=proton || process.arguments()!=QStringList({"run",game+"/launcher.exe"}) || process.state()!=QProcess::NotRunning) { fprintf(stderr,"FAIL: standalone command preparation failed\n"); return 1; }
    // Only literal, direct wrapper commands can cause a preflight block.
    if(simpleWrapperFromLaunchOptions("~/proton-launch %command%","/fake/home")!="/fake/home/proton-launch"||
       simpleWrapperFromLaunchOptions("\"/path with spaces/wrapper\" %command%","/fake/home")!="/path with spaces/wrapper"){
        fprintf(stderr,"FAIL: literal wrapper parsing\n");return 1;
    }
    for(const QString &complex:QStringList{"%command%","\"~/wrapper\" %command%",
        "VAR=1 /missing/wrapper %command%","/missing/wrapper --arg %command%",
        "/missing/wrapper %command% || echo fallback","/missing/wrapper; echo x %command%",
        "$HOME/wrapper %command%","gamemoderun /missing/wrapper %command%"}){
        if(!simpleWrapperFromLaunchOptions(complex,"/fake/home").isEmpty()){
            fprintf(stderr,"FAIL: shell command must not be interpreted as a simple path\n");return 1;
        }
    }
    const QString mockVdf="\"3513350\" { \"LaunchOptions\" \"/nonexistent/wrapper %command%\" }";
    if(extractVdfAppLaunchOptions(mockVdf,"3513350")!="/nonexistent/wrapper %command%"||
       !extractVdfAppLaunchOptions(mockVdf+mockVdf,"3513350").isEmpty()){
        fprintf(stderr,"FAIL: LaunchOptions VDF parsing or ambiguity\n");return 1;
    }
    const QString testSteam=root+"/steam-test",testHome=root+"/home-test";
    const QString configOne=testSteam+"/userdata/1/config/localconfig.vdf";
    const QString configTwo=testSteam+"/userdata/2/config/localconfig.vdf";
    const QString loginFile=testSteam+"/config/loginusers.vdf";
    const QString trash=testHome+"/.local/share/Trash/files/proton-launch";
    if(!QDir().mkpath(QFileInfo(configOne).absolutePath())||
       !QDir().mkpath(QFileInfo(configTwo).absolutePath())||
       !QDir().mkpath(QFileInfo(loginFile).absolutePath())||
       !QDir().mkpath(QFileInfo(trash).absolutePath())||
       !writeBytes(trash,"old deleted script")||
       !writeBytes(configOne,"\"3513350\" { \"LaunchOptions\" \"~/proton-launch %command%\" }")){
        fprintf(stderr,"FAIL: Steam account test setup\n");return 1;
    }
    const auto missing=inspectSteamLaunchOptions({testSteam,testSteam},WutheringAppId,testHome);
    if(missing.ok||missing.wrapperPath!=testHome+"/proton-launch"||QFileInfo::exists(testHome+"/proton-launch")){
        fprintf(stderr,"FAIL: missing wrapper must be reported without restoring the trash file\n");return 1;
    }
    if(!writeBytes(configTwo,"\"3513350\" { \"LaunchOptions\" \"/missing/other %command%\" }")||
       !inspectSteamLaunchOptions({testSteam},WutheringAppId,testHome).ok){
        fprintf(stderr,"FAIL: multiple profiles without login information must not block\n");return 1;
    }
    const QByteArray loginOne="\"users\" { \"76561197960265729\" { \"AccountName\" \"first\" } }";
    if(!writeBytes(loginFile,loginOne)||!writeBytes(configOne,"\"3513350\" { }")||
       !inspectSteamLaunchOptions({testSteam},WutheringAppId,testHome).ok){
        fprintf(stderr,"FAIL: empty selected profile must not inspect another profile\n");return 1;
    }
    const QByteArray loginRecentOne="\"users\" { \"76561197960265729\" { \"MostRecent\" \"1\" } \"76561197960265730\" { \"MostRecent\" \"0\" } }";
    if(!writeBytes(configOne,"\"3513350\" { \"LaunchOptions\" \"~/proton-launch %command%\" }")||
       !writeBytes(loginFile,loginRecentOne)||
       inspectSteamLaunchOptions({testSteam},WutheringAppId,testHome).ok){
        fprintf(stderr,"FAIL: active profile missing wrapper must be reported\n");return 1;
    }
    const QByteArray loginAmbiguous="\"users\" { \"76561197960265729\" { } \"76561197960265730\" { } }";
    if(!writeBytes(loginFile,loginAmbiguous)||
       !inspectSteamLaunchOptions({testSteam},WutheringAppId,testHome).ok||
       QFileInfo::exists(testHome+"/proton-launch")){
        fprintf(stderr,"FAIL: ambiguous profile must not block or restore files\n");return 1;
    }
    QProcessEnvironment desktop;
    desktop.insert("LD_PRELOAD","/tmp/test-overlay.so");
    desktop.insert("OTHER_SETTING","preserved");
    const auto desktopEnv=sanitizedSteamEnvironment(desktop);
    if(desktopEnv.value("LD_PRELOAD")!="/tmp/test-overlay.so"||desktopEnv.value("OTHER_SETTING")!="preserved"){
        fprintf(stderr,"FAIL: desktop overlay environment was not preserved\n");return 1;
    }
    for(const QString &marker:QStringList{"SteamAppId","SteamGameId","STEAM_COMPAT_APP_ID","SteamClientLaunch"}){
        auto steamEnv=desktop;steamEnv.insert(marker,"1");
        const auto cleaned=sanitizedSteamEnvironment(steamEnv);
        if(cleaned.contains(marker)||cleaned.contains("LD_PRELOAD")||cleaned.value("OTHER_SETTING")!="preserved"){
            fprintf(stderr,"FAIL: Steam launch environment was not cleaned correctly\n");return 1;
        }
    }
    fprintf(stdout,"PASS: Steam discovery, nested client, stale resources, ambiguous clients, direct Proton command and quality fallback, LaunchOptions validation; no game process started\n");
    return 0;
}
}

int main(int argc,char **argv){
    if(argc>1&&QByteArray(argv[1])=="--steam-wrap")return runSteamWrapper(argc,argv);
    const bool smoke=argc>1&&QString::fromLocal8Bit(argv[1])=="--smoke-test";
    const bool resourceCheck=argc>1&&QString::fromLocal8Bit(argv[1])=="--resource-check";
    const bool installerTest=argc>1&&QString::fromLocal8Bit(argv[1])=="--installer-test";
    const bool launchTest=argc>1&&QString::fromLocal8Bit(argv[1])=="--launch-test";
    const bool manifestTest=argc>1&&QString::fromLocal8Bit(argv[1])=="--manifest-test";
    const bool downloadTest=argc>1&&QString::fromLocal8Bit(argv[1])=="--download-test";
    const bool uiTest=argc>1&&QString::fromLocal8Bit(argv[1])=="--ui-test";
    const bool controllerTest=argc>1&&QString::fromLocal8Bit(argv[1])=="--controller-test";
    const bool autoInstallTest=argc>1&&QString::fromLocal8Bit(argv[1])=="--auto-install-test";
    const bool screenshot=argc>1&&QString::fromLocal8Bit(argv[1])=="--ui-screenshot";
    const bool dialogScreenshot=argc>1&&QString::fromLocal8Bit(argv[1])=="--dialog-screenshot";
    const QString screenshotPath=screenshot&&argc>2?QString::fromLocal8Bit(argv[2]):QString();
    const QString screenshotState=screenshot&&argc>3?QString::fromLocal8Bit(argv[3]):QStringLiteral("missing");
    QSize screenshotSize(940,680);
    if(screenshot&&argc>4){const auto parts=QString::fromLocal8Bit(argv[4]).split('x');if(parts.size()==2)screenshotSize=QSize(parts.at(0).toInt(),parts.at(1).toInt());else screenshotSize=QSize();}
    QTemporaryDir isolatedUiState;
    if (smoke || uiTest || screenshot || controllerTest || dialogScreenshot) {
        if (!isolatedUiState.isValid()) { fprintf(stderr, "FAIL: isolated UI state unavailable\n"); return 2; }
        const QByteArray statePath = QFile::encodeName(isolatedUiState.path());
        qputenv("XDG_CONFIG_HOME", statePath);
        qputenv("XDG_DATA_HOME", statePath);
        qputenv("XDG_CACHE_HOME", statePath);
    }
    QApplication app(argc,argv);app.setOrganizationName("ECHO Native");app.setApplicationName("ECHO Native");app.setStyle("Fusion");
    DialogLocalizer dialogLocalizer;app.installEventFilter(&dialogLocalizer);
    app.setStyleSheet(R"QSS(
        QWidget { color: #edf0f2; font-family: "Noto Sans", "DejaVu Sans", sans-serif; font-size: 11pt; }
        QWidget#rootWindow, QStackedWidget, QScrollArea, #homePage, #settingsPage { background: #101318; border: 0; }
        QMessageBox, QFileDialog { background: #181d22; color: #edf0f2; }
        QMessageBox QLabel, QFileDialog QLabel { color: #edf0f2; }
        QMessageBox QPushButton { min-width: 90px; min-height: 32px; }
        QMessageBox QPushButton:focus { border: 3px solid #ffffff; background: #55452f; color: #ffffff; }
        QFileDialog QAbstractItemView { background: #171a1c; color: #edf0f2; selection-background-color: #806a4c; }
        QWidget#topBar { background: #14181d; border-bottom: 1px solid #282f35; }
        QLabel, QCheckBox { background: transparent; }
        QLabel#controllerHint { background: #1e252a; color: #d8b987; padding: 5px 14px; font-size: 9pt; }
        QLabel#sideSubtitle { color: #8b959d; font-size: 9pt; letter-spacing: 1px; }
        QPushButton#navButton { text-align: center; border: 1px solid transparent; background: transparent; color: #adb5bb; padding: 9px 12px; border-radius: 8px; }
        QPushButton#navButton:checked { color: #f0d5a7; background: #252a2d; border-color: #373e43; }
        QPushButton#navButton:hover:!checked { color: #edf0f2; background: #1e2328; }
        QScrollBar:horizontal { height: 8px; }
        #brandName { color: #d8b987; font-size: 17pt; font-weight: 700; letter-spacing: 2px; }
        #pageTitle { color: #f3f4f5; font-size: 20pt; font-weight: 600; }
        #sectionTitle { color: #f0f1f2; font-size: 14pt; font-weight: 600; }
        #secondaryText { color: #aab2b8; }
        #gameCard { background: #1a1f24; border: 1px solid #303840; border-radius: 13px; }
        #gameTitle { color: #e5c99d; font-size: 10pt; font-weight: 650; letter-spacing: 1px; }
        #statusDot { color: #83b89b; font-size: 12pt; }
        #gameStatus { color: #d7dde1; }
        #surfaceCard { background: #1a1f24; border: 1px solid #303840; border-radius: 13px; }
        QLineEdit, QComboBox, QPlainTextEdit { background: #171a1c; color: #e9ecef; border: 1px solid #353b40; border-radius: 8px; padding: 9px 11px; selection-background-color: #806a4c; }
        QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus { border: 1px solid #c5a477; }
        QComboBox::drop-down { border: 0; width: 28px; }
        QComboBox QAbstractItemView { background: #202427; color: #e9ecef; selection-background-color: #38332b; border: 1px solid #353b40; }
        QPushButton { background: #252a2d; color: #edf0f2; border: 1px solid #3a4248; border-radius: 9px; padding: 10px 14px; min-height: 22px; }
        QPushButton:hover { background: #30373b; border-color: #68747b; }
        QPushButton:focus { border: 2px solid #d4b17f; }
        QLineEdit:focus, QComboBox:focus, QToolButton:focus { border: 2px solid #d4b17f; }
        QPushButton:disabled { color: #72797e; background: #202426; border-color: #2b3033; }
        QPushButton#playGameButton { background: #c5a477; color: #171615; border: 1px solid #c5a477; font-size: 12pt; font-weight: 700; padding: 12px 24px; }
        QPushButton#playGameButton:hover { background: #d2b48a; border-color: #e0c398; }
        QPushButton#playGameButton:disabled { background: #514b40; color: #aaa397; border-color: #514b40; }
        QPushButton#primaryButton { background: #282a29; color: #f1d5a9; border: 1px solid #9e8059; font-weight: 650; padding: 12px 18px; }
        QPushButton#primaryButton:hover { background: #35332e; border-color: #c5a477; }
        QPushButton#primaryButton:disabled { background: #222423; color: #85827b; border-color: #47433c; }
        QPushButton#secondaryButton, QPushButton#checkUpdatesButton { background: #252a2d; }
        QPushButton#channelCard { background: #171c21; border: 1px solid #39434a; color: #cbd2d7; font-weight: 600; }
        QPushButton#channelCard:hover { border-color: #71614b; color: #eee5d5; }
        QPushButton#channelCard:checked { background: #2b2925; border: 1px solid #b49365; color: #efd7af; }
        QPushButton#textButton { border: 0; color: #c5a477; background: transparent; padding: 4px 8px; }
        QToolButton#sectionDisclosure { border: 0; color: #e2e6e8; background: transparent; padding: 6px 0; font-weight: 650; text-align: left; }
        QToolButton#sectionDisclosure:focus { border: 1px solid #d4b17f; border-radius: 6px; }
        QToolButton#manualDisclosure { border: 0; color: #b9c0c4; background: transparent; padding: 8px 2px; font-weight: 600; text-align: left; }
        QToolButton#manualDisclosure:hover { color: #e9ecef; }
        QToolButton#manualDisclosure:focus { border: 1px solid #d4b17f; border-radius: 6px; }
        /* Gamepad and keyboard focus must stay visible on selected and special buttons. */
        QPushButton#navButton:focus, QPushButton#channelCard:focus,
        QPushButton#channelCard:checked:focus, QPushButton#playGameButton:focus,
        QPushButton#primaryButton:focus, QPushButton#secondaryButton:focus,
        QPushButton#checkUpdatesButton:focus, QPushButton#textButton:focus,
        QToolButton#sectionDisclosure:focus, QToolButton#manualDisclosure:focus {
            border: 3px solid #ffffff; background: #55452f; color: #ffffff;
        }
        #progressLabel, #versionValue { color: #e4d0ad; font-weight: 600; }
        #inlineStatus { color: #d5dbe0; }
        QProgressBar { background: #272d31; border: 0; border-radius: 4px; }
        QProgressBar::chunk { background: #c5a477; border-radius: 4px; }
        QGroupBox { color: #d9dddf; border: 1px solid #30363a; border-radius: 10px; margin-top: 12px; padding-top: 8px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 5px; }
        QGroupBox::indicator { width: 14px; height: 14px; }
        QFormLayout QLabel { color: #aeb5ba; }
        QScrollBar:vertical { background: #111315; width: 10px; margin: 2px; }
        QScrollBar::handle:vertical { background: #3a4146; min-height: 30px; border-radius: 4px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )QSS");
    if(dialogScreenshot){
        if(argc<3){fprintf(stderr,"FAIL: screenshot path is required\n");return 2;}
        QMessageBox box(QMessageBox::Question,"Подтвердите установку",
                        "Будет установлен русский перевод. Продолжить?",
                        QMessageBox::Yes|QMessageBox::No);
        box.setDefaultButton(QMessageBox::No);box.show();app.processEvents();
        auto *yes=box.button(QMessageBox::Yes);auto *no=box.button(QMessageBox::No);
        const QImage image=box.grab().toImage();
        auto *label=box.findChild<QLabel *>("qt_msgbox_label");
        if(!yes||!no||yes->text()!="Да"||no->text()!="Нет"||image.isNull()||
           qGray(image.pixel(5,5))>128||!label||label->palette().color(QPalette::WindowText).lightness()<160){
            fprintf(stderr,"FAIL: dialog appearance or Russian button labels\n");return 1;
        }
        if(!image.save(QString::fromLocal8Bit(argv[2]))){fprintf(stderr,"FAIL: cannot save dialog screenshot\n");return 1;}
        fprintf(stdout,"PASS: dark dialog, readable text and Russian buttons\n");return 0;
    }
    if(resourceCheck){
        const auto games=discoverWutheringInstallations(steamRoots());
        if(games.size()!=1){fprintf(stderr,"ERROR: Steam installations found: %lld\n",static_cast<long long>(games.size()));return 2;}
        if(games.first().gameDir.isEmpty()){fprintf(stderr,"ERROR: %s\n",qPrintable(games.first().discoveryError));return 2;}
        const QString version=EchoInstall::detectResourceVersion(games.first().gameDir);
        const QString warning=EchoInstall::englishMountPreflight(games.first().gameDir,version);
        const QString mount=EchoInstall::translationMount(games.first().gameDir,version);
        fprintf(stdout,"STEAM_INSTALL=%s\nGAME_CLIENT=%s\nRESOURCE_VERSION=%s\nTRANSLATION_MOUNT=%s\n",
                qPrintable(games.first().installDir),qPrintable(games.first().gameDir),qPrintable(version),warning.isEmpty()?qPrintable(mount):qPrintable(warning));
        return version.isEmpty()?1:0;
    }
    if(installerTest)return runInstallerTests();
    if(launchTest)return runLaunchTests();
    if(manifestTest)return runManifestTests();
    if(downloadTest)return runDownloadTests();
    if(autoInstallTest)return EchoInstall::runAutoInstallTests();
    Window window(smoke,!(smoke||uiTest||screenshot||controllerTest));
    attachController(&window,!(smoke||uiTest||screenshot||controllerTest));
    if(smoke)return 0;
    if(controllerTest){window.show();app.processEvents();if(!testController(&window)){fprintf(stderr,"FAIL: controller navigation\n");return 1;}fprintf(stdout,"PASS: controller page switch, back and focus navigation\n");return 0;}
    if(uiTest){
        window.resize(800,600);window.show();app.processEvents();
        QString failure;if(!window.uiChecks(&failure)){fprintf(stderr,"FAIL: UI assertion: %s\n",qPrintable(failure));return 1;}
        auto *play=window.findChild<QPushButton*>("playGameButton");
        auto *primary=window.findChild<QPushButton*>("primaryButton");
        auto *pages=window.findChild<QStackedWidget*>("pageStack");
        auto *homeScroll=pages?qobject_cast<QScrollArea*>(pages->widget(0)):nullptr;
        auto *homeViewport=homeScroll?homeScroll->viewport():nullptr;
        auto *check=window.findChild<QPushButton*>("checkUpdatesButton");
        auto globalRect=[&](QWidget *item){return QRect(window.mapFromGlobal(item->mapToGlobal(QPoint(0,0))),item->size());};
        const QRect viewportRect=homeViewport?globalRect(homeViewport):QRect();
        auto fullyVisible=[&](QWidget *item){return item&&item->isVisible()&&viewportRect.contains(globalRect(item));};
        if(!homeScroll||homeScroll->horizontalScrollBar()->maximum()!=0){fprintf(stderr,"FAIL: horizontal overflow at 800x600\n");return 1;}
        for(const auto &state: {"missing","healthy","repair"}) {
            window.applyScreenshotState(QString::fromLatin1(state));app.processEvents();
            auto *translationAction=QString::fromLatin1(state)=="healthy"?check:primary;
            if(!fullyVisible(play)||!fullyVisible(translationAction)){
                fprintf(stderr,"FAIL: Play or translation action is below the fold at 800x600 in %s state (viewport %d,%d %dx%d; play %d,%d %dx%d; action %d,%d %dx%d)\n",state,viewportRect.x(),viewportRect.y(),viewportRect.width(),viewportRect.height(),play?globalRect(play).x():0,play?globalRect(play).y():0,play?play->width():0,play?play->height():0,translationAction?globalRect(translationAction).x():0,translationAction?globalRect(translationAction).y():0,translationAction?translationAction->width():0,translationAction?translationAction->height():0);return 1;
            }
        }
        fprintf(stdout,"PASS: UI assertions — first-screen Play and translation action above fold in three states at 800x600, Settings ownership, accessible labels\n");return 0;
    }
    if(screenshot){if(screenshotPath.isEmpty()){fprintf(stderr,"FAIL: --ui-screenshot requires a file path\n");return 2;}if(!screenshotSize.isValid()||screenshotSize.width()<800||screenshotSize.height()<600){fprintf(stderr,"FAIL: screenshot size must be at least 800x600\n");return 2;}window.resize(screenshotSize);window.applyScreenshotState(screenshotState);window.show();app.processEvents();QDir().mkpath(QFileInfo(screenshotPath).absolutePath());if(!window.grab().save(screenshotPath,"PNG")){fprintf(stderr,"FAIL: could not save screenshot: %s\n",qPrintable(screenshotPath));return 1;}fprintf(stdout,"SCREENSHOT: %s (%dx%d, %s)\n",qPrintable(QFileInfo(screenshotPath).absoluteFilePath()),screenshotSize.width(),screenshotSize.height(),qPrintable(screenshotState));return 0;}
    window.show();
    return app.exec();
}


