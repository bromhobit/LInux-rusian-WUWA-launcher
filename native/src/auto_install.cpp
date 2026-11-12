#include "auto_install.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVersionNumber>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStorageInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QRegularExpression>
#include <QProcess>
#include <signal.h>
#include <unistd.h>
#include <xxhash.h>

namespace EchoInstall {
namespace {
constexpr auto OwnedRelative = "Lang_en/RU/pakchunk0-WindowsNoEditor_10100_P";
constexpr auto PakName = "pakchunk0-WindowsNoEditor_10100_P.pak";

QString canonical(const QString &p) { return QFileInfo(p).canonicalFilePath(); }
bool noSymlinkChain(const QString &path) {
    QFileInfo info(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    if(info.isSymLink()) return false; // includes dangling symlinks
    QString current=info.exists()?info.absoluteFilePath():info.absolutePath();
    while(!current.isEmpty()) {
        QFileInfo part(current);
        if(part.isSymLink()) return false;
        const QString parent=QFileInfo(current).absolutePath();
        if(parent==current) break;
        current=parent;
    }
    return true;
}
bool writeAtomic(const QString &path,const QByteArray &bytes,QString *error) {
    if(!noSymlinkChain(path)){if(error)*error="Путь содержит символическую ссылку.";return false;}
    QSaveFile f(path);f.setDirectWriteFallback(false);
    if(!f.open(QIODevice::WriteOnly)){if(error)*error=f.errorString();return false;}
    if(f.write(bytes)!=bytes.size()){if(error)*error=f.errorString();f.cancelWriting();return false;}
    if(!f.commit()){if(error)*error=f.errorString();return false;}return true;
}
QByteArray readAll(const QString &path,QString *error) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly)){if(error)*error=f.errorString();return {};}
    return f.readAll();
}
bool hasExactLine(QByteArray bytes,const QByteArray &expected) {
    if(bytes.startsWith("\xEF\xBB\xBF"))bytes.remove(0,3);
    for(QByteArray line:bytes.split('\n')){if(line.endsWith('\r'))line.chop(1);if(line==expected)return true;}
    return false;
}
QString transactionDir(const QString &appData) {
    const QString root=appData+"/backups/translation";
    if(!QDir().mkpath(root)||QFileInfo(root).isSymLink())return {};
    const QString dir=root+"/"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    return QDir().mkpath(dir)?dir:QString();
}
bool copyAtomic(const QString &from,const QString &to,QString *error) {
    if(!noSymlinkChain(from)||!noSymlinkChain(to)){if(error)*error="Путь копирования содержит символическую ссылку.";return false;}
    QFile in(from);if(!in.open(QIODevice::ReadOnly)){if(error)*error=in.errorString();return false;}
    QSaveFile out(to);out.setDirectWriteFallback(false);if(!out.open(QIODevice::WriteOnly)){if(error)*error=out.errorString();return false;}
    while(!in.atEnd()){QByteArray b=in.read(1024*1024);if(b.isEmpty()&&in.error()!=QFile::NoError){if(error)*error=in.errorString();out.cancelWriting();return false;}if(out.write(b)!=b.size()){if(error)*error=out.errorString();out.cancelWriting();return false;}}
    if(!out.commit()){if(error)*error=out.errorString();return false;}
    if(sha1File(from,error)!=sha1File(to,error)){QFile::remove(to);if(error)*error="Проверка копии SHA-1 не совпала.";return false;}return true;
}
bool xxh64Matches(const QString &path,const QString &expected,QString *error) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly)){if(error)*error=f.errorString();return false;}
    auto *s=XXH64_createState();if(!s||XXH64_reset(s,0)!=XXH_OK){if(s)XXH64_freeState(s);if(error)*error="XXH64 недоступен.";return false;}
    bool ok=true;while(!f.atEnd()){QByteArray b=f.read(4*1024*1024);if(b.isEmpty()&&f.error()!=QFile::NoError){ok=false;break;}if(XXH64_update(s,b.constData(),size_t(b.size()))!=XXH_OK){ok=false;break;}}
    QString got=QString::number(XXH64_digest(s),16).rightJustified(16,'0');XXH64_freeState(s);
    if(!ok||got.compare(expected,Qt::CaseInsensitive)!=0){if(error)*error=ok?"XXH64 пакета не совпадает с выбранным манифестом.":"Ошибка чтения PAK при проверке XXH64.";return false;}return true;
}
QJsonObject readJournal(const QString &path) { QString e;auto b=readAll(path,&e);auto d=QJsonDocument::fromJson(b);return d.isObject()?d.object():QJsonObject(); }
bool writeJournal(const QString &path,const QJsonObject &o,QString *error) {return writeAtomic(path,QJsonDocument(o).toJson(QJsonDocument::Indented),error);}
QString journalMount(const QJsonObject &journal) {
    const QString name=journal.value("mountTable").toString("MountLang_en.txt");
    return name=="MountLang_en.txt"||name=="MountResource.txt"?name:QString();
}
bool validOwnedJournal(const QJsonObject &j) {
    static const QRegularExpression versionRe("^[0-9]+(?:\\.[0-9]+){1,3}$");
    static const QRegularExpression hashRe("^[0-9a-f]{40}$");
    const QString root=j.value("gameRoot").toString(), version=j.value("resourceVersion").toString();
    const QString sha=j.value("sha1").toString();
    return !root.isEmpty()&&canonical(root)==root&&versionRe.match(version).hasMatch()&&
        hashRe.match(sha).hasMatch()&&!journalMount(j).isEmpty()&&
        j.value("pakPath").toString()==root+"/Client/Saved/Resources/"+version+"/"+QString::fromLatin1(OwnedRelative)+".pak"&&
        j.value("entry").toString()==QString::fromLatin1(OwnedRelative)+",10100,"+sha+",,,";
}

// The journal is evidence of an earlier install, not authority to overwrite
// unknown files. Offer reinstall only if its exact PAK is absent and the
// recorded Mount is readable, well formed, and has no conflicting ECHO line.
bool canReinstallMissingPak(const QJsonObject &j,const QString &root,const QString &version) {
    static const QRegularExpression versionRe("^[0-9]+(?:\\.[0-9]+){1,3}$");
    static const QRegularExpression hashRe("^[0-9a-f]{40}$");
    if(root.isEmpty() || canonical(root).isEmpty() || canonical(root)!=j.value("gameRoot").toString() ||
       !versionRe.match(version).hasMatch() || version!=j.value("resourceVersion").toString()) return false;
    const QString expectedPak=root+"/Client/Saved/Resources/"+version+"/"+QString::fromLatin1(OwnedRelative)+".pak";
    const QString pak=j.value("pakPath").toString();
    const QString sha=j.value("sha1").toString();
    const QByteArray entry=j.value("entry").toString().toUtf8();
    if(pak!=expectedPak || !hashRe.match(sha).hasMatch() ||
       entry!=(QString::fromLatin1(OwnedRelative)+",10100,"+sha+",,,").toUtf8()) return false;
    const QFileInfo pakInfo(pak);
    if(pakInfo.exists() || pakInfo.isSymLink() || !noSymlinkChain(pak)) return false;
    const QString table=journalMount(j);
    if(table.isEmpty()) return false;
    const QString mount=root+"/Client/Saved/Resources/"+version+"/Mount/"+table;
    if(!noSymlinkChain(mount) || !QFileInfo(mount).isFile()) return false;
    const QByteArray bytes=readAll(mount,nullptr);
    if(bytes.isEmpty()) return false;
    QByteArray removed; QString error;
    if(!transformMount(bytes,{},&removed,true,&error)) return false;
    return hasExactLine(bytes,entry) || removed==bytes;
}

}

