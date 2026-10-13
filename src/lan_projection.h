#pragma once
#include "discovery.h"
#include "service_manager.h"
#include "session.h"
#include "receiver_discovery.h"
#include <QTcpServer>

// Original RMCast LAN entry. A manager handshake does not grant permission;
// the phone still presents its own connection and MediaProjection dialogs.
class LanProjection : public QObject {
    Q_OBJECT
public:
    explicit LanProjection(ProjectionSession &media, QObject *parent=nullptr);
    ~LanProjection() override;
    bool start(QHostAddress local, QHostAddress phone, quint16 discoveryPort=25683);
    bool listen(QHostAddress local,int prefix,QString interfaceName,quint16 discoveryPort=25683);
    QHostAddress incomingPeer() const { return phone_; }
    bool receiving() const { return incoming_; }
    void stop();
    bool running() const { return running_; }
    // Extensions must use the identity trusted by the normal casting flow.
    ServiceManager &serviceManager() { return manager_; }
    QString localId() const { return incoming_ ? receiver_.localId() : discovery_.localId(); }
    // Explicit handover mode: ending RMCast must not close the shared control
    // transport while a collaboration extension is negotiating its successor.
    void preserveControlForHandover(bool enabled) { preserveControl_=enabled; }
signals:
    void status(QString text);
    void incomingPeerChanged(QHostAddress phone);
private:
    void request();
    void receive(const QJsonObject &rpc);
    void fail(QString reason);
    ProjectionSession &media_;
    PhoneDiscovery discovery_;
    ServiceManager manager_;
    ReceiverDiscovery receiver_;
    QTcpServer listener_;
    QTimer deadline_, audioStart_;
    QHostAddress local_, phone_;
    bool running_=false, managerStarted_=false, requested_=false, replied_=false, audioRequested_=false;
    bool preserveControl_=false;
    bool incoming_=false;
    QString peerName_;
};
