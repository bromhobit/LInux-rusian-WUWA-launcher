#include "steam_options.h"

#include <QProcess>
#include <cstdio>

namespace EchoSteamOptions {
MergeResult merge(const QString &existing, const QString &executable) {
    auto reject=[&](const QString &why){return MergeResult{false,false,existing,why};};
    if(executable.isEmpty() || !executable.startsWith('/') || executable.contains('\n') ||
       executable.contains('\r') || executable.contains(QChar::Null))
        return reject("Не удалось определить безопасный абсолютный путь к ECHO.");
    QString quoted=executable;
    quoted.replace("'", "'\\''");
    const QString wrapper="'"+quoted+"' --steam-wrap ";
    if(existing.contains('\n') || existing.contains('\r') || existing.contains(QChar::Null))
        return reject("Параметры Steam содержат перевод строки.");
    bool onlyShellWhitespace=true;
    for(const QChar c:existing)if(c!=' ' && c!='\t'){onlyShellWhitespace=false;break;}
    if(onlyShellWhitespace)return {true,false,existing+wrapper+"%command%",{}};

    // Only replace one unquoted, unescaped, whitespace-delimited placeholder.
    // Shell operators and substitutions make command order ambiguous; refuse
    // them rather than silently change the user's command semantics.
    QChar quote;
    bool escape=false;
    int count=0, pos=-1;
    for(int i=0;i<existing.size();++i){
        const QChar c=existing.at(i);
        if(escape){escape=false;continue;}
        if(c=='\\' && quote!='\''){escape=true;continue;}
        if(!quote.isNull()){
            if(c==quote){quote=QChar();continue;}
            if(quote=='"' && (c=='`' || (c=='$' && i+1<existing.size() && existing.at(i+1)=='(')))
                return reject("Параметры Steam содержат подстановку команды.");
            continue;
        }
        if(c=='\'' || c=='"'){quote=c;continue;}
        if(QStringLiteral(";|&<>()`#").contains(c) || (c=='$' && i+1<existing.size() && existing.at(i+1)=='('))
            return reject("Сложные параметры Steam требуют ручной настройки.");
        if(existing.mid(i,9)=="%command%"){
            const bool left=i==0 || existing.at(i-1)==' ' || existing.at(i-1)=='\t';
            const bool right=i+9==existing.size() || existing.at(i+9)==' ' || existing.at(i+9)=='\t';
            if(!left || !right)return reject("%command% должен быть отдельным аргументом.");
            ++count;pos=i;i+=8;
        }
    }
    if(!quote.isNull() || escape)return reject("В параметрах Steam незакрытые кавычки или экранирование.");
    if(count!=1 || existing.count("%command%",Qt::CaseInsensitive)!=1)
        return reject("Нужен ровно один неэкранированный %command%.");
    // Refuse an existing wrapper, including a differently installed ECHO,
    // rather than wrap it twice or claim it points to the current binary.
    if(existing.contains("--steam-wrap"))
        return {false,true,existing,"Обёртка ECHO уже указана; параметры не изменены."};
    QString result=existing;
    result.insert(pos,wrapper);
    return {true,false,result,{}};
}

int runSteamOptionsTests(){
    auto check=[](bool ok,const char *what){if(!ok)fprintf(stderr,"FAIL: %s\n",what);return ok;};
    const QString path="/tmp/ECHO user's $(test).AppImage";
    const auto merged=merge("PROTON_FSR4_UPGRADE=1 gamemoderun %command% --foo",path);
    if(!check(merged.ok && merged.options.startsWith("PROTON_FSR4_UPGRADE=1 gamemoderun ") &&
              merged.options.endsWith(" --steam-wrap %command% --foo"),"preserve options"))return 1;
    QString probe=merged.options;
    probe.replace("PROTON_FSR4_UPGRADE=1 gamemoderun ","");
    probe.replace("%command%","sentinel");
    QProcess shell;shell.start("/bin/sh",{"-c","set -- "+probe+"; printf '%s\\n' \"$@\""});
    if(!check(shell.waitForFinished() && shell.exitCode()==0 &&
       shell.readAllStandardOutput()==path.toUtf8()+"\n--steam-wrap\nsentinel\n--foo\n","shell quoting"))return 1;
    const auto empty=merge({},"/usr/bin/echo-native");
    if(!check(empty.ok && empty.options=="'/usr/bin/echo-native' --steam-wrap %command%","empty options"))return 1;
    for(const auto &s:QStringList{"gamemoderun", "%command% %command%", "\"%command%\"", "x%command%", "echo hi; %command%", "$(whoami) %command%", "'unfinished %command%", "\\%command%"}){
        const auto r=merge(s,"/usr/bin/echo-native");
        if(!check(!r.ok && r.options==s,"ambiguous options changed"))return 1;
    }
    const auto repeated=merge(empty.options,"/usr/bin/echo-native");
    if(!check(!repeated.ok && repeated.alreadyConfigured && repeated.options==empty.options,"repeat wrapper"))return 1;
    const auto quotedArgs=merge("env FOO='a b' \"/opt/my wrapper\" %command%", "/usr/bin/echo-native");
    if(!check(quotedArgs.ok && quotedArgs.options.startsWith("env FOO='a b' \"/opt/my wrapper\" "),"quoted preceding arguments"))return 1;
    fprintf(stdout,"PASS: Steam options preservation, wrapper quoting, ambiguity and duplicate rejection\n");
    return 0;
}
}
