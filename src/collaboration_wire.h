#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace Collaboration {
constexpr qsizetype MaxPayload=256*1024;
constexpr qsizetype MaxWire=MaxPayload*5+1024;
// Official TYPE_MESSAGE_HIGH is TCP MsgData JSON, with Java signed byte arrays.
QByteArray payloadFrame(QByteArrayView payload, qint64 id);
QByteArray controlFrame(QString action, QJsonObject extra, int id);
class MessageReader {
public:
    bool append(QByteArrayView bytes, QList<QByteArray> &payloads);
    QString error() const { return error_; }
    void reset();
private:
    bool consume(QList<QByteArray> &payloads);
    QByteArray buffer_;
    QString error_;
    int depth_=0;
    bool quoted_=false, escaped_=false;
};
}
