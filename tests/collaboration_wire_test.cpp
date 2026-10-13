#include "collaboration_wire.h"
#include <QJsonDocument>
#include <QtTest>

class CollaborationWireTest:public QObject {
    Q_OBJECT
private slots:
    void fragmentedSignedBytesAndHeartbeats() {
        const QByteArray payload=QStringLiteral("中文 } \\\" {").toUtf8()+QByteArray::fromHex("0080ff");
        const auto wire=Collaboration::payloadFrame(payload,123);
        QVERIFY(wire.contains("-128")); QVERIFY(wire.contains("-1"));
        Collaboration::MessageReader reader; QList<QByteArray> packets;
        const auto stream=QByteArray("{\"type\":1}")+wire+wire;
        for(const char byte:stream) QVERIFY(reader.append(QByteArrayView(&byte,1),packets));
        QCOMPARE(packets,QList<QByteArray>({payload,payload}));
    }
    void stringsContainingBracesDoNotSplitFrames() {
        Collaboration::MessageReader reader; QList<QByteArray> packets;
        QVERIFY(reader.append("{\"type\":1,\"unused\":\"}\\\"{\"}{\"type\":0,\"data\":[65]}",packets));
        QCOMPARE(packets,QList<QByteArray>({"A"}));
    }
    void malformedAndOversizedFrames() {
        for(const QByteArray &wire:{QByteArray("{\"type\":0,\"data\":[128]}"),
             QByteArray("{\"type\":0,\"data\":[1.5]}"),QByteArray("{\"type\":0,\"data\":[\"1\"]}"),
             QByteArray("{\"type\":3}"),QByteArray("{" )+QByteArray(Collaboration::MaxWire,' ')}) {
            Collaboration::MessageReader reader; QList<QByteArray> packets;
            QVERIFY(!reader.append(wire,packets)); QVERIFY(!reader.error().isEmpty());
            QVERIFY(!reader.append("{\"type\":1}",packets));
        }
    }
    void playUsesNestedExtraString() {
        Collaboration::MessageReader reader; QList<QByteArray> packets;
        QVERIFY(reader.append(Collaboration::controlFrame("play",{{"width",1080},{"height",2400}},1),packets));
        QCOMPARE(packets.size(),1);
        const auto command=QJsonDocument::fromJson(packets[0]).object();
        QCOMPARE(command.value("action").toString(),QString("play"));
        QCOMPARE(QJsonDocument::fromJson(command.value("extra").toString().toUtf8()).object().value("width").toInt(),1080);
    }
};
QTEST_GUILESS_MAIN(CollaborationWireTest)
#include "collaboration_wire_test.moc"
