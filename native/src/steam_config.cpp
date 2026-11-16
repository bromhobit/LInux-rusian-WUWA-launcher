#include "steam_config.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QVector>
#include <cstdio>

namespace EchoSteam {
namespace {
struct Node { QString key, value; bool object{}; QVector<Node> children; };

// Parse quoted Valve KeyValues conservatively. Unknown escapes are retained
// literally; duplicate keys remain visible to the caller rather than merged.
class Parser {
    QString s; qsizetype p{}; int nodes{};
    void skip() {
        while (p<s.size()) {
            if (s[p].isSpace() || s[p]==QChar(0xfeff)) { ++p; continue; }
            if (s[p]=='/' && p+1<s.size() && s[p+1]=='/') {
                p+=2; while(p<s.size() && s[p]!='\n') ++p; continue;
            }
            break;
        }
    }
    bool quoted(QString *out) {
        skip(); if(p>=s.size() || s[p++]!='"') return false;
        while(p<s.size()) {
            const QChar c=s[p++];
            if(c=='"') return true;
            if(c=='\\' && p<s.size() && (s[p]=='"' || s[p]=='\\')) { out->append(s[p++]); continue; }
            if(c==QChar::Null) return false;
            out->append(c);
        }
        return false;
    }
    bool list(QVector<Node> *out,int depth,bool closing) {
        if(depth>64) return false;
        while(true) {
            skip();
            if(p==s.size()) return !closing;
            if(s[p]=='}') { if(!closing)return false; ++p; return true; }
            if(++nodes>500000) return false;
            Node n;if(!quoted(&n.key))return false;
            skip();if(p>=s.size())return false;
            if(s[p]=='{') { ++p;n.object=true;if(!list(&n.children,depth+1,true))return false; }
            else if(!quoted(&n.value)) return false;
            out->append(n);
        }
    }
public:
    explicit Parser(const QString &input):s(input){}
    bool parse(QVector<Node> *out) { return list(out,0,false); }
};

bool load(const QString &path,QVector<Node> *out) {
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly) || f.size()>32*1024*1024) return false;
    const QByteArray b=f.readAll();
    if(f.error()!=QFile::NoError) return false;
    const QString s=QString::fromUtf8(b);
    if(s.toUtf8()!=b) return false;
    return Parser(s).parse(out);
}

// Returns null for both a missing key and duplicates; callers requiring an
// optional key also inspect the count to distinguish those cases.
const Node *unique(const QVector<Node> &nodes,const QString &key,int *count=nullptr) {
    const Node *found=nullptr;int n=0;
    for(const auto &item:nodes)if(item.key.compare(key,Qt::CaseInsensitive)==0){found=&item;++n;}
    if(count)*count=n;
    return n==1?found:nullptr;
}
const Node *object(const QVector<Node> &nodes,const QString &key) {
    const Node *n=unique(nodes,key);return n && n->object?n:nullptr;
}
QString accountFor(const QVector<Node> &top) {
    const Node *users=object(top,"users");if(!users)return {};
    QStringList all,recent;
    constexpr quint64 base=76561197960265728ULL;
    for(const Node &n:users->children) {
        bool valid=false;const quint64 id=n.key.toULongLong(&valid);
        if(!valid||n.key.size()!=17||id<=base||id-base>4294967295ULL||!n.object)return {};
        const QString uid=QString::number(id-base);if(all.contains(uid))return {};all<<uid;
        int count=0;const Node *r=unique(n.children,"MostRecent",&count);
        if(count>1||(r&&(r->object||(r->value!="0"&&r->value!="1"))))return {};
        if(r&&r->value=="1")recent<<uid;
    }
    if(recent.size()==1)return recent.first();
    if(recent.isEmpty()&&all.size()==1)return all.first();
    return {};
}
}