QString sha1File(const QString &path,QString *error) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly)){if(error)*error=f.errorString();return {};}
    QCryptographicHash h(QCryptographicHash::Sha1);while(!f.atEnd()){QByteArray b=f.read(4*1024*1024);if(b.isEmpty()&&f.error()!=QFile::NoError){if(error)*error=f.errorString();return {};}h.addData(b);}return QString::fromLatin1(h.result().toHex());
}

QString detectResourceVersion(const QString &gameRoot) {
    if(gameRoot.isEmpty()) return {};
    const QString resources=gameRoot+"/Client/Saved/Resources";
    const QDir dir(resources);
    static const QRegularExpression versionPattern(R"(^[0-9]+(?:\.[0-9]+){1,3}$)");
    QStringList versions;
    for(const QString &version:dir.entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
        if(!versionPattern.match(version).hasMatch()) continue;
        const QFileInfo versionInfo(resources+"/"+version);
        const QFileInfo mountDir(resources+"/"+version+"/Mount");
        if(versionInfo.isSymLink()||!versionInfo.isDir()||mountDir.isSymLink()||!mountDir.isDir()) continue;
        bool recognized=false;
        for(const QString &name:QStringList{
            "MountResource.txt", "MountResource_HD.txt", "MountLang_en.txt",
            "MountLang_ja.txt", "MountLauncher.txt"}) {
            const QFileInfo marker(mountDir.absoluteFilePath()+"/"+name);
            if(marker.isFile()&&!marker.isSymLink()&&marker.size()>0) {recognized=true;break;}
        }
        if(recognized)versions<<version;
    }
    // Prefer the version recorded by the official launcher. The numerically
    // newest directory can instead be a future pre-download or stale data.
    QFile config(gameRoot+"/launcherDownloadConfig.json");
    if(config.open(QIODevice::ReadOnly)){
        const auto doc=QJsonDocument::fromJson(config.readAll());
        const auto bundles=doc.object().value("bundles").toObject();
        QStringList recorded;
        for(auto it=bundles.constBegin();it!=bundles.constEnd();++it){
            const QString version=it.value().toObject().value("version").toString();
            if(versionPattern.match(version).hasMatch()&&!recorded.contains(version))recorded<<version;
        }
        if(!recorded.isEmpty())
            return recorded.size()==1&&versions.contains(recorded.first())?recorded.first():QString();
    }
    return versions.size()==1?versions.first():QString();
}

QString translationMount(const QString &gameRoot,const QString &resourceVersion) {
    if(gameRoot.isEmpty()||resourceVersion.isEmpty())return {};
    const QString mountDir=gameRoot+"/Client/Saved/Resources/"+resourceVersion+"/Mount/";
    // The shared resource table supports English text with any installed voice.
    for(const QString &name:QStringList{"MountResource.txt","MountLang_en.txt"}) {
        const QFileInfo file(mountDir+name);
        if(file.isFile()&&!file.isSymLink()&&file.isReadable()&&file.size()>0&&noSymlinkChain(file.absoluteFilePath()))
            return name;
    }
    return {};
}

QString translationMountForInstall(const QString &gameRoot,const QString &resourceVersion,const QString &appDataPath) {
    const QJsonObject journal=readJournal(appDataPath+"/translation-install.json");
    const QString expectedPak=gameRoot+"/Client/Saved/Resources/"+resourceVersion+"/"+QString::fromLatin1(OwnedRelative)+".pak";
    if(!journal.isEmpty()&&journal.value("gameRoot").toString()==canonical(gameRoot)&&
       journal.value("resourceVersion").toString()==resourceVersion&&journal.value("pakPath").toString()==expectedPak)
        return journalMount(journal);
    return translationMount(gameRoot,resourceVersion);
}

QString englishMountPreflight(const QString &gameRoot,const QString &resourceVersion) {
    if(resourceVersion.isEmpty())
        return "Версия игровых ресурсов не определена: не найдена папка версии с таблицами Mount. Установка перевода заблокирована.";
    if(translationMount(gameRoot,resourceVersion).isEmpty())
        return QString("Для ресурсов игры %1 не найдена доступная штатная таблица MountResource.txt или MountLang_en.txt.").arg(resourceVersion);
    return {};
}

bool safeGameRoot(const QString &root,QString *error) {
    const QFileInfo r(root);
    if(!r.isDir()||r.isSymLink()||canonical(root).isEmpty()||!noSymlinkChain(root)) {
        if(error)*error="Папка игры отсутствует либо содержит символическую ссылку.";
        return false;
    }
    bool validLauncher=false;
    for(const QString &name:QStringList{"launcher.exe","Wuthering Waves.exe"}) {
        const QFileInfo exe(root+"/"+name);
        if(exe.isFile()&&!exe.isSymLink()) {validLauncher=true;break;}
    }
    if(!validLauncher) {
        if(error)*error="Не найден штатный launcher.exe или Wuthering Waves.exe.";
        return false;
    }
    const QString resources=root+"/Client/Saved/Resources";
    if(!noSymlinkChain(resources)||detectResourceVersion(root).isEmpty()) {
        if(error)*error="Не найдены корректные ресурсы Wuthering Waves или путь содержит символическую ссылку.";
        return false;
    }
    return true;
}


