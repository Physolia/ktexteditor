#include "commandinterfacetest.h"

#include <KTextEditor/Command>
#include <KTextEditor/Editor>

#include <QTest>

QTEST_MAIN(CommandInterfaceTest)

class BasicCmd : public KTextEditor::Command
{
public:
    BasicCmd(QObject *parent = nullptr)
        : KTextEditor::Command(QStringList{QStringLiteral("cmd1"), QStringLiteral("cmd2")}, parent)
    {
    }

public:
    bool exec(KTextEditor::View *, const QString &cmd, QString &msg, const KTextEditor::Range &) override
    {
        if (cmd == QLatin1String("cmd1")) {
            msg = QStringLiteral("cmd1 exec");
            return true;
        }
        if (cmd == QLatin1String("cmd2")) {
            msg = QStringLiteral("cmd2 exec");
            return true;
        }
        return false;
    }
    bool help(KTextEditor::View *, const QString &cmd, QString &msg) override
    {
        if (cmd == QLatin1String("cmd1")) {
            msg = QStringLiteral("help cmd1");
            return true;
        }
        if (cmd == QLatin1String("cmd2")) {
            msg = QStringLiteral("help cmd2");
            return true;
        }
        return false;
    }
};

CommandInterfaceTest::CommandInterfaceTest(QObject *parent)
    : QObject(parent)
{
}

void CommandInterfaceTest::basic()
{
    // register command
    BasicCmd cmd;

    auto editor = KTextEditor::Editor::instance();
    QVERIFY(editor);

    QString out;

    auto cmd1 = editor->queryCommand("cmd1");
    QVERIFY(cmd1);
    // exec
    bool res = cmd1->exec(nullptr, QStringLiteral("cmd1"), out);
    QVERIFY(res);
    QCOMPARE(out, QStringLiteral("cmd1 exec"));
    // help
    res = cmd1->help(nullptr, QStringLiteral("cmd1"), out);
    QVERIFY(res);
    QCOMPARE(out, QStringLiteral("help cmd1"));

    auto cmd2 = editor->queryCommand("cmd2");
    QVERIFY(cmd2);
    res = cmd2->exec(nullptr, QStringLiteral("cmd2"), out);
    QVERIFY(res);
    QCOMPARE(out, QStringLiteral("cmd2 exec"));
    // help
    res = cmd2->help(nullptr, QStringLiteral("cmd2"), out);
    QVERIFY(res);
    QCOMPARE(out, QStringLiteral("help cmd2"));

    out.clear();

    // invalid cmds
    QVERIFY(!editor->queryCommand("invalidd"));
    QVERIFY(!cmd2->exec(nullptr, QStringLiteral("cccc"), out));
    QVERIFY(out.isEmpty());
}

/** CommandV2 Tests **/
class V2Cmd : public KTextEditor::CommandV2
{
    Q_OBJECT
public:
    V2Cmd(QObject *parent = nullptr)
        : KTextEditor::CommandV2(QStringList{QStringLiteral("v2cmd")}, parent)
    {
    }

public:
    bool exec(KTextEditor::View *, const QString &cmd, QString &msg, const KTextEditor::Range &) override
    {
        if (cmd == QLatin1String("v2cmd")) {
            msg = QStringLiteral("exec");
            return true;
        }
        msg = QString();
        return false;
    }
    bool help(KTextEditor::View *, const QString &cmd, QString &msg) override
    {
        if (cmd == QLatin1String("v2cmd")) {
            msg = QStringLiteral("help");
            return true;
        }
        msg = QString();
        return false;
    }

    QVariantMap run(const QString &cmd, const QVariantMap &args) override
    {
        QString msg;
        if (cmd == QLatin1String("v2cmd")) {
            auto p1 = args[QStringLiteral("p1")];
            if (p1 == 1) {
                msg = QStringLiteral("run p1");
                return {{QStringLiteral("run"), QStringLiteral("p1=1")}, {QStringLiteral("msg"), msg}};
            }

            p1 = args[QStringLiteral("p1")];
            if (p1 == 2) {
                msg = QStringLiteral("run p2");
                return {{QStringLiteral("run"), QStringLiteral("p1=2")}, {QStringLiteral("msg"), msg}};
            }
        }
        return {};
    }
};

void CommandInterfaceTest::basicv2()
{
    auto editor = KTextEditor::Editor::instance();
    QVERIFY(editor);
    // BEGIN V2Cmd Scope
    {
        V2Cmd v2;

        auto v2Cmd = qobject_cast<KTextEditor::CommandV2 *>(editor->queryCommand(QStringLiteral("v2cmd")));
        QVERIFY(v2Cmd);

        QVariantMap args = {{QStringLiteral("p1"), 1}};
        auto res = v2Cmd->run(QStringLiteral("v2cmd"), args);
        QCOMPARE(res[QStringLiteral("msg")].toString(), QStringLiteral("run p1"));
        QCOMPARE(res[QStringLiteral("run")].toString(), QStringLiteral("p1=1"));

        args[QStringLiteral("p1")] = 2;
        res = v2Cmd->run(QStringLiteral("v2cmd"), args);
        QCOMPARE(res[QStringLiteral("msg")].toString(), QStringLiteral("run p2"));
        QCOMPARE(res[QStringLiteral("run")].toString(), QStringLiteral("p1=2"));
    }
    // END V2Cmd Scope
    // scope ended, so command shouldn't be there
    auto v2Cmd = qobject_cast<KTextEditor::CommandV2 *>(editor->queryCommand(QStringLiteral("v2cmd")));
    QVERIFY(!v2Cmd);
}

#include "commandinterfacetest.moc"
