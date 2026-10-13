#pragma once
#include <QObject>
#include <memory>
#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
namespace NubiaRfcomm {
// Select RFCOMM explicitly: this ROM also publishes an ATT/GATT record with
// exactly the same service UUID, which is not a management byte stream.
int channel(const sdp_record_t *record);
}
class RfcommConnector : public QObject {
    Q_OBJECT
public:
    explicit RfcommConnector(QObject *parent=nullptr);
    ~RfcommConnector() override;
    bool start(QString localAddress,QString phoneAddress);
    void stop();
signals:
    void connected(int descriptor);
    void error(QString message);
private:
    struct Job;
    std::shared_ptr<Job> job_;
};