namespace {
QString detectGameProcess(const QByteArray &commRaw, const QByteArray &cmdRaw,
                          const QByteArray &canonicalRoot, bool *updater) {
    *updater=false;
    if(canonicalRoot.isEmpty()||cmdRaw.isEmpty())return {};
    QByteArray cmd=cmdRaw.toLower();
    cmd.replace('\\','/');
    if(!cmd.contains(canonicalRoot))return {};
    const QByteArray comm=commRaw.trimmed().toLower();
    const QByteArray first=cmd.split('\0').value(0);
    // Steam Proton may create a wine steam.exe proxy whose *argument* names
    // launcher.exe. It is not the actual launcher and must not block install.
    if(comm=="steam.exe"||comm=="bash"||comm=="sh"||comm=="codex")return {};
    if(comm.startsWith("client-win64")||first.contains("client-win64-shipping.exe"))
        return "игра Client-Win64-Shipping.exe";
    if(comm=="launcher.exe"||first.endsWith("/launcher.exe")||
       comm.startsWith("launcher_main")||first.endsWith("/launcher_main.exe")||
       comm.startsWith("wuthering wave")||first.endsWith("/wuthering waves.exe"))
        return "лаунчер Wuthering Waves";
    if(comm.startsWith("launcher_updat")||first.contains("launcher_updater.exe")){
        *updater=true;return "фоновый launcher_updater.exe";
    }
    if(comm.startsWith("krinstallextern")||first.contains("krinstallexternal.exe")){
        *updater=true;return "фоновый KRInstallExternal.exe";
    }
    return {};
}
}
QList<GameProcess> blockingProcesses(const QString &gameRoot) {
    QList<GameProcess> out;
    QString processRoot=canonical(gameRoot);
    // Include Steam's launcher and updater above a nested game-client folder.
    QString current=processRoot;
    for(int depth=0;depth<3&&!current.isEmpty();++depth){
        const QDir parent=QFileInfo(current).dir();
        if(parent.dirName()=="common"&&QFileInfo(parent.absolutePath()).dir().dirName()=="steamapps"){
            processRoot=current;break;
        }
        const QString next=parent.absolutePath();
        if(next==current)break;
        current=next;
    }
    const QByteArray root=processRoot.toUtf8().toLower();
    if(root.isEmpty())return out;
    QDir proc("/proc");
    for(const auto &id:proc.entryList(QDir::Dirs|QDir::NoDotAndDotDot)){
        bool ok=false;const qint64 pid=id.toLongLong(&ok);
        if(!ok||pid<=0)continue;
        QFile commFile("/proc/"+id+"/comm"),cmdFile("/proc/"+id+"/cmdline");
        if(!commFile.open(QIODevice::ReadOnly)||!cmdFile.open(QIODevice::ReadOnly))continue;
        bool updater=false;
        const QString name=detectGameProcess(commFile.readAll(),cmdFile.read(1024*1024),root,&updater);
        if(!name.isEmpty())out.append({pid,name,updater});
    }
    return out;
}
bool gameRunning(const QString &gameRoot) {
    return !blockingProcesses(gameRoot).isEmpty();
}
bool terminateBackgroundUpdaters(const QString &gameRoot,QString *message) {
    const auto processes=blockingProcesses(gameRoot);
    if(processes.isEmpty()){if(message)*message="Нет работающих процессов обновлятора.";return true;}
    for(const auto &p:processes)if(!p.updater){
        if(message)*message="Обнаружена запущенная игра или основной лаунчер. Закройте их штатно.";
        return false;
    }
    int stopped=0;
    for(const auto &p:processes){
        // Revalidate that the same PID still represents this game's helper
        // immediately before sending a user-confirmed termination signal.
        QFile commFile("/proc/"+QString::number(p.pid)+"/comm");
        if(!commFile.open(QIODevice::ReadOnly))continue;
        const QByteArray comm=commFile.readAll().trimmed().toLower();
        if(!(comm.startsWith("launcher_updat")||comm.startsWith("krinstallextern")))continue;
        if(::kill(static_cast<pid_t>(p.pid),SIGTERM)==0)++stopped;
    }
    if(message)*message=QString("Отправлено завершение %1 фоновым процессам обновлятора. Если Steam снова запустит обновление, дождитесь его завершения.").arg(stopped);
    return stopped>0;
}

bool transformMount(const QByteArray &original,const QByteArray &entry,QByteArray *updated,bool remove,QString *error) {
    QByteArray bom; QByteArray body=original;if(body.startsWith("\xEF\xBB\xBF")){bom=body.left(3);body.remove(0,3);}
    const bool crlf=body.contains("\r\n");const QByteArray newline=crlf?"\r\n":"\n";
    for(int i=0;i<body.size();++i){
        if((body[i]=='\r'&&(i+1>=body.size()||body[i+1]!='\n'))||
           (body[i]=='\n'&&crlf&&(i==0||body[i-1]!='\r'))){
            if(error)*error="Формат Mount содержит смешанные или неизвестные переводы строк.";
            return false;
        }
    }
    const bool hadTrailingNewline=body.endsWith('\n');QList<QByteArray> lines=body.split('\n');if(hadTrailingNewline&&!lines.isEmpty())lines.removeLast();for(auto &line:lines)if(line.endsWith('\r'))line.chop(1);
    int mount=-1,del=-1,owned=-1;
    const QByteArray prefix="Lang_en/RU/pakchunk0-WindowsNoEditor_10100_P,";
    for(int i=0;i<lines.size();++i){if(lines[i]=="::Mount::"){if(mount>=0){if(error)*error="В MountLang_en.txt повторяется ::Mount::.";return false;}mount=i;}if(lines[i]=="::Del::"){if(del>=0){if(error)*error="В MountLang_en.txt повторяется ::Del::.";return false;}del=i;}if(lines[i].startsWith(prefix)){if(owned>=0){if(error)*error="В MountLang_en.txt дублируется запись ECHO.";return false;}owned=i;}}
    if(mount<0||del<0||del<=mount){if(error)*error="MountLang_en.txt не содержит корректные секции ::Mount:: и ::Del::.";return false;}
    if(remove){if(owned>=0)lines.removeAt(owned);}else{
        if(owned>=0&&lines[owned]!=entry){if(error)*error="Запись ECHO уже существует с неизвестным содержимым; отказ от перезаписи.";return false;}
        if(owned<0)lines.insert(del,entry);
    }
    QByteArray result=bom;for(int i=0;i<lines.size();++i){if(i)result+=newline;result+=lines[i];}if(hadTrailingNewline)result+=newline;*updated=result;return true;
}

