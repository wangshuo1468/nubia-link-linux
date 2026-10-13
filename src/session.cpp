#include "session.h"
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>

ProjectionSession::ProjectionSession(QObject *parent) : QObject(parent) {
    server_.setProxy(QNetworkProxy::NoProxy);
    connect(&server_, &QTcpServer::newConnection, this, &ProjectionSession::accept);
    connect(&video_, &VideoFramer::packet, this, &ProjectionSession::videoPacket);
    connect(&messages_, &MessageFramer::message, this, &ProjectionSession::deviceMessage);
    connect(&video_, &VideoFramer::error, this, &ProjectionSession::fail);
    connect(&messages_, &MessageFramer::error, this, &ProjectionSession::fail);
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] { fail(QStringLiteral("连接超时，请检查手机是否允许投屏")); });
}
ProjectionSession::~ProjectionSession() { disconnect(this, nullptr, nullptr, nullptr); stop(); }
bool ProjectionSession::listenLocal(quint16 port) {
    return listenNetwork(QHostAddress::LocalHost, QHostAddress::LocalHost, port);
}
bool ProjectionSession::listenNetwork(QHostAddress local, QHostAddress phone, quint16 port) {
    stop();
    if (local.protocol() != QAbstractSocket::IPv4Protocol || phone.protocol() != QAbstractSocket::IPv4Protocol ||
        !local.toIPv4Address() || !phone.toIPv4Address() || local.isMulticast() || phone.isMulticast() ||
        local == QHostAddress::Broadcast || phone == QHostAddress::Broadcast) {
        emit status(QStringLiteral("投屏需要有效的本机和手机 IPv4 地址")); return false;
    }
    expectedPeer_ = phone;
    if (!server_.listen(local, port)) {
        emit status(QStringLiteral("无法启动投屏监听：") + server_.errorString()); return false;
    }
    emit status(QStringLiteral("等待手机建立投屏通道"));
    deadline_.start(60000); return true;
}
void ProjectionSession::accept() {
    while (server_.hasPendingConnections()) {
        auto socket = server_.nextPendingConnection();
        if (channels_.size() >= 3 || socket->peerAddress() != expectedPeer_) {
            socket->abort(); socket->deleteLater(); continue;
        }
        int index = channels_.size(); channels_.append(socket);
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        socket->setReadBufferSize(index == 0 ? Nubia::MaxVideoPacket + 12 : 65536);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, index] {
            auto bytes = socket->readAll();
            if (index == 0) video_.feed(bytes);
            else if (index == 1) emit audioData(bytes);
            else messages_.feed(bytes);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this] {
            if (!stopping_) fail(QStringLiteral("手机已断开投屏"));
        });
        if (channels_.size() == 3) {
            deadline_.stop(); emit activeChanged(true);
            emit status(QStringLiteral("已连接，等待手机发送画面"));
        }
    }
}
void ProjectionSession::send(const QByteArray &bytes) {
    if (!connected()) return;
    auto socket = channels_[2];
    if (socket->bytesToWrite() > 65536) { fail(QStringLiteral("控制通道阻塞")); return; }
    socket->write(bytes);
}
void ProjectionSession::fail(const QString &message) { stop(); emit status(message); }
void ProjectionSession::adb(QStringList args, std::function<void(bool, QString)> done) {
    auto executable = QStandardPaths::findExecutable(QStringLiteral("adb"));
    if (executable.isEmpty()) { done(false, QStringLiteral("未找到 ADB")); return; }
    auto process = new QProcess(this); job_ = process;
    auto timer = new QTimer(process); timer->setSingleShot(true);
    auto generation = generation_;
    connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
    connect(process, &QProcess::finished, this, [this, process, timer, done, generation](int code, QProcess::ExitStatus state) {
        timer->stop();
        auto text = QString::fromUtf8(process->readAllStandardOutput()) + QString::fromUtf8(process->readAllStandardError());
        if (job_ == process) job_.clear();
        process->deleteLater();
        if (generation == generation_) done(code == 0 && state == QProcess::NormalExit, text.trimmed());
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, timer, done, generation](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        timer->stop(); auto message = process->errorString();
        if (job_ == process) job_.clear();
        process->deleteLater();
        if (generation == generation_) done(false, message);
    });
    process->start(executable, args); timer->start(10000);
}
void ProjectionSession::startAdb(const QString &serial, bool expanded) {
    if (!listenLocal()) return;
    serial_ = serial;
    emit status(QStringLiteral("正在连接手机原厂智慧投屏"));
    adb({"-s", serial_, "reverse", "--list"}, [this, expanded](bool ok, QString result) {
        if (!ok) { fail(result); return; }
        for (auto line : result.split('\n')) {
            if (line.split(' ', Qt::SkipEmptyParts).contains(QStringLiteral("localabstract:scrcpy"))) {
                fail(QStringLiteral("手机已有投屏通道，请先关闭其他投屏连接")); return;
            }
        }
        quint16 requestedPort = port();
        // Track before starting ADB: cancellation may happen after the server
        // creates the mapping but before the client reports success.
        ownedPort_ = requestedPort;
        adb({"-s", serial_, "reverse", "--no-rebind", "localabstract:scrcpy", QStringLiteral("tcp:%1").arg(requestedPort)},
            [this, expanded, requestedPort](bool ready, QString error) {
            if (!ready) { fail(error); return; }
            adb({"-s", serial_, "shell", "am", "start", "-n", "cn.nubia.touping/.WiredTouPingMainActivity",
                "--ez", "is_quick_mode", "true", "--ez", "is_nowireless", "true", "--ei", "maxFps", "60",
                "--ei", "height_level", expanded ? "1" : "2", "--ei", "bitrate", "20", "--ei", "connet_port", QString::number(requestedPort)},
                [this](bool started, QString error) {
                if (!started || error.contains("Error:")) { fail(error); return; }
                if (!connected()) emit status(QStringLiteral("请在手机上允许连接和投屏"));
            });
        });
    });
}
void ProjectionSession::removeReverse() {
    if (!ownedPort_ || serial_.isEmpty()) return;
    auto executable = QStandardPaths::findExecutable("adb");
    QProcess process;
    process.start(executable, {"-s", serial_, "reverse", "--list"});
    if (process.waitForFinished(1500)) {
        auto listing = QString::fromUtf8(process.readAllStandardOutput());
        for (auto line : listing.split('\n')) {
            auto fields = line.split(' ', Qt::SkipEmptyParts);
            if (fields.size() >= 2 && fields[fields.size()-2] == "localabstract:scrcpy" && fields.last() == QStringLiteral("tcp:%1").arg(ownedPort_)) {
                process.start(executable, {"-s", serial_, "reverse", "--remove", "localabstract:scrcpy"});
                if (!process.waitForFinished(1500)) { process.kill(); process.waitForFinished(); }
                break;
            }
        }
    } else { process.kill(); process.waitForFinished(); }
    ownedPort_ = 0;
}
void ProjectionSession::stop() {
    if (stopping_) return;
    stopping_ = true; ++generation_; deadline_.stop();
    if (job_) { job_->disconnect(this); job_->kill(); job_->deleteLater(); job_.clear(); }
    if (connected()) {
        send(Nubia::text(12, QStringLiteral("{\"set_mirrorclose_control\":1}")));
        channels_[2]->flush();
    }
    for (auto socket : channels_) { socket->disconnect(this); socket->abort(); socket->deleteLater(); }
    channels_.clear(); server_.close(); video_.reset(); messages_.reset();
    removeReverse(); serial_.clear(); stopping_ = false; emit activeChanged(false);
}
