#include "collaboration_input.h"
#include <QtTest>
#include <QNetworkProxy>
#include <QJsonDocument>

namespace {
QString json(QJsonObject value) { return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)); }
}
class CollaborationInputTest:public QObject {
    Q_OBJECT
private slots:
    void officialHandshakeAndReconnect_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("mouse")<<int(CollaborationInput::Mouse);
        QTest::newRow("text")<<int(CollaborationInput::Text);
    }
    void officialHandshakeAndReconnect() {
        QFETCH(int,kind);
        QTcpServer server; server.setProxy(QNetworkProxy::NoProxy); QVERIFY(server.listen(QHostAddress::LocalHost));
        ServiceManager manager;
        QVERIFY(manager.start(QHostAddress::LocalHost,server.serverPort(),"phone","linux"));
        QTRY_VERIFY(server.hasPendingConnections()); std::unique_ptr<QTcpSocket> rpc(server.nextPendingConnection());
        QTRY_VERIFY(rpc->bytesAvailable()>0); rpc->readAll();
        rpc->write(DistBus::frame({{"action","connectDevice"},{"uuid","requestService"},
            {"profile",json({{"a","phone"},{"b","Phone"},{"c",4},{"p",0}})}}));
        QTRY_VERIFY(manager.ready());
        CollaborationInput input(manager,CollaborationInput::Kind(kind));
        QSignalSpy edits(&input,&CollaborationInput::editChanged),sizes(&input,&CollaborationInput::screenChanged);
        QVERIFY(input.prepare(QHostAddress::LocalHost,QHostAddress::LocalHost)); QVERIFY(input.publish());
        QTRY_VERIFY(rpc->bytesAvailable()>0); rpc->readAll();
        DistBus::RpcReader rpcReader;
        for(int cycle=0;cycle<2;++cycle) {
            rpc->write(DistBus::frame({{"action","distributebus.action.startservice"},{"uuid",input.uuid()},{"profile","null"}}));
            QList<QJsonObject> messages;
            QTRY_VERIFY(rpc->bytesAvailable()>0); QVERIFY(rpcReader.append(rpc->readAll(),messages));
            QCOMPARE(messages.size(),2);
            QCOMPARE(messages[0].value("action"),"distributebus.action.onstartservicecallback");
            const auto cb=QJsonDocument::fromJson(messages[0].value("profile").toString().toUtf8()).object();
            QVERIFY(cb.value("result").toBool());
            QCOMPARE(QJsonDocument::fromJson(cb.value("CallBackServiceInParam").toString().toUtf8()).object().value("type"),"distributebus.action.startservicecallback");
            QCOMPARE(QJsonDocument::fromJson(messages[1].value("profile").toString().toUtf8()).object().value("command"),1);
            rpc->write(DistBus::frame({{"action","distributebus.action.sendnotification"},{"uuid",input.uuid()},
                {"profile",json({{"command",2},{"profile","{\"v\":2,\"fillWifiParam\":1}"}})}}));
            messages.clear(); QTRY_VERIFY(rpc->bytesAvailable()>0); QVERIFY(rpcReader.append(rpc->readAll(),messages)); QCOMPARE(messages.size(),1);
            const auto outer=QJsonDocument::fromJson(messages[0].value("profile").toString().toUtf8()).object(); QCOMPARE(outer.value("command"),3);
            const auto connection=QJsonDocument::fromJson(outer.value("profile").toString().toUtf8()).object(); QCOMPARE(connection.value("type"),8);
            const auto endpoint=QJsonDocument::fromJson(connection.value("msgTransferProfile").toString().toUtf8()).object();
            QCOMPARE(endpoint.value("ip"),"127.0.0.1"); QVERIFY(endpoint.value("port").toInt()>0);
            QTcpSocket phone; phone.setProxy(QNetworkProxy::NoProxy); phone.connectToHost(QHostAddress::LocalHost,quint16(endpoint.value("port").toInt()));
            QTRY_COMPARE(phone.state(),QAbstractSocket::ConnectedState); QTRY_VERIFY(phone.bytesAvailable()>0);
            Collaboration::MessageReader reader; QList<QByteArray> payloads; QVERIFY(reader.append(phone.readAll(),payloads)); QCOMPARE(payloads.size(),1);
            QCOMPARE(QJsonDocument::fromJson(payloads[0]).object(),QJsonObject({{"type","MsgConnectAck"},{"result",true}}));
            // Genuine phone replies fragmented across reads, following an empty heartbeat.
            const auto response=Collaboration::payloadFrame(QJsonDocument(kind==CollaborationInput::Mouse ?
                QJsonObject{{"type","ScreenInfo"},{"point",QJsonObject{{"x",1080},{"y",2400}}}} :
                QJsonObject{{"type","EditInfo"},{"canEdit",true},{"status",1}}).toJson(QJsonDocument::Compact),17);
            phone.write("{\"type\":1,\"id\":16,\"data\":[]}"); phone.write(response.first(19)); phone.flush(); QTest::qWait(20);
            QVERIFY(!input.editable()); phone.write(response.sliced(19));
            QTRY_VERIFY(input.ready());
            if(kind==CollaborationInput::Mouse) QTRY_COMPARE(sizes.size(),cycle+1);
            else QTRY_VERIFY(input.editable());
            const QJsonObject body=kind==CollaborationInput::Text ? QJsonObject{{"type","InputTextInfo"},{"inputContent",QStringLiteral("努比亚测试")}} :
                QJsonObject{{"type","MotionEventInfo"},{"pointerId",-1},{"action",0},{"sourceX",540},{"sourceY",120},{"displayW",1080},{"displayH",2400}};
            QVERIFY(input.send(body)); payloads.clear(); QTRY_VERIFY(phone.bytesAvailable()>0); QVERIFY(reader.append(phone.readAll(),payloads));
            QCOMPARE(QJsonDocument::fromJson(payloads.last()).object(),body);
            rpc->write(DistBus::frame({{"action","distributebus.action.stopservice"},{"uuid",input.uuid()},{"profile","null"}}));
            QTRY_VERIFY(!input.ready()); QTRY_COMPARE(phone.state(),QAbstractSocket::UnconnectedState); QVERIFY(!input.send(body)); QVERIFY(!input.editable());
            messages.clear(); QTRY_VERIFY(rpc->bytesAvailable()>0); QVERIFY(rpcReader.append(rpc->readAll(),messages));
            QCOMPARE(messages.last().value("action"),"distributebus.action.onstopservicecallback");
        }
        input.close(); manager.stop();
    }
};
QTEST_GUILESS_MAIN(CollaborationInputTest)
#include "collaboration_input_test.moc"