Result install(const QString &gameRoot,const QString &resourceVersion,const QString &channel,const QString &translationVersion,const QString &cachedPak,const QString &xxh64,const QString &appDataPath) {
    QString error;if(!safeGameRoot(gameRoot,&error))return {false,error};
    if(gameRunning(gameRoot))return {false,"Закройте Wuthering Waves перед изменением игровых файлов."};
    if(detectResourceVersion(gameRoot)!=resourceVersion)return {false,"Активная версия ресурсов игры изменилась или не определена."};
    static const QRegularExpression versionRe("^[0-9]+(?:\\.[0-9]+){1,3}$");
    if(!versionRe.match(resourceVersion).hasMatch()||!versionRe.match(translationVersion).hasMatch()||resourceVersion.section('.',0,1)!=translationVersion.section('.',0,1))return {false,"Версия ресурсов игры не совпадает с переводом по major/minor; установка заблокирована."};
    if(channel!="over"&&channel!="aniwie"&&channel!="qwikki")return {false,"Неизвестный канал перевода."};
    if(!noSymlinkChain(cachedPak)||!QFileInfo(cachedPak).isFile()||QFileInfo(cachedPak).size()<=0||!xxh64Matches(cachedPak,xxh64,&error))return {false,error.isEmpty()?"Проверенный PAK не найден.":error};
    const QString resources=gameRoot+"/Client/Saved/Resources";
    const QString journalPath=appDataPath+"/translation-install.json";
    const QJsonObject prior=readJournal(journalPath);
    if(QFileInfo::exists(journalPath)&&prior.isEmpty())return {false,"Журнал установки повреждён; установка остановлена."};
    if(!prior.isEmpty()&&journalMount(prior).isEmpty())return {false,"Журнал относится к неизвестной таблице Mount; установка заблокирована."};
    const QString mountTable=translationMountForInstall(gameRoot,resourceVersion,appDataPath);
    if(mountTable.isEmpty())return {false,"Не найдена штатная таблица Mount для перевода."};
    const QString mountPath=resources+"/"+resourceVersion+"/Mount/"+mountTable;
    if(!noSymlinkChain(mountPath)||!QFileInfo(mountPath).isFile())return {false,"Mount файла нет или путь содержит символическую ссылку."};
    const QString targetDir=resources+"/"+resourceVersion+"/"+QFileInfo(OwnedRelative).path();
    if(!noSymlinkChain(targetDir))return {false,"Папка перевода содержит символическую ссылку."};
    const QFileInfo mountInfo(mountPath);if(!mountInfo.isWritable()||!QFileInfo(mountInfo.absolutePath()).isWritable())return {false,"Нет прав на запись в папку Mount; игра не изменена."};
    QStorageInfo storage(mountInfo.absolutePath());if(!storage.isValid()||!storage.isReady()||storage.bytesAvailable()<QFileInfo(cachedPak).size()*2+mountInfo.size()+1024*1024)return {false,"Недостаточно места для перевода и резервных копий."};
    const QString targetPak=targetDir+"/"+PakName;
    if(!noSymlinkChain(targetPak))return {false,"Путь целевого PAK содержит символическую ссылку."};
    const bool owned=prior.value("pakPath").toString()==targetPak&&prior.value("gameRoot").toString()==canonical(gameRoot);
    if(owned&&!validOwnedJournal(prior))return {false,"Журнал установки повреждён или содержит неверные пути."};
    const bool reinstall=owned&&canReinstallMissingPak(prior,gameRoot,resourceVersion);
    if(QFileInfo::exists(targetPak)&&!owned)return {false,"В целевой папке уже есть неизвестный файл. Он не будет заменён."};
    if(owned){QString oldErr;if(!reinstall&&sha1File(targetPak,&oldErr)!=prior.value("sha1").toString())return {false,"Установленный PAK изменён или неизвестен; обновление остановлено."};}
    if(!QDir().mkpath(targetDir)||QFileInfo(targetDir).isSymLink()||!noSymlinkChain(targetDir))return {false,"Не удалось безопасно создать папку перевода."};
    const QByteArray oldMount=readAll(mountPath,&error);if(oldMount.isEmpty())return {false,"Не удалось прочитать "+mountTable+": "+error};
    const QString sha1=sha1File(cachedPak,&error);if(sha1.isEmpty())return {false,"Не удалось вычислить SHA-1 пакета: "+error};
    const QByteArray entry=(QString::fromLatin1(OwnedRelative)+",10100,"+sha1+",,,").toUtf8();QByteArray newMount;
    if(owned){
        // Only replace a Mount entry that still exactly matches our own journal.
        const QByteArray priorEntry=prior.value("entry").toString().toUtf8();
        if(priorEntry.isEmpty()||(!reinstall&&!hasExactLine(oldMount,priorEntry)))return {false,"Предыдущая запись ECHO в Mount изменилась; обновление остановлено."};
        QByteArray withoutOwned;
        if(!transformMount(oldMount,{},&withoutOwned,true,&error)||
           !transformMount(withoutOwned,entry,&newMount,false,&error))return {false,error};
    }else if(!transformMount(oldMount,entry,&newMount,false,&error))return {false,error};
    const QString tx=transactionDir(appDataPath);if(tx.isEmpty())return {false,"Не удалось создать каталог резервных копий."};
    if(!prior.isEmpty()&&!copyAtomic(journalPath,tx+"/translation-install.json",&error))return {false,"Не удалось сохранить прежний журнал: "+error};
    const QString mountBackup=tx+"/"+mountTable;if(!writeAtomic(mountBackup,oldMount,&error))return {false,"Не удалось сохранить Mount backup: "+error};
    const QString originalBackup=owned?prior.value("originalMountBackup").toString():mountBackup;
    QString previousPakBackup;
    if(owned&&!reinstall){previousPakBackup=tx+"/previous-translation.pak";if(!copyAtomic(targetPak,previousPakBackup,&error))return {false,"Не удалось сохранить старый PAK: "+error};}
    const bool hadPak=QFileInfo::exists(targetPak);
    auto rollbackPak=[&](){if(hadPak)return copyAtomic(previousPakBackup,targetPak,nullptr);return !QFileInfo::exists(targetPak)||QFile::remove(targetPak);};
    // Recheck after download and backup, immediately before writing.
    if(detectResourceVersion(gameRoot)!=resourceVersion||gameRunning(gameRoot)||
       readAll(mountPath,nullptr)!=oldMount||readJournal(journalPath)!=prior||
       (reinstall&&!canReinstallMissingPak(prior,gameRoot,resourceVersion))||
       (!owned&&QFileInfo::exists(targetPak))||
       (owned&&!reinstall&&sha1File(targetPak,nullptr)!=prior.value("sha1").toString()))
        return {false,"Состояние перевода изменилось; установка остановлена."};
    if(!copyAtomic(cachedPak,targetPak,&error))return {false,"Не удалось записать перевод: "+error};
    if(sha1File(targetPak,&error)!=sha1){rollbackPak();return {false,"Установленный PAK не прошёл SHA-1; выполнен откат."};}
    if(!writeAtomic(mountPath,newMount,&error)){rollbackPak();return {false,"Mount не записан, PAK восстановлен: "+error};}
    QJsonObject journal{{"gameRoot",canonical(gameRoot)},{"resourceVersion",resourceVersion},{"channel",channel},{"translationVersion",translationVersion},{"pakPath",targetPak},{"sha1",sha1},{"xxh64",xxh64.toLower()},{"entry",QString::fromUtf8(entry)},{"mountTable",mountTable},{"originalMountBackup",originalBackup},{"lastMountBackup",mountBackup},{"previousPakBackup",previousPakBackup}};
    if(!prior.isEmpty())journal.insert("previousJournalBackup",tx+"/translation-install.json");
    if(!writeJournal(journalPath,journal,&error)){
        writeAtomic(mountPath,oldMount,nullptr);rollbackPak();
        return {false,"Не удалось сохранить журнал; файлы откатаны: "+error};
    }
    return {true,QString("Установлен перевод %1 v%2. PAK проверен SHA-1.").arg(channel,translationVersion)};
}

