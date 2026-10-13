#pragma once
#include "dfs.h"
#include "discovery.h"
#include "service_manager.h"

// Dedicated file session so browsing never replaces an active cast identity.
class FileSession : public QObject {
    Q_OBJECT
public:
    explicit FileSession(QObject *parent=nullptr);
    ~FileSession() override;
    bool start(QHostAddress local,QHostAddress phone);
    void stop();
    DfsClient &client() { return client_; }
    bool running() const { return running_; }
signals:
    void status(QString message);
    void available(bool available);
private:
    void request();
    void fail(QString reason);
    void release();
    PhoneDiscovery discovery_;
    ServiceManager manager_;
    DfsClient client_;
    QTimer deadline_,connectDelay_,cleanup_;
    QHostAddress phone_;
    bool running_=false,requested_=false,acknowledged_=false,managerStarted_=false;
};
