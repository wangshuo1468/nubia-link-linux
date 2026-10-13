#pragma once
#include <QObject>
#include <QHostAddress>
#include <QJsonObject>
#include <QTimer>
#include <coap3/coap.h>
#include "device_role.h"

// Official DistBus discovery, using libcoap rather than a custom CoAP stack.
// Discovery alone neither authenticates a peer nor starts collaboration.
class PhoneDiscovery : public QObject {
    Q_OBJECT
public:
    explicit PhoneDiscovery(QObject *parent=nullptr);
    ~PhoneDiscovery() override;
    bool start(QHostAddress local, QHostAddress phone, quint16 phonePort=25683, DesktopRole role=DesktopRole::SmartCast);
    void stop();
    quint16 port() const { return port_; }
    QString localId() const { return localId_; }
signals:
    void found(QJsonObject device);
    void error(QString message);
private:
    static void receive(coap_resource_t *, coap_session_t *, const coap_pdu_t *, const coap_string_t *, coap_pdu_t *);
    coap_context_t *context_=nullptr;
    coap_session_t *session_=nullptr;
    QHostAddress phone_;
    QString localId_;
    quint64 generation_=0;
    quint16 port_=0;
    QTimer pump_,deadline_;
};