Result uninstall(const QString &appDataPath) {
    const QString journalPath=appDataPath+"/translation-install.json";QJsonObject j=readJournal(journalPath);if(j.isEmpty())return {false,"Нет действующей записи об установке ECHO."};
    if(!validOwnedJournal(j))return {false,"Журнал установки повреждён или содержит неверные пути."};
    const QString game=j.value("gameRoot").toString(),version=j.value("resourceVersion").toString(),pak=j.value("pakPath").toString();QString error;
    const QString table=journalMount(j);if(table.isEmpty())return {false,"Журнал относится к неизвестной таблице Mount; удаление заблокировано."};
    if(!safeGameRoot(game,&error))return {false,error};
    if(gameRunning(game))return {false,"Закройте Wuthering Waves перед удалением перевода."};
    const QString resources=game+"/Client/Saved/Resources",mount=resources+"/"+version+"/Mount/"+table;
    if(pak!=resources+"/"+version+"/"+QString::fromLatin1(OwnedRelative)+".pak")return {false,"Путь PAK в журнале не соответствует установке ECHO."};
    if(!noSymlinkChain(mount)||!noSymlinkChain(pak))return {false,"Путь установки содержит символическую ссылку."};
    if(!QFileInfo(pak).isFile()||sha1File(pak,&error)!=j.value("sha1").toString())return {false,"Установленный PAK отсутствует или изменён; он оставлен без изменений."};
    const QByteArray current=readAll(mount,&error);QByteArray updated;if(current.isEmpty()||!transformMount(current,{},&updated,true,&error))return {false,"Не удалось безопасно проверить Mount: "+error};
    const QByteArray entry=j.value("entry").toString().toUtf8();
    if(entry.isEmpty())return {false,"Журнал не содержит запись ECHO. Ничего не удалено."};
    const bool present=hasExactLine(current,entry);
    // If the game restored MountLang_en.txt, our verified PAK can still be
    // uninstalled. Never remove an unknown line claiming the same path.
    if(!present&&updated!=current)
        return {false,"Mount содержит другую запись с путём ECHO; удаление заблокировано."};
    const QString tx=transactionDir(appDataPath);if(tx.isEmpty())return {false,"Не удалось создать резервную копию перед удалением."};
    if(!writeAtomic(tx+"/"+table,current,&error))
        return {false,"Не удалось сохранить Mount backup: "+error};
    if(!copyAtomic(pak,tx+"/installed-translation.pak",&error))
        return {false,"Не удалось сохранить резервную копию PAK: "+error};
    if(present&&!writeAtomic(mount,updated,&error))
        return {false,"Не удалось удалить запись ECHO из Mount: "+error};
    if(!QFile::remove(pak)){
        if(present)writeAtomic(mount,current,nullptr);
        return {false,"Не удалось удалить PAK; Mount восстановлен."};
    }
    if(!QFile::remove(journalPath))
        return {false,"PAK удалён, но журнал приложения не удалён: "+journalPath+". Резервная копия PAK: "+tx};
    return {true,"Перевод удалён. Файл PAK и Mount сохранены в резервной копии: "+tx};
}

QString installedStatus(const QString &appDataPath,const QString &gameRoot) {
    const auto state=inspect(appDataPath,gameRoot,{}, {}, {});
    return state.healthy?QString("Установлен: %1 v%2").arg(state.channel,state.translationVersion):QString();
}

Inspection inspect(const QString &appDataPath,const QString &gameRoot,const QString &currentResourceVersion,const QString &selectedChannel,const QString &selectedXxh64) {
    Inspection s;s.currentResourceVersion=currentResourceVersion;
    const QJsonObject j=readJournal(appDataPath+"/translation-install.json");
    if(j.isEmpty()){s.message="Перевод не установлен.";return s;}
    const QString table=journalMount(j);
    if(table.isEmpty()){s.owned=true;s.message="Журнал относится к неизвестной таблице Mount; управление заблокировано.";return s;}
    const QString recordedRoot=j.value("gameRoot").toString();
    if(recordedRoot.isEmpty()||(!gameRoot.isEmpty()&&canonical(gameRoot)!=recordedRoot)){s.message="Есть запись ECHO для другой установки игры.";return s;}
    if(!validOwnedJournal(j)){s.owned=true;s.message="Журнал установки повреждён или содержит неверные пути.";return s;}
    s.owned=true;s.channel=j.value("channel").toString();s.translationVersion=j.value("translationVersion").toString();s.installedResourceVersion=j.value("resourceVersion").toString();
    s.resourceVersionChanged=!currentResourceVersion.isEmpty()&&currentResourceVersion!=s.installedResourceVersion;
    s.selectedChannelDiffers=!selectedChannel.isEmpty()&&selectedChannel!=s.channel;
    s.packageDiffers=!selectedXxh64.isEmpty()&&selectedXxh64.compare(j.value("xxh64").toString(),Qt::CaseInsensitive)!=0;
    const QString pak=j.value("pakPath").toString(),mount=recordedRoot+"/Client/Saved/Resources/"+s.installedResourceVersion+"/Mount/"+table;
    const QString expectedPak=recordedRoot+"/Client/Saved/Resources/"+s.installedResourceVersion+"/"+QString::fromLatin1(OwnedRelative)+".pak";
    QString error;const bool pakOk=pak==expectedPak&&noSymlinkChain(pak)&&QFileInfo(pak).isFile()&&sha1File(pak,&error)==j.value("sha1").toString();
    QByteArray bytes;const bool mountReadable=noSymlinkChain(mount)&&QFileInfo(mount).isFile()&&(bytes=readAll(mount,nullptr)).size()>0;
    const QByteArray entry=j.value("entry").toString().toUtf8();const bool lineOk=mountReadable&&!entry.isEmpty()&&hasExactLine(bytes,entry);
    s.healthy=pakOk&&lineOk;
    s.repairable=pakOk&&!lineOk&&!s.resourceVersionChanged&&mountReadable&&!entry.isEmpty();
    s.reinstallable=!pakOk&&canReinstallMissingPak(j,recordedRoot,currentResourceVersion);
    if(s.healthy){
        if(s.resourceVersionChanged)s.message=QString("Перевод сохранён в ресурсах %1; обнаружена текущая версия %2.").arg(s.installedResourceVersion,currentResourceVersion);
        else if(s.selectedChannelDiffers)s.message=QString("Установлен %1 v%2. Выбран канал %3.").arg(s.channel,s.translationVersion,selectedChannel);
        else if(s.packageDiffers)s.message=QString("Установлен %1 v%2; доступен другой пакет выбранного канала.").arg(s.channel,s.translationVersion);
        else s.message=QString("Перевод установлен и проверен: %1 v%2.").arg(s.channel,s.translationVersion);
    } else if(s.reinstallable)s.message="PAK перевода отсутствует. Можно установить перевод повторно; совместимость с микропатчем не подтверждена.";
    else if(!pakOk)s.message="Установленный PAK отсутствует или его SHA-1 изменён. Удаление и восстановление заблокированы.";
    else if(s.resourceVersionChanged)s.message=QString("PAK принадлежит ресурсам %1, текущие ресурсы %2. Автоматическое перенесение запрещено.").arg(s.installedResourceVersion,currentResourceVersion);
    else if(s.repairable)s.message="PAK проверен, но запись Mount отсутствует. Файлы можно восстановить.";
    else s.message="Файлы установки повреждены или Mount недоступен; автоматическое исправление невозможно.";
    return s;
}

