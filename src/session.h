#pragma once
#include "protocol.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QProcess>
#include <QTimer>
#include <QPointer>
#include <functional>

class ProjectionSession : public QObject {
    Q_OBJECT
public:
    explicit ProjectionSession(QObject *parent = nullptr);
    ~ProjectionSession() override;
    bool listenLocal(quint16 port = 0);
    bool listenNetwork(QHostAddress local, QHostAddress phone, quint16 port = 0);
    quint16 port() const { return server_.serverPort(); }
    bool connected() const { return channels_.size() == 3; }
    void startAdb(const QString &serial, bool expanded = false);
    void stop();
    void send(const QByteArray &bytes);
signals:
    void status(QString text);
    void activeChanged(bool active);
    void videoPacket(QByteArray bytes, qint64 timestamp);
    void audioData(QByteArray bytes);
    void deviceMessage(quint8 type, QString value);
private:
    void accept();
    void fail(const QString &message);
    void adb(QStringList args, std::function<void(bool, QString)> done);
    void removeReverse();
    QTcpServer server_;
    QHostAddress expectedPeer_;
    QList<QTcpSocket *> channels_;
    VideoFramer video_;
    MessageFramer messages_;
    QTimer deadline_;
    QPointer<QProcess> job_;
    QString serial_;
    quint16 ownedPort_ = 0;
    bool stopping_ = false;
    quint64 generation_ = 0;
};
