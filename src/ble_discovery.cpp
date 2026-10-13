#include "ble_discovery.h"
#include "rfcomm_connector.h"
#include <QCryptographicHash>
#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQueue>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QElapsedTimer>
#include <QUuid>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <unistd.h>

using Interfaces=QMap<QString,QVariantMap>;
using Objects=QMap<QDBusObjectPath,Interfaces>;
Q_DECLARE_METATYPE(Interfaces)
Q_DECLARE_METATYPE(Objects)
namespace {
const QString Uuid="187f8000-0000-1000-8000-ceb574815000";
const QString Root="/org/nubialink/collaboration";
QString compact(QJsonObject o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
// HCIGETDEVINFO is read-only and does not require a privileged HCI command.
// BLE advertising alone does not make a dual-mode adapter accept RFCOMM.
int pageScan(QString adapter) {
    const auto match=QRegularExpression("/hci([0-9]+)$").match(adapter);
    if(!match.hasMatch()) return -1;
    hci_dev_info info{};
    if(hci_devinfo(match.captured(1).toInt(),&info)<0) return -1;
    return hci_test_bit(HCI_PSCAN,&info.flags) ? 1 : 0;
}
}
namespace NubiaBle {
QByteArray identity(QString id) {
    if(!QRegularExpression("^[0-9a-f]{12}$").match(id).hasMatch()) return {};
    return QByteArray::fromHex(id.toLatin1());
}
QByteArray response(QString name,quint8 role,quint8 tag) {
    // 16-bit service data UUID encodes the role and service revision.
    QByteArray bytes; bytes.append(char(tag)); bytes.append(char(role));
    QByteArray payload(27,'\0'); auto text=name.toUtf8();
    if(text.size()>16) { text=text.left(15); while(!text.isEmpty() && !QString::fromUtf8(text).toUtf8().startsWith(text)) text.chop(1); text.append('\t'); }
    for(int i=0;i<text.size();++i) payload[9+i]=text[i];
    payload[25]=1; payload[26]=1; return bytes+payload;
}
QString desktopId() {
    QSettings settings; auto id=settings.value("collaboration/deviceId").toString();
    if(!QRegularExpression("^[0-9a-f]{12}$").match(id).hasMatch()) {
        if(id.isEmpty()) id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        id=QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(),QCryptographicHash::Sha256).left(6).toHex());
        settings.setValue("collaboration/deviceId",id);
    }
    return id;
}
}
class BleObjects : public QObject {
    Q_OBJECT
public:
    Objects objects;
};
class ObjectAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.freedesktop.DBus.ObjectManager")
public:
    explicit ObjectAdaptor(BleObjects *p):QDBusAbstractAdaptor(p),owner(p) {}
public slots:
    Objects GetManagedObjects() const { return owner->objects; }
private:
    BleObjects *owner;
};
class ServiceAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.bluez.GattService1")
    Q_PROPERTY(QString UUID READ uuid)
    Q_PROPERTY(bool Primary READ primary)
    Q_PROPERTY(QList<QDBusObjectPath> Includes READ includes)
public:
    explicit ServiceAdaptor(QObject *p):QDBusAbstractAdaptor(p) {}
    QString uuid() const { return Uuid; }
    bool primary() const { return true; }
    QList<QDBusObjectPath> includes() const { return {}; }
};
class CharacteristicAdaptor : public QDBusAbstractAdaptor,protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.bluez.GattCharacteristic1")
    Q_PROPERTY(QString UUID READ uuid)
    Q_PROPERTY(QDBusObjectPath Service READ service)
    Q_PROPERTY(QStringList Flags READ flags)
public:
    CharacteristicAdaptor(QObject *p,bool writable):QDBusAbstractAdaptor(p),write_(writable) {}
    QString uuid() const { return write_ ? "187f8000-0000-1000-8000-ceb574815003" : "187f8000-0000-1000-8000-ceb574815001"; }
    QDBusObjectPath service() const { return QDBusObjectPath(Root+"/service"); }
    QStringList flags() const { return write_ ? QStringList{"write","write-without-response"} : QStringList{"read"}; }
    std::function<QByteArray(QString,int)> read;
    std::function<bool(QString,QByteArray,int)> write;