Result repair(const QString &appDataPath,const QString &currentResourceVersion) {
    const QString journalPath=appDataPath+"/translation-install.json";const QJsonObject j=readJournal(journalPath);
    if(j.isEmpty())return {false,"Не найдена принадлежащая ECHO запись установки."};
    if(!validOwnedJournal(j))return {false,"Журнал установки повреждён или содержит неверные пути."};
    const QString table=journalMount(j);if(table.isEmpty())return {false,"Журнал относится к неизвестной таблице Mount; восстановление заблокировано."};
    const QString game=j.value("gameRoot").toString(),version=j.value("resourceVersion").toString();QString error;
    if(!safeGameRoot(game,&error))return {false,error};
    if(!currentResourceVersion.isEmpty()&&currentResourceVersion!=version)return {false,"Перевод относится к другой версии ресурсов. Восстановление переноса заблокировано; сначала установите перевод для текущей версии."};
    if(gameRunning(game))return {false,"Закройте игру и лаунчер перед восстановлением Mount."};
    const QString pak=j.value("pakPath").toString(),mount=game+"/Client/Saved/Resources/"+version+"/Mount/"+table;
    if(pak!=game+"/Client/Saved/Resources/"+version+"/"+QString::fromLatin1(OwnedRelative)+".pak")return {false,"Путь PAK в журнале не соответствует установке ECHO."};
    if(!noSymlinkChain(pak)||!QFileInfo(pak).isFile()||sha1File(pak,&error)!=j.value("sha1").toString())return {false,"PAK не прошёл SHA-1 проверку; Mount не изменён."};
    if(!noSymlinkChain(mount)||!QFileInfo(mount).isFile())return {false,"Mount отсутствует или содержит символическую ссылку."};
    const QByteArray prior=j.value("entry").toString().toUtf8();if(prior.isEmpty())return {false,"Журнал не содержит точную строку ECHO; восстановление невозможно."};
    const QByteArray current=readAll(mount,&error);if(current.isEmpty())return {false,"Mount не прочитан: "+error};
    QByteArray updated;if(!transformMount(current,prior,&updated,false,&error))return {false,"Mount изменён неизвестным образом: "+error};
    if(updated==current)return {true,"Запись ECHO уже присутствует; файлы исправны."};
    const QString tx=transactionDir(appDataPath);if(tx.isEmpty()||!writeAtomic(tx+"/"+table,current,&error))return {false,"Не удалось сохранить резервную копию Mount: "+error};
    if(!writeAtomic(mount,updated,&error))return {false,"Mount не изменён: "+error};
    return {true,"Запись перевода восстановлена. Резервная копия: "+tx};
}

