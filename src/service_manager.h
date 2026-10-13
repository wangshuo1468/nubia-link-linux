#pragma once
#include <QObject>
#include <QJsonObject>
#include <QMap>
#include <QTcpSocket>
#include <QTimer>
#include <QSocketNotifier>
#include "device_role.h"

namespace DistBus {
constexpr qsizetype MaxRpc = 256 * 1024;
QByteArray frame(const QJsonObject &message);
// Incremental official version-1 TCP framing. Errors are terminal until reset.
class RpcReader {
public:
    bool append(QByteArrayView bytes, QList<QJsonObject> &messages);
    QString error() const { return error_; }
    void reset() { buffer_.clear(); error_.clear(); }
private:
    QByteArray buffer_;
    QString error_;
};
}

// Service-manager connection, not application pairing or a media connection.
// Caller must use the endpoint and identity returned by official discovery.
class ServiceManager : public QObject {
    Q_OBJECT
public:
    explicit ServiceManager(QObject *parent=nullptr);
    ~ServiceManager() override;
    bool start(QHostAddress phone, quint16 port, QString phoneId, QString localId,DesktopRole role=DesktopRole::SmartCast);
    // Takes ownership of a discovered phone's incoming management socket.
    bool accept(QTcpSocket *socket, QString phoneId, QString localId,DesktopRole role=DesktopRole::SmartCast);
    // BlueZ Profile1 passes an already connected RFCOMM stream, never TCP.
    // Takes ownership even on failure; discovery must supply the peer identity.
    bool acceptRfcomm(int descriptor,QString phoneId,QString localId,QHostAddress local,QString bluetoothAddress,bool incoming=true);
    void stop();
    bool ready() const { return ready_; }
    QList<QJsonObject> services() const { return services_.values(); }
    bool send(QString action, QString uuid, const QJsonObject &profile);
    // DFS and AppRelay use plain string profiles, not embedded JSON objects.
    bool sendProfile(QString action, QString uuid, QString profile);
    void flush();
signals:
    void connected(QString name);
    void servicePublished(QJsonObject service);
    void message(QJsonObject rpc);
    void error(QString reason);
    void disconnected();
private:
    void receive(const QJsonObject &rpc);
    void fail(QString reason);
    bool write(const QJsonObject &rpc);
    void attach(QTcpSocket *socket);
    bool configureSocket();
    bool hello(QString uuid);
    void readRfcomm();
    void drainRfcomm();
    void receiveBytes(QByteArray bytes);
    int rfcomm_=-1;
    QSocketNotifier *rfcommRead_=nullptr,*rfcommWrite_=nullptr;
    QByteArray rfcommQueue_;
    QHostAddress bluetoothLocal_;
    QString bluetoothAddress_;
    QTcpSocket *socket_;
    QTimer timeout_;
    DistBus::RpcReader reader_;
    QMap<QString,QJsonObject> services_;
    QString phoneId_,localId_;
    DesktopRole role_=DesktopRole::SmartCast;
    QList<QJsonObject> pendingMessages_;
    qsizetype pendingBytes_=0;
    bool encrypted_=false;
    quint64 generation_=0;
    bool ready_=false, running_=false;
    bool incoming_=false;
};
