#include "ble_discovery.h"
#include "rfcomm_connector.h"
#include <bluetooth/sdp_lib.h>
#include <QtTest>
#include <QJsonObject>
#include <unistd.h>
class BleDiscoveryTest : public QObject {
    Q_OBJECT
private slots:
    void packets() {
        const auto identity=NubiaBle::identity("0123456789ab");
        QCOMPARE(identity,QByteArray::fromHex("0123456789ab"));
        QVERIFY(NubiaBle::identity("bad-id").isEmpty());
        // Android BleScanner parses these exact offsets in the concatenated
        // legacy advertising and scan-response records (31 + 31 bytes).
        auto raw=QByteArray::fromHex("020106110700508174b5ce00800010000000807f18");
        raw+=QByteArray::fromHex("09165000")+identity;
        QCOMPARE(raw.size(),31);
        raw+=QByteArray::fromHex("1e16")+NubiaBle::response("Nubia Link Linux",7,1);
        QCOMPARE(raw.size(),62); QCOMPARE(quint8(raw[23]),quint8(0x50));
        QCOMPARE(raw.mid(25,6),identity); QCOMPARE(quint8(raw[33]),quint8(1));
        QCOMPARE(quint8(raw[34]),quint8(7)); QCOMPARE(quint8(raw[35]),quint8(0));
        QCOMPARE(raw.mid(44,16).split('\0').first(),QByteArray("Nubia Link Linux"));
        QCOMPARE(quint8(raw[60]),quint8(1)); QCOMPARE(quint8(raw[61]),quint8(1));
    }
    void duplicateUuidProtocolSelection() {
        for(bool rfcomm:{false,true}) {
            auto record=sdp_record_alloc(); QVERIFY(record);
            uuid_t service{},l2cap{},transport{};
            sdp_uuid128_create(&service,QByteArray::fromHex("187f8000000010008000ceb574815000").constData());
            auto services=sdp_list_append(nullptr,&service);
            QCOMPARE(sdp_set_service_classes(record,services),0);
            sdp_uuid16_create(&l2cap,L2CAP_UUID);
            auto first=sdp_list_append(nullptr,&l2cap);
            uint16_t psm=31; auto attPsm=sdp_data_alloc(SDP_UINT16,&psm);
            if(!rfcomm) first=sdp_list_append(first,attPsm);
            sdp_uuid16_create(&transport,rfcomm ? RFCOMM_UUID : ATT_UUID);
            auto second=sdp_list_append(nullptr,&transport);
            uint8_t port=13; auto channel=sdp_data_alloc(SDP_UINT8,&port);
            if(rfcomm) second=sdp_list_append(second,channel);
            auto protocols=sdp_list_append(nullptr,first); protocols=sdp_list_append(protocols,second);
            auto access=sdp_list_append(nullptr,protocols);
            QCOMPARE(sdp_set_access_protos(record,access),0);
            QCOMPARE(NubiaRfcomm::channel(record),rfcomm ? 13 : 0);
            sdp_list_free(services,nullptr); sdp_list_free(access,nullptr); sdp_list_free(protocols,nullptr);
            sdp_list_free(first,nullptr); sdp_list_free(second,nullptr);
            sdp_data_free(attPsm); sdp_data_free(channel); sdp_record_free(record);
        }
        QCOMPARE(NubiaRfcomm::channel(nullptr),0);
    }
    void nativeManagementDevice() {
        const auto phone=qEnvironmentVariable("NUBIA_TEST_BLUETOOTH_PHONE"),local=qEnvironmentVariable("NUBIA_TEST_BLUETOOTH_LOCAL");
        if(phone.isEmpty() || local.isEmpty()) QSKIP("Explicit opt-in for original phone RFCOMM service");
        RfcommConnector connector; QSignalSpy connected(&connector,&RfcommConnector::connected),errors(&connector,&RfcommConnector::error);
        connect(&connector,&RfcommConnector::connected,this,[](int fd) { ::close(fd); });
        connect(&connector,&RfcommConnector::error,this,[](QString reason) { qWarning().noquote()<<reason; });
        QVERIFY(connector.start(local,phone));
        QTRY_VERIFY_WITH_TIMEOUT(!connected.isEmpty() || !errors.isEmpty(),25000);
        QVERIFY(errors.isEmpty()); QCOMPARE(connected.size(),1); connector.stop();
    }
    void deviceDiscovery() {
        if(!qEnvironmentVariableIsSet("NUBIA_TEST_BLE")) QSKIP("Explicit opt-in for original phone BLE discovery");
        BleDiscovery discovery; QSignalSpy scanned(&discovery,&BleDiscovery::peerScanned),errors(&discovery,&BleDiscovery::error);
        connect(&discovery,&BleDiscovery::status,this,[](QString s){ qInfo().noquote()<<s; });
        connect(&discovery,&BleDiscovery::error,this,[](QString s){ qWarning().noquote()<<s; });
        connect(&discovery,&BleDiscovery::managementSocket,this,[](int fd,QString path){ qInfo()<<"Original RFCOMM connection"<<path; ::close(fd); });
        const QJsonArray services{
            QJsonObject{{"serviceName","MultiScrService"},{"serviceUUid","187f8000-0000-1000-8000-ceb574815100"},{"version",9},{"custData","{}"}},
            QJsonObject{{"serviceName","RELAY_SERVICE"},{"serviceUUid","187f8000-0000-1000-8000-ceb574815500"},{"version",22},{"custData","{}"}}
        };
        QVERIFY(discovery.start(NubiaBle::desktopId(),services));
        QElapsedTimer timer; timer.start();
        while(scanned.isEmpty() && errors.isEmpty() && timer.elapsed()<90000) QTest::qWait(100);
        QVERIFY2(errors.isEmpty(),"BlueZ registration failed");
        QVERIFY2(!scanned.isEmpty(),"No original phone GATT service scan received");
        qInfo()<<"Original phone GATT service discovery"<<scanned.first();
        QTest::qWait(15000); discovery.stop();
    }
};
QTEST_GUILESS_MAIN(BleDiscoveryTest)
#include "ble_discovery_test.moc"
