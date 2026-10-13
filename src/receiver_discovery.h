#pragma once
#include <QObject>
#include <QHostAddress>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QTimer>
#include <coap3/coap.h>
#include "device_role.h"

// Original DistBus multicast discovery, shared with the existing libcoap build.
// Advertising services only makes a receiver visible; it grants no consent.
class ReceiverDiscovery : public QObject {
    Q_OBJECT
public:
    explicit ReceiverDiscovery(QObject *parent=nullptr);
    ~ReceiverDiscovery() override;
    bool start(QHostAddress local,int prefix,QString interfaceName,quint16 managerPort,QJsonArray services,quint16 discoveryPort=25683,DesktopRole role=DesktopRole::SmartCast);
    void stop();
    QString localId() const { return id_; }
    QJsonObject peer(QHostAddress address) const;
    QHostAddress addressForId(QString id) const;
    quint16 port() const { return port_; }
signals:
    void error(QString reason);
    void peerFound(QHostAddress address, QJsonObject metadata);
private:
    static void receive(coap_resource_t *,coap_session_t *,const coap_pdu_t *,const coap_string_t *,coap_pdu_t *);
    bool send(QHostAddress address,quint16 port,const QJsonObject &metadata);
    void announce();
    QJsonObject metadata(bool multicast) const;
    coap_context_t *context_=nullptr;
    QMap<QString,coap_session_t *> clients_;
    QMap<QString,QJsonObject> peers_;
    QMap<QString,qint64> seen_;
    QTimer pump_,announce_;
    QHostAddress local_;
    QString id_;
    QJsonArray services_;
    DesktopRole role_=DesktopRole::SmartCast;
    int prefix_=0;
    quint16 port_=0,managerPort_=0;
    quint64 generation_=0;
};