public slots:
    QByteArray ReadValue(QVariantMap options) {
        if(write_ || !read) { sendErrorReply("org.bluez.Error.NotSupported"); return {}; }
        return read(qvariant_cast<QDBusObjectPath>(options.value("device")).path(),options.value("offset").toInt());
    }
    void WriteValue(QByteArray value,QVariantMap options) {
        if(!write_ || !write || !write(qvariant_cast<QDBusObjectPath>(options.value("device")).path(),value,options.value("offset").toInt()))
            sendErrorReply("org.bluez.Error.InvalidValueLength");
    }
private:
    bool write_;
};
class AdvertisementAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.bluez.LEAdvertisement1")
    Q_PROPERTY(QString Type READ type)
    Q_PROPERTY(QStringList ServiceUUIDs READ services)
    Q_PROPERTY(QVariantMap ServiceData READ data)
    Q_PROPERTY(QVariantMap ScanResponseServiceData READ scan)
    Q_PROPERTY(quint16 Timeout READ timeout)
    Q_PROPERTY(quint32 MinInterval READ minInterval)
    Q_PROPERTY(quint32 MaxInterval READ maxInterval)
public:
    explicit AdvertisementAdaptor(QObject *p):QDBusAbstractAdaptor(p) {}
    QByteArray deviceBytes,scanBytes;
    QString type() const { return "peripheral"; }
    QStringList services() const { return {Uuid}; }
    QVariantMap data() const { return {{"00000050-0000-1000-8000-00805f9b34fb",deviceBytes}}; }
    QVariantMap scan() const {
        if(scanBytes.size()<2) return {};
        const auto tag=quint8(scanBytes[0]);
        const auto role=quint8(scanBytes[1]);
        return {{QString("0000%1%2-0000-1000-8000-00805f9b34fb").arg(role,2,16,QChar('0')).arg(tag,2,16,QChar('0')),scanBytes.mid(2)}};
    }
    quint16 timeout() const { return 0; }
    quint32 minInterval() const { return 100; }
    quint32 maxInterval() const { return 150; }
public slots:
    void Release() {}
};
class ProfileAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.bluez.Profile1")
public:
    explicit ProfileAdaptor(QObject *p):QDBusAbstractAdaptor(p) {}
    std::function<void(int,QString)> connected;
    std::function<void(QString)> disconnected;
