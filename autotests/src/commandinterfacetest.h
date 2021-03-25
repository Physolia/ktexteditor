#ifndef COMMANDINTERFACETEST_H
#define COMMANDINTERFACETEST_H

#include <QObject>

class CommandInterfaceTest : public QObject
{
    Q_OBJECT
public:
    explicit CommandInterfaceTest(QObject *parent = nullptr);

private Q_SLOTS:
    void basic();
    void basicv2();
};

#endif // COMMANDINTERFACETEST_H
