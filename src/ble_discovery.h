#pragma once
#include <QObject>
#include <QJsonArray>
#include <memory>

namespace NubiaBle {
// Original legacy advertisement: flags, 128-bit service, 16-bit identity
// service data, then a separate 31-byte scan response. The ROM reads fixed
// offsets across these two packets, so an extended advertisement is unsuitable.
QByteArray identity(QString deviceId);
QByteArray response(QString name,quint8 role,quint8 serviceTag=1);
QString desktopId();
}
class BleDiscovery : public QObject {
    Q_OBJECT
public:
    explicit BleDiscovery(QObject *parent=nullptr);
    ~BleDiscovery() override;
    bool start(QString id,QJsonArray services);
    // Select the phone identity received on the chosen WLAN. Scan its
    // original BLE/GATT services, then connect its RFCOMM management profile.
    bool connectPeer(QString phoneId);
    void stop();
    QString bluetoothAddress() const;
signals:
    void status(QString text);
    void error(QString text);
    void peerScanned(QString id,QString name,QString bluetoothPath);
    void managementSocket(int descriptor,QString bluetoothPath);
    void managementDisconnected(QString bluetoothPath);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