SteamConfigRead readSteamConfig(const QStringList &roots) {
    QSet<QString> uniqueRoots;
    for(const QString &root:roots){const QString c=QFileInfo(root).canonicalFilePath();if(!c.isEmpty())uniqueRoots.insert(c);}
    QSet<QString> configs, accounts;
    for(const QString &root:uniqueRoots) {
        const QString login=root+"/config/loginusers.vdf";
        if(!QFileInfo::exists(login))continue;
        QVector<Node> tree;if(!load(login,&tree))return {false,false,{},"Не удалось прочитать список аккаунтов Steam."};
        const QString uid=accountFor(tree);if(uid.isEmpty())return {false,false,{},"Не удалось однозначно определить активный аккаунт Steam."};
        accounts.insert(uid);
        const QFileInfo config(root+"/userdata/"+uid+"/config/localconfig.vdf");
        if(config.isFile()&&!config.canonicalFilePath().isEmpty())configs.insert(config.canonicalFilePath());
    }
    if(accounts.size()!=1||configs.size()!=1)return {false,false,{},"Не удалось однозначно определить настройки активного аккаунта Steam."};
    QVector<Node> tree;if(!load(*configs.cbegin(),&tree))return {false,false,{},"Не удалось прочитать настройки Steam."};
    const Node *n=object(tree,"UserLocalConfigStore");
    for(const auto &part:QStringList{"Software","Valve","Steam","apps"}) {
        if(!n)return {false,false,{},"Структура настроек Steam не распознана."};
        n=object(n->children,part);
    }
    if(!n)return {false,false,{},"Структура настроек Steam не распознана."};
    int appCount=0;const Node *app=unique(n->children,"3513350",&appCount);
    if(appCount==0)return {true,false,{}, {}};
    if(!app||!app->object)return {false,false,{},"Запись игры в Steam неоднозначна."};
    int count=0;const Node *opt=unique(app->children,"LaunchOptions",&count);
    if(count==0)return {true,false,{}, {}};
    if(!opt||opt->object)return {false,false,{},"Параметры запуска в Steam неоднозначны."};
    return {true,true,opt->value,{}};
}

int runSteamConfigTests() {
    QTemporaryDir tmp;if(!tmp.isValid())return 1;
    const QString root=tmp.path()+"/steam",uid="12345";
    if(!QDir().mkpath(root+"/config")||!QDir().mkpath(root+"/userdata/"+uid+"/config"))return 1;
    auto put=[](const QString &path,const QByteArray &data){QFile f(path);return f.open(QIODevice::WriteOnly|QIODevice::Truncate)&&f.write(data)==data.size();};
    const QString login=root+"/config/loginusers.vdf",cfg=root+"/userdata/"+uid+"/config/localconfig.vdf";
    const QByteArray users="\"users\" { \"76561197960278073\" { \"MostRecent\" \"1\" } }";
    const QByteArray begin="\"UserLocalConfigStore\" { \"Software\" { \"Valve\" { \"Steam\" { \"apps\" { \"3513350\" { ";
    const QByteArray end=" } } } } } }";
    if(!put(login,users)||!put(cfg,begin+"\"LaunchOptions\" \"A=1 wrapper \\\"path with spaces\\\" %command%\""+end))return 1;
    const auto state=readSteamConfig({root,root});
    if(!state.ok||!state.present||state.options!="A=1 wrapper \"path with spaces\" %command%")return 1;
    if(!put(cfg,begin+end)||!readSteamConfig({root}).ok||readSteamConfig({root}).present)return 1;
    if(!put(cfg,begin+"\"LaunchOptions\" \"a\" \"LaunchOptions\" \"b\""+end)||readSteamConfig({root}).ok)return 1;
    if(!put(login,"\"users\" { \"76561197960278073\" { \"MostRecent\" \"1\" } \"76561197960278074\" { \"MostRecent\" \"1\" } }")||readSteamConfig({root}).ok)return 1;
    if(!put(login,users)||!put(cfg,"\"UserLocalConfigStore\" { bad")||readSteamConfig({root}).ok)return 1;
    fprintf(stdout,"PASS: Steam active account selection, options decoding, duplicates and malformed VDF\n");return 0;
}
}
