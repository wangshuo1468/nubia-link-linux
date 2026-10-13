#include "collaboration_session.h"
#include <QtTest>
#include <QTcpServer>
#include <QNetworkProxy>
#include <QJsonDocument>

class CollaborationLifecycleTest:public QObject {
    Q_OBJECT
private slots:
    void applicationReplacementKeepsAssociationAndTransport() {
        const auto json=[](QJsonObject object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); };
        QTcpServer server,controlServer;
        QVERIFY(server.listen(QHostAddress::LocalHost)); QVERIFY(controlServer.listen(QHostAddress::LocalHost));
        CollaborationSession session; auto &manager=session.link_.manager();
        QVERIFY(manager.start(QHostAddress::LocalHost,server.serverPort(),"phone","linux",DesktopRole::Collaboration));
        QTRY_VERIFY(server.hasPendingConnections()); std::unique_ptr<QTcpSocket> peer(server.nextPendingConnection());
        QTRY_VERIFY(peer->bytesAvailable()>0); peer->readAll();
        peer->write(DistBus::frame({{"action","connectDevice"},{"uuid","requestService"},
            {"profile",json({{"a","phone"},{"b","Phone"},{"c",4},{"p",0}})}})+
            DistBus::frame({{"action","distributebus.action.publishservice"},{"profile",json({
                {"serviceUuid","187f8000-0000-1000-8000-ceb574815101"},{"serviceName","MultiScrService"},{"enable",true},{"authType",0}})}}));
        QTRY_VERIFY(manager.ready() && !manager.services().isEmpty());
        session.control_.connectToHost(QHostAddress::LocalHost,controlServer.serverPort());
        QTRY_VERIFY(controlServer.hasPendingConnections()); std::unique_ptr<QTcpSocket> control(controlServer.nextPendingConnection());
        QTRY_COMPARE(session.control_.state(),QAbstractSocket::ConnectedState);
        session.state_=CollaborationSession::Playing; session.associated_=true; session.incoming_=true;
        session.local_=session.phone_=QHostAddress::LocalHost; session.port_=8600; session.transportReceived_=true;
        session.offered_=true; session.displayType_="mirror";
        QVERIFY(!session.requestApplication("bad package")); QCOMPARE(session.state_,CollaborationSession::Playing);
        QVERIFY(session.requestApplication("com.android.settings"));
        QCOMPARE(session.state_,CollaborationSession::Offering); QVERIFY(session.associated_); QVERIFY(manager.ready());
        QCOMPARE(session.port_,quint16(8600)); QVERIFY(session.transportReceived_);
        QCOMPARE(session.control_.state(),QAbstractSocket::ConnectedState);
        QTRY_VERIFY(peer->bytesAvailable()>6); DistBus::RpcReader reader; QList<QJsonObject> messages;
        QVERIFY(reader.append(peer->readAll(),messages)); QCOMPARE(messages.size(),1);
        QCOMPARE(messages[0].value("action"),QJsonValue("distributebus.action.startservice"));
        QCOMPARE(messages[0].value("uuid"),QJsonValue("187f8000-0000-1000-8000-ceb574815101"));
        session.acceptOffer({{"profile",json({{"castDisplayType","virtualDisplay"},{"pkg","com.android.settings"},
            {"profile",json({{"abilities",3}})}})}});
        QCOMPARE(session.state_,CollaborationSession::Transport); QCOMPARE(session.control_.state(),QAbstractSocket::ConnectedState);
        QTRY_VERIFY(peer->bytesAvailable()>6); messages.clear(); QVERIFY(reader.append(peer->readAll(),messages));
        QCOMPARE(messages.size(),1);
        const auto callback=QJsonDocument::fromJson(messages[0].value("profile").toString().toUtf8()).object();
        const auto nativeAnswer=QJsonDocument::fromJson(callback.value("CallBackServiceInParam").toString().toUtf8()).object();
        QVERIFY(nativeAnswer.value("isCasting").toBool()); QVERIFY(session.associated_);
        QTRY_VERIFY_WITH_TIMEOUT(control->bytesAvailable()>0,2000);
        Collaboration::MessageReader controls; QList<QByteArray> commands;
        QVERIFY(controls.append(control->readAll(),commands)); QVERIFY(!commands.isEmpty());
        QCOMPARE(QJsonDocument::fromJson(commands[0]).object().value("action"),QJsonValue("play"));
        session.associated_=false; session.stop(); QVERIFY(!session.running());
    }
    void replacementRejectsMirrorAndWrongPackage_data() {
        QTest::addColumn<QString>("type"); QTest::addColumn<QString>("package");
        QTest::newRow("mirror-in-place-of-application")<<QString("mirror")<<QString("com.android.settings");
        QTest::newRow("different-application")<<QString("virtualDisplay")<<QString("com.other.application");
    }
    void replacementRejectsMirrorAndWrongPackage() {
        QFETCH(QString,type); QFETCH(QString,package);
        CollaborationSession session; session.state_=CollaborationSession::Offering; session.incoming_=true;
        session.package_="com.android.settings";
        const auto inner=QString::fromUtf8(QJsonDocument(QJsonObject{{"abilities",3}}).toJson(QJsonDocument::Compact));
        session.acceptOffer({{"profile",QString::fromUtf8(QJsonDocument(QJsonObject{{"castDisplayType",type},{"pkg",package},{"profile",inner}}).toJson(QJsonDocument::Compact))}});
        QVERIFY(!session.offered_); QVERIFY(!session.running());
    }
    void offerBeforeWlanIdentityConnectsWhenReady() {
        const auto json=[](QJsonObject object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); };
        QTcpServer server; server.setProxy(QNetworkProxy::NoProxy); QVERIFY(server.listen(QHostAddress::LocalHost));
        CollaborationSession session; auto &manager=session.link_.manager();
        QVERIFY(manager.start(QHostAddress::LocalHost,server.serverPort(),"phone","linux",DesktopRole::Collaboration));
        QTRY_VERIFY(server.hasPendingConnections()); std::unique_ptr<QTcpSocket> peer(server.nextPendingConnection());
        QTRY_VERIFY(peer->bytesAvailable()>0); peer->readAll();
        peer->write(DistBus::frame({{"action","connectDevice"},{"uuid","requestService"},
            {"profile",json({{"a","phone"},{"b","Phone"},{"c",4},{"p",0}})}}));
        QTRY_VERIFY(manager.ready());
        session.state_=CollaborationSession::Discovering; session.incoming_=true;
        peer->write(DistBus::frame({{"action","distributebus.action.startservice"},
            {"uuid","187f8000-0000-1000-8000-ceb574815100"},
            {"profile",json({{"castDisplayType","mirror"},{"profile",json({{"abilities",3},{"type",4}})}})}}));
        QTRY_COMPARE(session.earlyRequests_.size(),1);
        QCOMPARE(peer->bytesAvailable(),qint64(0)); QVERIFY(!session.offered_); QVERIFY(session.mediaSockets_.isEmpty());
        QVERIFY(session.mouse_.prepare(QHostAddress::LocalHost,QHostAddress::LocalHost));
        QVERIFY(session.text_.prepare(QHostAddress::LocalHost,QHostAddress::LocalHost));
        session.publish(); QVERIFY(session.earlyRequests_.isEmpty());
        QVERIFY(session.offered_); QCOMPARE(session.state_,CollaborationSession::Transport);
        QTRY_VERIFY(peer->bytesAvailable()>6);
        DistBus::RpcReader reader; QList<QJsonObject> messages; QVERIFY(reader.append(peer->readAll(),messages));
        bool accepted=false;
        for(const auto &message:messages) if(message.value("action")=="distributebus.action.onstartservicecallback") {
            const auto callback=QJsonDocument::fromJson(message.value("profile").toString().toUtf8()).object();
            accepted=callback.value("result").toBool();
        }
        QVERIFY(accepted); QVERIFY(session.mediaSockets_.isEmpty());
        session.stop(); QTRY_VERIFY(!session.running());
    }
    void independentOfferThenNormalMediaStop() {
        const QString relay="187f8000-0000-1000-8000-ceb574815500",sink="187f8000-0000-1000-8000-ceb574815100";
        const auto json=[](QJsonObject object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); };
        QTcpServer server; server.setProxy(QNetworkProxy::NoProxy); QVERIFY(server.listen(QHostAddress::LocalHost));
        CollaborationSession session;
        auto &manager=session.link_.manager();
        QVERIFY(manager.start(QHostAddress::LocalHost,server.serverPort(),"phone","linux"));
        QTRY_VERIFY(server.hasPendingConnections()); std::unique_ptr<QTcpSocket> peer(server.nextPendingConnection());
        QTRY_VERIFY(peer->bytesAvailable()>0); peer->readAll();
        peer->write(DistBus::frame({{"action","connectDevice"},{"uuid","requestService"},
            {"profile",json({{"a","phone"},{"b","Phone"},{"c",4},{"p",0}})}}));
        QTRY_VERIFY(manager.ready());
        session.state_=CollaborationSession::Offering; session.associated_=true;
        session.package_="com.android.settings";
        DistBus::RpcReader reader; QList<QJsonObject> messages;
        peer->write(DistBus::frame({{"action","distributebus.action.startservice"},{"uuid",sink},
            {"profile",json({{"castDisplayType","virtualDisplay"},{"pkg","com.android.settings"},
                {"profile",json({{"abilities",3},{"type",4}})}})}}));
        QTRY_VERIFY(peer->bytesAvailable()>6);
        messages.clear(); QVERIFY(reader.append(peer->readAll(),messages));
        QCOMPARE(messages.size(),1); QCOMPARE(messages[0].value("action"),"distributebus.action.onstartservicecallback");
        const auto callback=QJsonDocument::fromJson(messages[0].value("profile").toString().toUtf8()).object();
        QVERIFY(callback.value("result").toBool());
        const auto answer=QJsonDocument::fromJson(callback.value("CallBackServiceInParam").toString().toUtf8()).object();
        QCOMPARE(QJsonDocument::fromJson(answer.value("profile").toString().toUtf8()).object().value("type"),2);
        QVERIFY(session.offered_);
        QCOMPARE(answer.value("deviceModel").toString(),QString("7"));
        // An explicit media stop still closes the offered application window.
        peer->write(DistBus::frame({{"action","distributebus.action.stopservice"},{"uuid",sink},{"profile",""}}));
        QTRY_COMPARE(session.state_,CollaborationSession::Stopping);
        QTRY_VERIFY(peer->bytesAvailable()>6); messages.clear(); QVERIFY(reader.append(peer->readAll(),messages));
        bool disconnectSent=false;
        for(const auto &message:messages) if(message.value("action")=="distributebus.action.stopservice" &&
            message.value("uuid")==relay && message.value("profile")=="disconnect") disconnectSent=true;
        QVERIFY(disconnectSent);
        peer->write(DistBus::frame({{"action","distributebus.action.onstopservicecallback"},{"uuid",relay},
            {"profile",json({{"result",true},{"CallBackServiceInParam","disconnect"}})}}));
        QTRY_VERIFY(!session.running()); QTRY_COMPARE(peer->state(),QAbstractSocket::UnconnectedState);
    }
    void relayDisconnectEndsIndependentSession() {
        const QString relay="187f8000-0000-1000-8000-ceb574815500";
        const auto json=[](QJsonObject object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); };
        QTcpServer server; server.setProxy(QNetworkProxy::NoProxy); QVERIFY(server.listen(QHostAddress::LocalHost));
        CollaborationSession session; auto &manager=session.link_.manager();
        QVERIFY(manager.start(QHostAddress::LocalHost,server.serverPort(),"phone","linux",DesktopRole::Collaboration));
        QTRY_VERIFY(server.hasPendingConnections()); std::unique_ptr<QTcpSocket> peer(server.nextPendingConnection());
        QTRY_VERIFY(peer->bytesAvailable()>0); peer->readAll();
        peer->write(DistBus::frame({{"action","connectDevice"},{"uuid","requestService"},
            {"profile",json({{"a","phone"},{"b","Phone"},{"c",4},{"p",0}})}}));
        QTRY_VERIFY(manager.ready());
        session.state_=CollaborationSession::Offering; session.associated_=true;
        peer->write(DistBus::frame({{"action","distributebus.action.stopservice"},{"uuid",relay},{"profile","disconnect"}}));
        QTRY_VERIFY(!session.running()); QTRY_COMPARE(peer->state(),QAbstractSocket::UnconnectedState);
    }

};
QTEST_GUILESS_MAIN(CollaborationLifecycleTest)
#include "collaboration_lifecycle_test.moc"