int runAutoInstallTests() {
    QTemporaryDir missingEnglish;
    if(!missingEnglish.isValid())return 1;
    const QString steamRoot=missingEnglish.path()+"/Steam Wuthering Waves";
    const QString versions=steamRoot+"/Client/Saved/Resources";
    const QString v370=versions+"/3.7.0/Mount",v380=versions+"/3.8.0/Mount";
    if(!QDir().mkpath(v370) || !QDir().mkpath(v380))return 1;
    {
        QFile exe(steamRoot+"/Wuthering Waves.exe");
        if(!exe.open(QIODevice::WriteOnly)||exe.write("steam")!=5)return 1;
        exe.close();
        QFile japanese(v370+"/MountLang_ja.txt");
        if(!japanese.open(QIODevice::WriteOnly)||japanese.write("::Mount::\n::Del::\n")<0)return 1;
        japanese.close();
        QFile resource(v370+"/MountResource.txt");
        if(!resource.open(QIODevice::WriteOnly)||resource.write("::Mount::\n::Del::\n")<0)return 1;
        resource.close();
    }
    QString rootError;
    if(detectResourceVersion(steamRoot)!="3.7.0"||
       !safeGameRoot(steamRoot,&rootError)||
       !englishMountPreflight(steamRoot,"3.7.0").isEmpty()||
       translationMount(steamRoot,"3.7.0")!="MountResource.txt"||
       QFileInfo::exists(v370+"/MountLang_en.txt")) {
        fprintf(stderr,"FAIL: Steam resources must be detected without inventing an English Mount: %s\n",qPrintable(rootError));
        return 1;
    }
    {
        QFile resource(v380+"/MountResource.txt");
        if(!resource.open(QIODevice::WriteOnly)||resource.write("::Mount::\n::Del::\n")<0)return 1;
        resource.close();
    }
    if(!detectResourceVersion(steamRoot).isEmpty())return 1;
    {
        QFile config(steamRoot+"/launcherDownloadConfig.json");
        if(!config.open(QIODevice::WriteOnly)||config.write("{\"bundles\":{\"HD\":{\"version\":\"3.7.0\"}}}")<0)return 1;
    }
    if(detectResourceVersion(steamRoot)!="3.7.0")return 1;
    {
        QFile config(steamRoot+"/launcherDownloadConfig.json");
        if(!config.open(QIODevice::WriteOnly|QIODevice::Truncate)||config.write("{\"bundles\":{\"HD\":{\"version\":\"3.8.0\"}}}")<0)return 1;
    }
    if(detectResourceVersion(steamRoot)!="3.8.0")return 1;
    if(!QFile::remove(v380+"/MountResource.txt")||!QFile::remove(steamRoot+"/launcherDownloadConfig.json")||detectResourceVersion(steamRoot)!="3.7.0")return 1;
    QTemporaryDir temp;if(!temp.isValid())return 1;const QString root=temp.path()+"/game",res=root+"/Client/Saved/Resources/3.7.0",mount=res+"/Mount/MountLang_en.txt",app=temp.path()+"/app",pak=temp.path()+"/translation.pak";
    QDir().mkpath(res+"/Mount");QDir().mkpath(app);
    QByteArray original="\xEF\xBB\xBF::Mount::\r\nLang_en/Base/pakchunk10-WindowsNoEditor,4,89C6309745252902C297007F3EB3D2C50807AF3A,EE7F1D1A2027E4367E45797CE56A3286202D778D,,\r\nLang_en/3.7.6/pakchunk10-WindowsNoEditor_P,10,17D0D8925FCC399BE97960D161D3767F4FBA957E,ACD316833F62174D5CDC437C0A3E4252C5E7740F,,\r\n::Del::\r\n";
    auto put=[](const QString&p,const QByteArray&b){QFile f(p);return f.open(QIODevice::WriteOnly)&&f.write(b)==b.size();};
    if(!put(root+"/launcher.exe","fixture")||!put(mount,original)||!put(pak,"synthetic verified translation payload"))return 1;
    const char payload[]="synthetic verified translation payload";const QString xxh=QString::number(XXH64(payload,sizeof(payload)-1,0),16).rightJustified(16,'0');
    QString err;if(!safeGameRoot(root,&err)){fprintf(stderr,"FAIL: fake root: %s\n",qPrintable(err));return 1;}const QByteArray third="ThirdParty/entry,2,ABC,,,\r\n";QByteArray withThird=original;withThird.replace("::Del::",third+"::Del::");if(!put(mount,withThird))return 1;
    auto result=install(root,"3.7.0","over","3.7",pak,xxh,app);if(!result.ok){fprintf(stderr,"FAIL: install: %s\n",qPrintable(result.message));return 1;}
    const QString target=res+"/Lang_en/RU/"+PakName;if(EchoInstall::sha1File(target)!=EchoInstall::sha1File(pak)||!EchoInstall::installedStatus(app,root).startsWith("Установлен: over v3.7")){fprintf(stderr,"FAIL: installed verification\n");return 1;}
    QFile mf(mount);if(!mf.open(QIODevice::ReadOnly))return 1;QByteArray patched=mf.readAll();if(!patched.startsWith("\xEF\xBB\xBF")||!patched.contains("\r\n")||!patched.contains(third)||patched.indexOf("Lang_en/RU/")>patched.indexOf("::Del::"))return 1;
    const auto healthy=inspect(app,root,"3.7.0","over",xxh);if(!healthy.owned||!healthy.healthy||healthy.packageDiffers||healthy.selectedChannelDiffers)return 1;
    const auto changedHash=inspect(app,root,"3.7.0","over","0000000000000000");if(!changedHash.healthy||!changedHash.packageDiffers)return 1;
    const auto changedResources=inspect(app,root,"3.8.0","over",xxh);if(!changedResources.healthy||!changedResources.resourceVersionChanged||changedResources.repairable)return 1;
    QByteArray withoutEcho;if(!transformMount(patched,{},&withoutEcho,true,&err)||!put(mount,withoutEcho))return 1;
    const auto damaged=inspect(app,root,"3.7.0","over",xxh);if(!damaged.owned||damaged.healthy||!damaged.repairable)return 1;
    result=repair(app,"3.7.0");if(!result.ok){fprintf(stderr,"FAIL: repair: %s\n",qPrintable(result.message));return 1;}
    mf.close();if(!mf.open(QIODevice::ReadOnly)||!mf.readAll().contains(EchoInstall::sha1File(target).toLatin1())||!EchoInstall::inspect(app,root,"3.7.0","over",xxh).healthy)return 1;
    const QString pak2=temp.path()+"/translation-v2.pak";const QByteArray payload2="second synthetic translation payload";if(!put(pak2,payload2))return 1;
    const QString xxh2=QString::number(XXH64(payload2.constData(),size_t(payload2.size()),0),16).rightJustified(16,'0');
    result=install(root,"3.7.0","aniwie","3.7.1",pak2,xxh2,app);if(!result.ok){fprintf(stderr,"FAIL: update: %s\n",qPrintable(result.message));return 1;}
    mf.close();if(!mf.open(QIODevice::ReadOnly))return 1;patched=mf.readAll();const QByteArray ownedPrefix="Lang_en/RU/pakchunk0-WindowsNoEditor_10100_P,";if(!patched.contains(EchoInstall::sha1File(pak2).toLatin1())||patched.count(ownedPrefix)!=1||EchoInstall::sha1File(target)!=EchoInstall::sha1File(pak2)||!EchoInstall::installedStatus(app,root).startsWith("Установлен: aniwie v3.7.1")){fprintf(stderr,"FAIL: updated Mount entry or package verification\n");return 1;}
    // A game update can remove the PAK, the Mount entry, or both while leaving
    // our journal intact. Exercise recovery only inside this synthetic tree.
    const QByteArray priorJournal=readAll(app+"/translation-install.json",nullptr);
    QByteArray cleanMount;
    if(!transformMount(patched,{},&cleanMount,true,&err)||!QFile::remove(target)||!put(mount,cleanMount))return 1;
    auto missing=inspect(app,root,"3.7.0","over",xxh);
    if(!missing.reinstallable || missing.healthy || missing.repairable ||
       readAll(app+"/translation-install.json",nullptr)!=priorJournal || readAll(mount,nullptr)!=cleanMount){
        fprintf(stderr,"FAIL: missing PAK and Mount entry not safely detected\n");return 1;
    }
    if(inspect(app,root,"3.8.0","over",xxh).reinstallable || inspect(app,root,{},"over",xxh).reinstallable){
        fprintf(stderr,"FAIL: missing PAK recovery accepted an unknown or changed resource version\n");return 1;
    }
    // An unknown Mount table must not be touched.
    QJsonObject otherMount=QJsonDocument::fromJson(priorJournal).object();
    otherMount.insert("mountTable","MountUnsupported.txt");
    if(!writeJournal(app+"/translation-install.json",otherMount,&err))return 1;
    const auto unsupported=inspect(app,root,"3.7.0","over",xxh);
    const QString otherGame=temp.path()+"/other-game";
    const QString otherTable=otherGame+"/Client/Saved/Resources/3.7.0/Mount";
    if(!QDir().mkpath(otherTable)||!put(otherGame+"/launcher.exe","fixture")||
       !put(otherTable+"/MountLang_en.txt",cleanMount))return 1;
    if(!unsupported.owned||unsupported.reinstallable||unsupported.healthy||
       install(root,"3.7.0","over","3.7",pak,xxh,app).ok||
       install(otherGame,"3.7.0","over","3.7",pak,xxh,app).ok||
       uninstall(app).ok||repair(app,"3.7.0").ok||
       readJournal(app+"/translation-install.json")!=otherMount||
       readAll(otherTable+"/MountLang_en.txt",nullptr)!=cleanMount){
        fprintf(stderr,"FAIL: journal for unsupported Mount table accepted\n");return 1;
    }
    if(!writeAtomic(app+"/translation-install.json",priorJournal,&err))return 1;
    QByteArray mixed=patched;
    const int firstCrlf=mixed.indexOf("\r\n");
    if(firstCrlf<0)return 1;
    mixed.remove(firstCrlf,1); // Keep the exact owned line but mix LF and CRLF.
    if(!put(mount,mixed)||inspect(app,root,"3.7.0","over",xxh).reinstallable||
       install(root,"3.7.0","over","3.7",pak,xxh,app).ok||readAll(mount,nullptr)!=mixed){
        fprintf(stderr,"FAIL: mixed Mount line endings accepted or rewritten\n");return 1;
    }
    // No line, exact old line, conflicting line, malformed table and dangling
    // symlink must be distinguished without changing the files.
    QByteArray foreign=cleanMount;foreign.replace("::Del::",ownedPrefix+"10100,0000000000000000000000000000000000000000,,,\r\n::Del::");
    if(!put(mount,foreign)||inspect(app,root,"3.7.0","over",xxh).reinstallable||
       install(root,"3.7.0","over","3.7",pak,xxh,app).ok||readAll(mount,nullptr)!=foreign){
        fprintf(stderr,"FAIL: conflicting ECHO Mount line accepted\n");return 1;
    }
    if(!put(mount,"::Mount::\n::Mount::\n::Del::\n") || inspect(app,root,"3.7.0","over",xxh).reinstallable ||
       install(root,"3.7.0","over","3.7",pak,xxh,app).ok){
        fprintf(stderr,"FAIL: malformed Mount accepted\n");return 1;
    }
    if(!put(mount,cleanMount))return 1;
    if(::symlink(QFile::encodeName(temp.path()+"/missing-file").constData(),QFile::encodeName(target).constData())!=0)return 1;
    if(inspect(app,root,"3.7.0","over",xxh).reinstallable||install(root,"3.7.0","over","3.7",pak,xxh,app).ok){
        fprintf(stderr,"FAIL: dangling PAK symlink accepted\n");return 1;
    }
    if(!QFile::remove(target)||!put(target,"changed PAK"))return 1;
    if(inspect(app,root,"3.7.0","over",xxh).reinstallable||install(root,"3.7.0","over","3.7",pak,xxh,app).ok||
       readAll(target,nullptr)!="changed PAK"){
        fprintf(stderr,"FAIL: changed PAK accepted or overwritten\n");return 1;
    }
    if(!QFile::remove(target))return 1;
    // A failed journal commit must restore the updated game's Mount and leave
    // the PAK missing, retaining the old journal for a later retry.
    if(::geteuid()!=0){
        const auto oldPermissions=QFile::permissions(app);
        if(!QFile::setPermissions(app,QFileDevice::ReadOwner|QFileDevice::ExeOwner))return 1;
        const auto failed=install(root,"3.7.0","over","3.7",pak,xxh,app);
        if(!QFile::setPermissions(app,oldPermissions))return 1;
        if(failed.ok || !failed.message.startsWith("Не удалось сохранить журнал;") ||
           QFileInfo::exists(target) || readAll(mount,nullptr)!=cleanMount ||
           readAll(app+"/translation-install.json",nullptr)!=priorJournal){
            fprintf(stderr,"FAIL: failed journal commit did not roll back reinstall\n");return 1;
        }
    }
    result=install(root,"3.7.0","over","3.7",pak,xxh,app);
    if(!result.ok||!inspect(app,root,"3.7.0","over",xxh).healthy||
       !readAll(mount,nullptr).contains(third)||readAll(mount,nullptr).count(ownedPrefix)!=1){
        fprintf(stderr,"FAIL: reinstall with removed Mount line: %s\n",qPrintable(result.message));return 1;
    }
    bool journalBackedUp=false;
    const QDir backups(app+"/backups/translation");
    for(const QString &name:backups.entryList(QDir::Dirs|QDir::NoDotAndDotDot))
        if(readAll(backups.filePath(name+"/translation-install.json"),nullptr)==priorJournal)journalBackedUp=true;
    if(!journalBackedUp){fprintf(stderr,"FAIL: old journal was not backed up\n");return 1;}
    // PAK missing but the exact journal-owned Mount line survives.
    if(!QFile::remove(target)||!inspect(app,root,"3.7.0","over",xxh).reinstallable)return 1;
    result=install(root,"3.7.0","over","3.7",pak,xxh,app);
    if(!result.ok||!inspect(app,root,"3.7.0","over",xxh).healthy||readAll(mount,nullptr).count(ownedPrefix)!=1){
        fprintf(stderr,"FAIL: reinstall with retained Mount line: %s\n",qPrintable(result.message));return 1;
    }
    // Simulate Steam verifier discarding only our Mount entry before removal.
    QByteArray restored;mf.close();if(!mf.open(QIODevice::ReadOnly))return 1;
    const QByteArray beforeMissing=mf.readAll();
    if(!transformMount(beforeMissing,{},&restored,true,&err)||!put(mount,restored))return 1;
    result=EchoInstall::uninstall(app);
    if(!result.ok){fprintf(stderr,"FAIL: uninstall after Mount reset: %s\n",qPrintable(result.message));return 1;}mf.close();if(!mf.open(QIODevice::ReadOnly)||mf.readAll()!=withThird||QFileInfo::exists(target)){mf.close();const bool reopened=mf.open(QIODevice::ReadOnly);fprintf(stderr,"FAIL: uninstall preservation actual=%s expected=%s pak=%d reopen=%d\n",reopened?mf.readAll().toHex().constData():"<unreadable>",withThird.toHex().constData(),QFileInfo::exists(target),reopened);return 1;}
    QByteArray malformed="::Mount::\n::Mount::\n::Del::\n";QByteArray out;if(EchoInstall::transformMount(malformed,"x",&out,false,&err))return 1;
    const QString link=root+"/Client/Saved/Resources/3.7.0/linked";QFile::link(res+"/Mount",link);if(QFileInfo(link).isSymLink()&&noSymlinkChain(link+"/MountLang_en.txt"))return 1;
    if(install(root,"3.7.0","over","3.8",pak,xxh,app).ok)return 1;
    // Reproduce the real upgrade: the old 3.7 journal uses the shared table,
    // while the new 3.8 client lives inside the Steam launcher directory.
    const QString upgradeApp=temp.path()+"/upgrade-app";
    const QString newGame=steamRoot+"/Wuthering Waves Game";
    const QString newMountDir=newGame+"/Client/Saved/Resources/3.8.0/Mount";
    if(!QDir().mkpath(upgradeApp)||!QDir().mkpath(newMountDir)||
       !put(newGame+"/Wuthering Waves.exe","client")||
       !put(newMountDir+"/MountResource.txt","::Mount::\n::Del::\n")||
       !put(newMountDir+"/MountLang_ja.txt","::Mount::\nJapanese-voice\n::Del::\n"))return 1;
    result=install(steamRoot,"3.7.0","over","3.7",pak,xxh,upgradeApp);
    if(!result.ok){fprintf(stderr,"FAIL: shared Mount install: %s\n",qPrintable(result.message));return 1;}
    const QByteArray legacyJournal=readAll(upgradeApp+"/translation-install.json",nullptr);
    const QByteArray legacyMount=readAll(v370+"/MountResource.txt",nullptr);
    const QByteArray japaneseBefore=readAll(newMountDir+"/MountLang_ja.txt",nullptr);
    result=install(newGame,"3.8.0","over","3.8",pak2,xxh2,upgradeApp);
    const QJsonObject upgraded=readJournal(upgradeApp+"/translation-install.json");
    if(!result.ok||upgraded.value("mountTable").toString()!="MountResource.txt"||
       readAll(upgraded.value("previousJournalBackup").toString(),nullptr)!=legacyJournal||
       readAll(v370+"/MountResource.txt",nullptr)!=legacyMount||
       readAll(newMountDir+"/MountLang_ja.txt",nullptr)!=japaneseBefore||
       !inspect(upgradeApp,newGame,"3.8.0","over",xxh2).healthy) {
        fprintf(stderr,"FAIL: new-client migration or old journal preservation: %s\n",qPrintable(result.message));return 1;
    }
    if(!put(newMountDir+"/MountResource.txt","::Mount::\n::Del::\n")||
       !inspect(upgradeApp,newGame,"3.8.0","over",xxh2).repairable||
       !repair(upgradeApp,"3.8.0").ok||!uninstall(upgradeApp).ok||
       readAll(newMountDir+"/MountLang_ja.txt",nullptr)!=japaneseBefore) {
        fprintf(stderr,"FAIL: shared Mount repair or uninstall\n");return 1;
    }
    fprintf(stdout,"PASS: Steam install without English Mount, resource-version fallback, fake Steam game install, cross-version/channel update, journal+SHA-1 inspection, changed resource/hash detection, missing-entry repair, uninstall, XXH64, BOM+CRLF and third-party preservation, malformed Mount/version/symlink rejection\n");return 0;
}
}

