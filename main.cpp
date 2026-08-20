#include "aiagent.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    AIagent w;
    w.show();
    return QApplication::exec();
}
