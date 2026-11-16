#include "fps_unlock.h"
#include <QCoreApplication>
#include <cstdio>
int main(int argc,char **argv) { QCoreApplication app(argc,argv); if(argc!=3) return 3; auto result=QString(argv[1])=="on"?EchoFps::enable(argv[2]):EchoFps::disable(argv[2]); fprintf(stdout,"%s: %s\n",result.ok?"OK":"ERROR",qPrintable(result.message)); return result.ok?0:1; }
