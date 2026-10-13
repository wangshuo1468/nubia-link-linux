#include "dfs.h"
#include <QtTest>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QNetworkProxy>
#include <QtEndian>

namespace {
QByteArray attr(qint64 size=0,quint32 mode=0100600) {
    QByteArray b(88,0); qToLittleEndian(mode,b.data()+16); qToLittleEndian(size,b.data()+40); return b;
}
struct Fixture {
    QTcpServer server; DfsClient client; QTcpSocket *peer=nullptr;
    Dfs::Reader reader; QList<Dfs::Packet> received;
    std::function<void(Dfs::Packet)> handler;
    Fixture() {
        server.setProxy(QNetworkProxy::NoProxy);
        QObject::connect(&server,&QTcpServer::newConnection,&server,[this] {
            peer=server.nextPendingConnection();
            QObject::connect(peer,&QTcpSocket::readyRead,&server,[this] {
                QList<Dfs::Packet> packets;
                QVERIFY(reader.append(peer->readAll(),packets));
                for(const auto &p:packets) {
                    received.append(p);
                    if(received.size()==1) {
                        QCOMPARE(p.type,quint8(3));
                        QCOMPARE(p.payload,Dfs::string("/mnt/dfs/")+Dfs::string("NubiaLinkLinux")+
                            Dfs::string(QCryptographicHash::hash("127.0.0.1test-device",QCryptographicHash::Md5).toHex())+Dfs::u32(1048576));
                        reply(p,105,attr(0,0040700));
                    } else if(handler) handler(p);
                }
            });
        });
    }
    ~Fixture() { QObject::disconnect(&server,nullptr,nullptr,nullptr); if(peer) QObject::disconnect(peer,nullptr,nullptr,nullptr); }
    void open() { QVERIFY(server.listen(QHostAddress::LocalHost)); client.start(QHostAddress::LocalHost,server.serverPort(),"test-device"); QTRY_VERIFY(client.ready()); }
    void reply(const Dfs::Packet &request,quint8 type,QByteArray payload) { peer->write(Dfs::frame(type,request.id,payload)); }
};
}
class DfsTest:public QObject {
    Q_OBJECT
private slots:
    void framingFragmentationAndBounds() {
        const auto bytes=Dfs::frame(103,42,Dfs::string("中文"));
        for(qsizetype split=0;split<=bytes.size();++split) {
            Dfs::Reader reader; QList<Dfs::Packet> out;
            QVERIFY(reader.append(QByteArrayView(bytes).first(split),out));
            QVERIFY(reader.append(QByteArrayView(bytes).sliced(split),out));
            QCOMPARE(out.size(),1); QCOMPARE(out[0].id,quint32(42)); QCOMPARE(out[0].payload,Dfs::string("中文"));
        }
        Dfs::Reader reader; QList<Dfs::Packet> out;
        QVERIFY(reader.append(bytes+Dfs::frame(101,43,{}),out)); QCOMPARE(out.size(),2);
        QVERIFY(!reader.append(Dfs::u32(Dfs::MaxPayload+1)+QByteArray(5,0),out));
        QVERIFY(!reader.append(bytes,out)); reader.reset(); QVERIFY(reader.append(bytes,out));
    }
    void rejectUnsafePathsAndMalformedEntries() {
        QVERIFY(Dfs::validPath("路径浏览/Download/中文.txt")); QVERIFY(Dfs::validPath(""));
        for(const auto &bad:QStringList{"../x","a/../b","/sdcard","a//b","a\\b","a/./b"}) QVERIFY(!Dfs::validPath(bad));
        QList<Dfs::Entry> entries;
        const auto good=Dfs::u32(1)+Dfs::string("中文.txt")+attr(123);
        QVERIFY(Dfs::entries(good,entries)); QCOMPARE(entries[0].size,qint64(123)); QCOMPARE(entries[0].name,QString("中文.txt"));
        QVERIFY(!Dfs::entries(good.chopped(1),entries));
        QVERIFY(!Dfs::entries(Dfs::u32(1)+Dfs::string("../x")+attr(),entries));
        QVERIFY(!Dfs::entries(Dfs::u32(0xffffffff),entries));
    }
    void directoryPagedAndHandleClosed() {
        Fixture f; f.open(); QSignalSpy listed(&f.client,&DfsClient::listing);
        int page=0;
        f.handler=[&](const Dfs::Packet &p) {
            if(p.type==5) { QCOMPARE(p.payload,Dfs::string("/mnt/dfs/")); f.reply(p,102,Dfs::u32(7)); }
            else if(p.type==6) {
                QCOMPARE(p.payload,Dfs::u32(7));
                if(page++==0) f.reply(p,104,Dfs::u32(1)+Dfs::string("Download")+attr(0,0040700));
                else f.reply(p,101,Dfs::u32(1));
            } else { QCOMPARE(p.type,quint8(2)); f.reply(p,101,Dfs::u32(0)); }
        };
        QVERIFY(f.client.list("")); QTRY_COMPARE(listed.size(),1);
        const auto entries=qvariant_cast<QList<Dfs::Entry>>(listed[0][1]); QCOMPARE(entries.size(),1); QVERIFY(entries[0].directory()); QVERIFY(!f.client.busy());
    }
    void transferMultipleChunksAndExactBytes() {
        Fixture f; f.open(); QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto source=dir.filePath("source"),target=dir.filePath("target");
        QByteArray content(150003,0); for(qsizetype i=0;i<content.size();++i) content[i]=char(i*29);
        QFile local(source); QVERIFY(local.open(QIODevice::WriteOnly)); QCOMPARE(local.write(content),qint64(content.size())); local.close();
        QByteArray phone;
        f.handler=[&](const Dfs::Packet &p) {
            if(p.type==1) {
                const auto path=Dfs::string("/mnt/dfs/Download/中文.bin");
                QVERIFY(p.payload.startsWith(path));
                const auto flags=qFromBigEndian<quint32>(p.payload.constData()+path.size());
                QVERIFY(flags==0xc1 || flags==0); f.reply(p,102,Dfs::u32(19));
            } else if(p.type==8) {
                QVERIFY(p.payload.size()>=20); QCOMPARE(qFromBigEndian<quint32>(p.payload.constData()),quint32(1));
                QCOMPARE(qFromBigEndian<quint32>(p.payload.constData()+4),quint32(19));
                QCOMPARE(qFromBigEndian<quint64>(p.payload.constData()+8),quint64(phone.size()));
                const auto size=qFromBigEndian<quint32>(p.payload.constData()+16); QCOMPARE(p.payload.size(),20+qsizetype(size));
                phone+=p.payload.mid(20); f.reply(p,101,Dfs::u32(0));
            } else if(p.type==3) f.reply(p,105,attr(phone.size()));
            else if(p.type==7) {
                QCOMPARE(p.payload.size(),qsizetype(16));
                const auto offset=qFromBigEndian<quint64>(p.payload.constData()+4),size=quint64(qFromBigEndian<quint32>(p.payload.constData()+12));
                const auto chunk=phone.mid(qsizetype(offset),qsizetype(size)); f.reply(p,103,Dfs::u32(chunk.size())+chunk);
            } else { QVERIFY(p.type==2 || p.type==24); f.reply(p,101,Dfs::u32(0)); }
        };
        QSignalSpy completed(&f.client,&DfsClient::completed),errors(&f.client,&DfsClient::operationFailed);
        QVERIFY(f.client.upload(source,"Download/中文.bin")); QVERIFY(!f.client.list("")); QTRY_COMPARE(completed.size(),1); QCOMPARE(phone,content);
        QVERIFY(f.client.download("Download/中文.bin",target)); QTRY_COMPARE(completed.size(),2); QCOMPARE(errors.size(),0);
        QFile saved(target); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(),content);
    }
    void existingPhoneFileRejectedWithoutDeletion() {
        Fixture f; f.open(); QTemporaryDir dir;
        QFile file(dir.filePath("source")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("test"); file.close();
        f.handler=[&](const Dfs::Packet &p) { QCOMPARE(p.type,quint8(1)); f.reply(p,101,Dfs::u32(17)); };
        QSignalSpy errors(&f.client,&DfsClient::operationFailed);
        QVERIFY(f.client.upload(file.fileName(),"Download/existing")); QTRY_COMPARE(errors.size(),1); QVERIFY(!f.client.busy());
        QTest::qWait(30); QCOMPARE(f.received.size(),2);
    }
    void wrongRequestIdPreservesExistingLocalFile() {
        Fixture f; f.open(); QTemporaryDir dir; const auto target=dir.filePath("target");
        QFile file(target); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("original"); file.close();
        f.handler=[&](const Dfs::Packet &p) { f.peer->write(Dfs::frame(105,p.id+1,attr(3))); };
        QSignalSpy errors(&f.client,&DfsClient::error);
        QVERIFY(f.client.download("Download/test",target)); QTRY_COMPARE(errors.size(),1); QVERIFY(!f.client.ready());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),QByteArray("original"));
    }
};
QTEST_GUILESS_MAIN(DfsTest)
#include "dfs_test.moc"
