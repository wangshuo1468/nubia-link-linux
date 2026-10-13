#pragma once
#include <QByteArray>
#include <QObject>
#include <QPoint>
#include <QSize>

namespace Nubia {
constexpr quint32 MaxVideoPacket = 16 * 1024 * 1024;
constexpr qsizetype MaxText = 4092;
QByteArray text(quint8 type, const QString &value, qsizetype limit = MaxText);
QByteArray key(quint8 action, quint32 code, quint32 modifiers = 0);
QByteArray touch(quint8 action, QPoint position, QSize screen, quint16 pressure = 0xffff);
QByteArray scroll(QPoint position, QSize screen, qint32 horizontal, qint32 vertical);
QByteArray clipboard(const QString &value, bool paste);
}

class VideoFramer : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    void feed(const QByteArray &bytes);
    void reset();
signals:
    void packet(QByteArray payload, qint64 timestamp);
    void error(QString message);
private:
    QByteArray buffer_;
    bool failed_ = false;
};

class MessageFramer : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    void feed(const QByteArray &bytes);
    void reset();
signals:
    void message(quint8 type, QString text);
    void error(QString message);
private:
    QByteArray buffer_;
    bool failed_ = false;
};
