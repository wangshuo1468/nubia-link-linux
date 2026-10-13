#pragma once
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QFile>
#include <QSaveFile>
#include <functional>
#include <memory>

namespace Dfs {
constexpr qsizetype MaxPayload=2*1024*1024;
struct Packet { quint8 type; quint32 id; QByteArray payload; };
struct Entry { QString name; quint32 mode=0; qint64 size=0, modified=0; bool directory() const { return (mode&0170000)==0040000; } bool regular() const { return (mode&0170000)==0100000; } };
QByteArray u32(quint32 value);
QByteArray u64(quint64 value);
QByteArray string(QByteArrayView value);
QByteArray frame(quint8 type,quint32 id,QByteArrayView payload);
bool attributes(QByteArrayView bytes,Entry &entry);
bool entries(QByteArrayView bytes,QList<Entry> &result);
bool validPath(const QString &relative);
class Reader {
public:
    bool append(QByteArrayView bytes,QList<Packet> &packets);
    void reset() { buffer_.clear(); failed_=false; }
private:
    QByteArray buffer_; bool failed_=false;
};
}

// Native DFS wire, not SFTP. One request/transfer at a time, bounded chunks.
// Upload uses Android O_EXCL so an existing phone file is never overwritten.
class DfsClient : public QObject {
    Q_OBJECT
public:
    using Reply=std::function<void(const Dfs::Packet &)>;
    explicit DfsClient(QObject *parent=nullptr);
    ~DfsClient() override;
    enum class Root { Virtual, SharedStorage };
    void start(QHostAddress phone,quint16 port,QString localDeviceId,Root root=Root::Virtual);
    void stop();
    bool ready() const { return ready_; }
    bool busy() const { return busy_; }
    bool list(const QString &path);
    bool upload(const QString &localFile,const QString &remotePath);
    bool download(const QString &remotePath,const QString &localFile);
    // Low-level requests used by opt-in protocol probes, never concurrent.
    bool request(quint8 type,QByteArray payload,Reply callback);
signals:
    void connected();
    void disconnected();
    void error(QString reason);
    void listing(QString path,QList<Dfs::Entry> entries);
    void progress(qint64 bytes,qint64 total);
    void completed(QString path);
    void operationFailed(QString reason);
    void busyChanged(bool busy);
private:
    void fail(QString reason);
    void setBusy(bool value);
    void operationError(QString reason);
    void closeHandle(std::function<void(bool)> next);
    void readDirectory();
    void writeChunk();
    void readChunk();
    void completeTransfer();
    bool expect(const Dfs::Packet &packet,quint8 type);
    QString statusError(const Dfs::Packet &packet) const;
    QByteArray path(const QString &relative) const;
    QTcpSocket socket_;
    QTimer timeout_;
    Dfs::Reader reader_;
    Reply pending_;
    quint32 nextId_=2,pendingId_=0,handle_=0;
    quint32 chunkSize_=64*1024;
    QString deviceId_,remotePath_,root_;
    bool ready_=false,running_=false,busy_=false,hasHandle_=false,uploading_=false,created_=false;
    qint64 offset_=0,total_=0;
    QList<Dfs::Entry> listing_;
    std::unique_ptr<QFile> input_;
    std::unique_ptr<QSaveFile> output_;
};
Q_DECLARE_METATYPE(Dfs::Entry)
Q_DECLARE_METATYPE(QList<Dfs::Entry>)
