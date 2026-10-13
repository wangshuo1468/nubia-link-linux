#pragma once
#include "service_manager.h"
#include "collaboration_wire.h"
#include <QTcpServer>
#include <QSize>

// Official type-8 NSTransfer, initiated by the PC after the phone starts
// its advertised service. Both channels share the authorized manager identity.
class CollaborationInput : public QObject {
    Q_OBJECT
public:
    enum Kind { Mouse, Text };
    CollaborationInput(ServiceManager &manager,Kind kind,QObject *parent=nullptr);
    ~CollaborationInput() override;
    bool prepare(QHostAddress local,QHostAddress phone);
    bool publish();
    void close();
    bool ready() const { return ready_; }
    bool editable() const { return ready_ && editable_; }
    bool send(QJsonObject payload);
    QString uuid() const;
signals:
    void readyChanged(bool ready);
    void editChanged(bool editable);
    void screenChanged(QSize size);
    void status(QString message);
private:
    void receive(QJsonObject rpc);
    void read();
    void resetConnection();
    ServiceManager &manager_;
    Kind kind_;
    QTcpServer server_;
    QTcpSocket *socket_=nullptr;
    Collaboration::MessageReader reader_;
    QTimer deadline_,heartbeat_;
    QHostAddress local_,phone_;
    int id_=0;
    bool prepared_=false,started_=false,accepted_=false,ready_=false,editable_=false;
};