public slots:
    void Release() {}
    void RequestDisconnection(QDBusObjectPath device) { if(disconnected) disconnected(device.path()); }
    void NewConnection(QDBusObjectPath device,QDBusUnixFileDescriptor descriptor,QVariantMap) {
        const int fd=dup(descriptor.fileDescriptor());
        if(fd>=0 && connected) connected(fd,device.path()); else if(fd>=0) ::close(fd);
    }
};
struct BleDiscovery::Impl {
    BleDiscovery *owner;
    QDBusConnection bus=QDBusConnection::systemBus();
    QString busName;
    BleObjects objects;
    QObject service,readObject,writeObject,advertisement,profile;
    CharacteristicAdaptor *read,*write;
    AdvertisementAdaptor *ad;
    ProfileAdaptor *rfcomm;
    QString adapter,id,address;
    QJsonArray services;
    QMap<QString,QQueue<QByteArray>> queues;
    QMap<QString,QByteArray> last;
    QMap<QString,QString> peerHints;
    QString targetId,lePath,readPath,classicPath;
    RfcommConnector connector;
    QTimer search;
    QElapsedTimer searchTime;
    bool searchPending=false,searchStarted=false,ownLeConnection=false;
    int searchStage=0,readCount=0,reconnects=0;
    QStringList registered;
    quint64 generation=0;
    bool gattRegistered=false,adRegistered=false,profileRegistered=false;
    bool discoverabilityChanged=false;
    bool connectabilityChanged=false,discoverableTimeoutChanged=false;
    quint32 previousDiscoverableTimeout=0;
    explicit Impl(BleDiscovery *p):owner(p) {
        qDBusRegisterMetaType<Interfaces>(); qDBusRegisterMetaType<Objects>();
        new ObjectAdaptor(&objects); new ServiceAdaptor(&service);
        read=new CharacteristicAdaptor(&readObject,false); write=new CharacteristicAdaptor(&writeObject,true);
        ad=new AdvertisementAdaptor(&advertisement); rfcomm=new ProfileAdaptor(&profile);
        QObject::connect(&connector,&RfcommConnector::connected,owner,[this](int fd) {
            emit owner->managementSocket(fd,classicPath);
        });
        QObject::connect(&connector,&RfcommConnector::error,owner,&BleDiscovery::error);
        search.setInterval(500);
        QObject::connect(&search,&QTimer::timeout,owner,[this] { pollPeer(); });
        read->read=[this](QString device,int offset) {
            if(offset<0 || offset>512) return QByteArray{};
            if(!offset) {
                auto &queue=queues[device]; last[device]=queue.isEmpty() ? QByteArray{} : queue.dequeue();
            }
            return last.value(device).mid(offset);
        };
        write->write=[this](QString device,QByteArray bytes,int offset) {
            if(device.isEmpty() || offset || bytes.isEmpty() || bytes.size()>512) return false;
            const auto rpc=QJsonDocument::fromJson(bytes).object();
            if(rpc.value("action")!="GATT_SERVICE_SCAN") return false;
            const auto peer=rpc.value("profile").toString().split('-');
            if(peer.isEmpty() || NubiaBle::identity(peer[0]).isEmpty() || queues.size()>=16) return false;
            auto &queue=queues[device]; queue.clear();
            for(const auto &value:services) {
                const auto entry=value.toObject();
                const QJsonObject service{{"serviceName",entry.value("serviceName")},{"serviceUuid",entry.value("serviceUUid")},
                    {"serviceVersion",entry.value("version")},{"enable",true},{"authType",0},{"custData",entry.value("custData")},{"serviceType","publish"}};
                queue.enqueue(compact({{"action","GATT_SERVICE_SCAN"},{"profile",compact(service)}}).toUtf8());
            }
            queue.enqueue(compact({{"action","GATT_SERVICE_SCAN_FINISHED"},{"profile",compact({{"a",address},{"b","Nubia Link Linux"}})}}).toUtf8());
            peerHints[device]=peer[0];
            emit owner->peerScanned(peer[0],peer.value(1),device);
            emit owner->status(QStringLiteral("手机已通过蓝牙读取协同服务列表")); return true;
        };
        rfcomm->connected=[this](int fd,QString device) {
            emit owner->status(QStringLiteral("手机正在建立协同蓝牙管理连接"));
            emit owner->managementSocket(fd,device);
        };
        rfcomm->disconnected=[this](QString device) { emit owner->managementDisconnected(device); };
    }
    void call(QString path,QString interface,QString method,QVariantList args,std::function<void()> success) {
        auto message=QDBusMessage::createMethodCall("org.bluez",path,interface,method); message.setArguments(args);
        const auto current=generation;
        auto watcher=new QDBusPendingCallWatcher(bus.asyncCall(message,10000),owner);
        QObject::connect(watcher,&QDBusPendingCallWatcher::finished,owner,[this,current,success,method](QDBusPendingCallWatcher *w) {
            const QDBusPendingReply<> reply=*w; w->deleteLater();
            if(current!=generation) return;
            if(reply.isError() && method=="StartDiscovery" && reply.error().name()=="org.bluez.Error.InProgress") {
                // BlueZ can still be finishing a previous scan transition.
                // The existing scan inventory is sufficient for selection;
                // this private bus does not own the other client's scan.
                searchStarted=false; pollPeer(); return;
            }
            if(reply.isError()) { emit owner->error(QStringLiteral("蓝牙协同操作失败（")+method+"）："+reply.error().name()+" "+reply.error().message()); return; }
            success();
        });
    }
    void request(QString path,QString interface,QString method,QVariantList args,std::function<void(QDBusMessage)> success) {
        auto message=QDBusMessage::createMethodCall("org.bluez",path,interface,method); message.setArguments(args);
        const auto current=generation;
        auto watcher=new QDBusPendingCallWatcher(bus.asyncCall(message,15000),owner);
        QObject::connect(watcher,&QDBusPendingCallWatcher::finished,owner,[this,current,success,method](QDBusPendingCallWatcher *w) {
            const auto reply=w->reply(); w->deleteLater();
            if(current!=generation) return;
            if(reply.type()==QDBusMessage::ErrorMessage) {
                // A phone-initiated GATT scan may end the LE bearer between
                // inventory and WriteValue. Reconnect only for this transient
                // failure, within the existing discovery deadline.
                if((method=="ReadValue" || method=="WriteValue" || method=="ConnectDevice") &&
                   (reply.errorName()=="org.bluez.Error.NotConnected" || reply.errorMessage().contains("Not connected")) &&
                   ++reconnects<=3 && searchTime.elapsed()<90000) {
                    searchStage=0; searchPending=false; readPath.clear(); readCount=0;
                    emit owner->status(QStringLiteral("手机结束了蓝牙扫描连接，正在重新建立协同连接")); return;
                }
                emit owner->error(QStringLiteral("协同蓝牙连接失败（")+method+"）："+reply.errorName()+" "+reply.errorMessage()); return;
            }
            success(reply);
        });
    }
    void readService() {
        if(++readCount>64) { emit owner->error(QStringLiteral("手机协同蓝牙服务列表过长")); return; }
        request(readPath,"org.bluez.GattCharacteristic1","ReadValue",{QVariantMap{}},[this](QDBusMessage reply) {
            const auto bytes=qdbus_cast<QByteArray>(reply.arguments().value(0));
            if(bytes.isEmpty() || bytes.size()>512) { emit owner->error(QStringLiteral("手机返回了无效的协同蓝牙服务数据")); return; }
            const auto rpc=QJsonDocument::fromJson(bytes).object();
            if(rpc.value("action")=="GATT_SERVICE_SCAN") { readService(); return; }
            const auto info=QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8()).object();
            const auto mac=info.value("a").toString().toUpper();
            if(rpc.value("action")!="GATT_SERVICE_SCAN_FINISHED" ||
               !QRegularExpression("^([0-9A-F]{2}:){5}[0-9A-F]{2}$").match(mac).hasMatch() ||
               mac=="00:00:00:00:00:00" || mac==address.toUpper()) {
                emit owner->error(QStringLiteral("手机未返回有效的协同蓝牙管理地址")); return;
            }
            classicPath=adapter+"/dev_"+QString(mac).replace(':','_');
            qInfo().noquote()<<"Original phone RFCOMM address"<<mac;
            emit owner->peerScanned(targetId,info.value("b").toString(),classicPath);
            searchStage=3; searchPending=false; search.stop();
            // Device1.Disconnect closes both bearers after BlueZ merges the
            // public identity. Keep our LE connection until normal stop so
            // it cannot cancel the RFCOMM connection being established.
            emit owner->status(QStringLiteral("已读取手机协同服务，正在建立蓝牙管理连接"));
            if(!connector.start(address,mac)) emit owner->error(QStringLiteral("协同蓝牙管理连接已经在建立中"));
        });
    }
    void pollPeer() {
        if(targetId.isEmpty() || searchPending || !adRegistered) return;
        if(searchTime.elapsed()>90000) { search.stop(); emit owner->error(QStringLiteral("未能发现手机的协同蓝牙服务，请保持手机多屏协同开启")); return; }
        searchPending=true;
        request("/","org.freedesktop.DBus.ObjectManager","GetManagedObjects",{},[this](QDBusMessage reply) {
            searchPending=false;
            const auto inventory=qdbus_cast<Objects>(reply.arguments().value(0));
            if(searchStage==0) {
                for(auto it=inventory.cbegin();it!=inventory.cend();++it) {
                    const auto device=it.value().value("org.bluez.Device1");
                    if(device.isEmpty() || !it.key().path().startsWith(adapter+"/dev_")) continue;
                    const auto data=qdbus_cast<QVariantMap>(device.value("ServiceData"));
                    const auto bytes=qdbus_cast<QByteArray>(data.value("00000050-0000-1000-8000-00805f9b34fb"));
                    if(QString::fromLatin1(bytes.toHex())!=targetId && peerHints.value(it.key().path())!=targetId) continue;
                    lePath=it.key().path(); searchStage=1;
                    if(!device.value("Connected").toBool()) {
                        ownLeConnection=true; searchPending=true;
                        // After identity resolution Device1.Connect may
                        // prefer classic audio profiles. AddressType selects
                        // the LE bearer explicitly for the GATT scan.
                        request(adapter,"org.bluez.Adapter1","ConnectDevice",{QVariantMap{
                            {"Address",device.value("Address")},{"AddressType",device.value("AddressType")}}},
                            [this](QDBusMessage reply) {
                                const auto path=qdbus_cast<QDBusObjectPath>(reply.arguments().value(0)).path();
                                if(!path.isEmpty()) lePath=path;
                                searchPending=false; pollPeer();
                            });
                        return;
                    }
                    break;
                }
            }
            if(searchStage==1) {
                const auto device=inventory.value(QDBusObjectPath(lePath)).value("org.bluez.Device1");
                if(!device.value("ServicesResolved").toBool()) return;
                QString writePath;
                for(auto it=inventory.cbegin();it!=inventory.cend();++it) {
                    if(!it.key().path().startsWith(lePath+"/")) continue;
                    const auto characteristic=it.value().value("org.bluez.GattCharacteristic1");
                    if(characteristic.value("UUID").toString()==read->uuid()) readPath=it.key().path();
                    if(characteristic.value("UUID").toString()==write->uuid()) writePath=it.key().path();
                }
                if(readPath.isEmpty() || writePath.isEmpty()) { emit owner->error(QStringLiteral("手机没有公布原厂协同蓝牙读写服务")); return; }
                searchStage=2; searchPending=true;
                const auto scan=compact({{"action","GATT_SERVICE_SCAN"},{"profile",id+"-Nubia Link Linux"}}).toUtf8();
                // Android's characteristic prefers WRITE_NO_RESPONSE and its
                // server never acknowledges a write request. Use ATT command.
                request(writePath,"org.bluez.GattCharacteristic1","WriteValue",{scan,QVariantMap{{"type","command"}}},[this](QDBusMessage) {
                    const auto current=generation;
                    QTimer::singleShot(300,owner,[this,current] { if(current==generation) readService(); });
                });
            }
        });
    }
};
BleDiscovery::BleDiscovery(QObject *p):QObject(p),impl_(std::make_unique<Impl>(this)) {}
BleDiscovery::~BleDiscovery() { stop(); }
bool BleDiscovery::start(QString id,QJsonArray services) {
    stop(); auto &s=*impl_;
    s.busName="nubia-ble-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    s.bus=QDBusConnection::connectToBus(QDBusConnection::SystemBus,s.busName);
    if(NubiaBle::identity(id).isEmpty() || !s.bus.isConnected()) { emit error(QStringLiteral("协同需要可用的系统蓝牙服务和六字节设备身份")); return false; }
    QDBusInterface root("org.bluez","/","org.freedesktop.DBus.ObjectManager",s.bus);
    const QDBusReply<Objects> inventory=root.call("GetManagedObjects");
    if(!inventory.isValid()) { emit error(QStringLiteral("无法读取 BlueZ 蓝牙适配器")); return false; }
    const auto devices=inventory.value();
    for(auto i=devices.cbegin();i!=devices.cend();++i) {
        if(i.value().contains("org.bluez.LEAdvertisingManager1") && i.value().contains("org.bluez.GattManager1") &&
           i.value().value("org.bluez.Adapter1").value("Powered").toBool()) {
            s.adapter=i.key().path(); s.address=i.value().value("org.bluez.Adapter1").value("Address").toString(); break;
        }
    }
    if(s.adapter.isEmpty()) { emit error(QStringLiteral("未找到已开启且支持协同广播的蓝牙适配器")); return false; }
    // The ROM indexes raw advertising bytes. Per-advertisement Discoverable
    // makes BlueZ append flags; adapter discoverability lets the kernel prepend
    // them, matching Android's original 31-byte packet. Restore our change.
    QDBusInterface adapter("org.bluez",s.adapter,"org.bluez.Adapter1",s.bus);
    const bool wasDiscoverable=adapter.property("Discoverable").toBool();
    s.previousDiscoverableTimeout=adapter.property("DiscoverableTimeout").toUInt();
    const auto connectable=adapter.property("Connectable");
    if(connectable.isValid() && (!connectable.toBool() || pageScan(s.adapter)==0)) {
        s.connectabilityChanged=!connectable.toBool();
        // Reconcile a cached true property with a controller that has stopped
        // page scanning; do not toggle a healthy adapter or its power state.
        if((connectable.toBool() && !adapter.setProperty("Connectable",false)) ||
           !adapter.setProperty("Connectable",true)) {
            stop(); emit error(QStringLiteral("无法开启电脑蓝牙的协同连接接收")); return false;
        }
    }
    s.discoverableTimeoutChanged=s.previousDiscoverableTimeout!=0;
    s.discoverabilityChanged=!wasDiscoverable;
    // Keep reception available for the whole session, including when the
    // desktop was already discoverable with a short system timeout.
    if((s.discoverableTimeoutChanged && !adapter.setProperty("DiscoverableTimeout",quint32(0))) ||
       !adapter.setProperty("Discoverable",true)) {
        stop(); emit error(QStringLiteral("无法开启协同蓝牙自动发现")); return false;
    }
    if(pageScan(s.adapter)==0) {
        stop(); emit error(QStringLiteral("电脑蓝牙未开启连接接收，手机无法建立协同管理连接")); return false;
    }
    QSettings settings;
    const auto tag=quint8(settings.value("collaboration/bleServiceTag",0).toUInt()+1);
    settings.setValue("collaboration/bleServiceTag",tag);
    s.id=id; s.services=services; s.ad->deviceBytes=NubiaBle::identity(id); s.ad->scanBytes=NubiaBle::response("Nubia Link Linux",7,tag);
    s.objects.objects={
        {QDBusObjectPath(Root+"/service"),{{"org.bluez.GattService1",{{"UUID",Uuid},{"Primary",true},{"Includes",QVariant::fromValue(QList<QDBusObjectPath>{})}}}}},
        {QDBusObjectPath(Root+"/service/read"),{{"org.bluez.GattCharacteristic1",{{"UUID",s.read->uuid()},{"Service",QVariant::fromValue(s.read->service())},{"Flags",s.read->flags()}}}}},
        {QDBusObjectPath(Root+"/service/write"),{{"org.bluez.GattCharacteristic1",{{"UUID",s.write->uuid()},{"Service",QVariant::fromValue(s.write->service())},{"Flags",s.write->flags()}}}}}
    };
    const QList<QPair<QString,QObject *>> exports{{Root,&s.objects},{Root+"/service",&s.service},{Root+"/service/read",&s.readObject},
        {Root+"/service/write",&s.writeObject},{Root+"/advertisement",&s.advertisement},{Root+"/profile",&s.profile}};
    for(const auto &entry:exports) {
        if(!s.bus.registerObject(entry.first,entry.second,QDBusConnection::ExportAdaptors)) { stop(); emit error(QStringLiteral("无法导出协同蓝牙服务")); return false; }
        s.registered.append(entry.first);
    }
    s.call("/org/bluez","org.bluez.ProfileManager1","RegisterProfile",{QVariant::fromValue(QDBusObjectPath(Root+"/profile")),Uuid,
        QVariantMap{{"Name","Nubia Link Linux"},{"Channel",QVariant::fromValue(quint16(0))},{"RequireAuthentication",false},{"RequireAuthorization",false}}},[this] {
        auto &s=*impl_; s.profileRegistered=true;
        s.call(s.adapter,"org.bluez.GattManager1","RegisterApplication",{QVariant::fromValue(QDBusObjectPath(Root)),QVariantMap{}},[this] {
            auto &s=*impl_; s.gattRegistered=true;
            s.call(s.adapter,"org.bluez.LEAdvertisingManager1","RegisterAdvertisement",{QVariant::fromValue(QDBusObjectPath(Root+"/advertisement")),QVariantMap{}},[this] {
                impl_->adRegistered=true; emit status(QStringLiteral("独立协同蓝牙发现已开启，等待手机搜索电脑"));
            });
        });
    });
    return true;
}
bool BleDiscovery::connectPeer(QString phoneId) {
    auto &s=*impl_;
    if(NubiaBle::identity(phoneId).isEmpty() || s.adapter.isEmpty() || !s.targetId.isEmpty()) return false;
    s.targetId=phoneId; s.searchTime.start(); s.search.start();
    s.call(s.adapter,"org.bluez.Adapter1","SetDiscoveryFilter",{QVariantMap{{"Transport","le"},{"UUIDs",QStringList{Uuid}}}},[this] {
        auto &s=*impl_;
        s.call(s.adapter,"org.bluez.Adapter1","StartDiscovery",{},[this] {
            impl_->searchStarted=true; impl_->pollPeer();
            emit status(QStringLiteral("正在自动发现手机的原厂协同蓝牙服务"));
        });
    });
    return true;
}
void BleDiscovery::stop() {
    auto &s=*impl_; ++s.generation;
    s.search.stop(); s.connector.stop();
    auto unregister=[&](QString path,QString iface,QString method,QString object) {
        auto message=QDBusMessage::createMethodCall("org.bluez",path,iface,method); message<<QVariant::fromValue(QDBusObjectPath(object)); s.bus.asyncCall(message);
    };
    if(s.adRegistered) unregister(s.adapter,"org.bluez.LEAdvertisingManager1","UnregisterAdvertisement",Root+"/advertisement");
    if(s.gattRegistered) unregister(s.adapter,"org.bluez.GattManager1","UnregisterApplication",Root);
    if(s.profileRegistered) unregister("/org/bluez","org.bluez.ProfileManager1","UnregisterProfile",Root+"/profile");
    if(s.searchStarted) {
        auto message=QDBusMessage::createMethodCall("org.bluez",s.adapter,"org.bluez.Adapter1","StopDiscovery"); s.bus.asyncCall(message);
    }
    if(s.ownLeConnection && !s.lePath.isEmpty()) {
        auto message=QDBusMessage::createMethodCall("org.bluez",s.lePath,"org.bluez.Device1","Disconnect"); s.bus.asyncCall(message);
    }
    if(!s.adapter.isEmpty()) {
        QDBusInterface adapter("org.bluez",s.adapter,"org.bluez.Adapter1",s.bus);
        if(adapter.property("DiscoverableTimeout").toUInt()==0) {
            if(s.discoverabilityChanged) adapter.setProperty("Discoverable",false);
            if(s.discoverableTimeoutChanged) adapter.setProperty("DiscoverableTimeout",s.previousDiscoverableTimeout);
        }
        if(s.connectabilityChanged && adapter.property("Connectable").toBool()) adapter.setProperty("Connectable",false);
    }
    for(const auto &path:s.registered) s.bus.unregisterObject(path);
    s.registered.clear(); s.queues.clear(); s.last.clear(); s.adapter.clear();
    s.adRegistered=s.gattRegistered=s.profileRegistered=false;
    s.discoverabilityChanged=false;
    s.connectabilityChanged=s.discoverableTimeoutChanged=false;
    s.targetId.clear(); s.lePath.clear(); s.readPath.clear(); s.classicPath.clear(); s.peerHints.clear();
    s.searchPending=s.searchStarted=s.ownLeConnection=false; s.searchStage=s.readCount=s.reconnects=0;
    if(!s.busName.isEmpty()) { s.bus=QDBusConnection::systemBus(); QDBusConnection::disconnectFromBus(s.busName); s.busName.clear(); }
}
QString BleDiscovery::bluetoothAddress() const { return impl_->address; }
#include "ble_discovery.moc"
