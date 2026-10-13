#pragma once
#include "discovery.h"
#include "receiver_discovery.h"
#include "service_manager.h"
#include "ble_discovery.h"
#include <QTcpServer>

// Discovery and service management only. No RMCast, ADB or video bootstrap.
// The collaboration session owns consent, association and media negotiation.
class CollaborationLink : public QObject {
    Q_OBJECT
public:
    explicit CollaborationLink(QObject *parent=nullptr);
    ~CollaborationLink() override;
    bool start(QHostAddress local,QHostAddress phone,quint16 discoveryPort=25683);
    bool listen(QHostAddress local,int prefix,QString interfaceName,quint16 discoveryPort=25683);
    void stop();
    ServiceManager &manager() { return manager_; }
    bool running() const { return running_; }
signals:
    void ready(QHostAddress local,QHostAddress phone,QString name);
    void error(QString reason);
    void disconnected();
    void status(QString text);
private:
    void fail(QString reason);
    void completeReady();
    BleDiscovery bluetooth_;
    QMap<QString,QString> blePeers_;
    QString blePath_,blePeerId_,peerName_;
    int prefix_=0;
    bool readyEmitted_=false;
    PhoneDiscovery discovery_;
    ReceiverDiscovery receiver_;
    ServiceManager manager_;
    QTcpServer listener_;
    QHostAddress local_,phone_;
    bool running_=false,incoming_=false,managerStarted_=false;
};
