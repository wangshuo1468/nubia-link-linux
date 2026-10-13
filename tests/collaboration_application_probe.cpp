#include "collaboration_session.h"
#include <QCoreApplication>
#include <QNetworkInterface>
#include <QJsonDocument>
#include <QTextStream>
#include <QImage>
#include <QElapsedTimer>

// Explicit research: first accept an independent collaboration mirror, then
// request virtualDisplay over that same identity. Default rejects the offer;
// --accept-virtual negotiates and decodes it but still requires separate
// verification that the phone foreground and input remain independent.
class CollaborationApplicationProbe: public QObject {
public:
    CollaborationApplicationProbe(QHostAddress local,bool accept):QObject(nullptr),accept_(accept) {
        connect(&session_,&CollaborationSession::status,this,[](QString text) { qInfo().noquote()<<text; });
        connect(&session_,&CollaborationSession::stopped,this,[this] { QCoreApplication::exit(offered_ && (!accept_ || decodedVirtual_) ? 0 : 1); });
        connect(&session_.link_.manager(),&ServiceManager::message,this,[this](QJsonObject rpc) {
            if(!requested_ || offered_ || rpc.value("uuid")!="187f8000-0000-1000-8000-ceb574815100" || rpc.value("action")!="distributebus.action.startservice") return;
            const auto profile=QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8()).object();
            const auto type=profile.value("castDisplayType").toString();
            qInfo()<<"Independent replacement offer type"<<type<<"dimensions"<<profile.value("width").toInt()<<profile.value("height").toInt();
            offered_=type=="virtualDisplay";
            if(accept_) return;
            session_.link_.manager().send("distributebus.action.onstartservicecallback",rpc.value("uuid").toString(),
                {{"result",false},{"CallBackServiceInParam",QStringLiteral("{\"resultCode\":2}")}});
            session_.link_.manager().flush();
            QTimer::singleShot(500,&session_,&CollaborationSession::stop);
        });
        connect(&poll_,&QTimer::timeout,this,[this] {
            const auto frame=session_.takeFrame();
            if(frame && session_.castDisplayType()=="virtualDisplay" && snapshotTimer_.elapsed()>2000) {
                snapshotTimer_.restart();
                const auto path=qEnvironmentVariable("NUBIA_COLLABORATION_PROBE_SNAPSHOT");
                if(!path.isEmpty()) {
                    QImage picture(frame->size,QImage::Format_RGB32);
                    const int width=frame->size.width(),height=frame->size.height(),chroma=(width+1)/2;
                    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
                        const double luma=(quint8(frame->y[y*width+x])-(frame->fullRange ? 0 : 16))*(frame->fullRange ? 1.0 : 255.0/219);
                        const double u=quint8(frame->u[(y/2)*chroma+x/2])-128,v=quint8(frame->v[(y/2)*chroma+x/2])-128;
                        picture.setPixel(x,y,qRgb(qBound(0,int(luma+1.5748*v),255),qBound(0,int(luma-.1873*u-.4681*v),255),qBound(0,int(luma+1.8556*u),255)));
                    }
                    picture.save(path);
                }
            }
            if(frame && accept_ && offered_ && session_.castDisplayType()=="virtualDisplay" && !decodedVirtual_) {
                decodedVirtual_=true; qInfo()<<"Independent virtual application decoded"<<frame->size;
                QTimer::singleShot(60000,&session_,&CollaborationSession::stop);
            }
            if(frame && !requested_ && session_.frameCount()>=3) {
                requested_=true;
                qInfo()<<"Independent mirror decoded"<<frame->size<<"association check received"<<session_.associated_;
                QTimer::singleShot(2000,this,[this] {
                    const auto sent=accept_ ? session_.requestApplication("com.android.settings") : session_.link_.manager().send("distributebus.action.startservice","187f8000-0000-1000-8000-ceb574815101",
                        {{"castDisplayType","virtualDisplay"},{"pkg","com.android.settings"}});
                    session_.link_.manager().flush(); qInfo()<<"Independent Settings application request sent"<<sent;
                    if(!accept_) QTimer::singleShot(15000,&session_,&CollaborationSession::stop);
                });
            }
        });
        poll_.start(16);
        snapshotTimer_.start();
        QTimer::singleShot(180000,this,[this] { session_.stop(); if(!session_.running()) QCoreApplication::exit(1); });
        bool started=false;
        for(const auto &interface:QNetworkInterface::allInterfaces()) for(const auto &entry:interface.addressEntries()) {
            if(entry.ip()==local && !started) started=session_.listen(local,entry.prefixLength(),interface.name());
        }
        if(!started) QTimer::singleShot(0,this,[] { QCoreApplication::exit(2); });
    }
private:
    CollaborationSession session_;
    QTimer poll_;
    QElapsedTimer snapshotTimer_;
    bool requested_=false,offered_=false,accept_=false,decodedVirtual_=false;
};
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if(app.arguments().size()<2 || app.arguments().size()>3 || (app.arguments().size()==3 && app.arguments()[2]!="--accept-virtual")) { QTextStream(stderr)<<"Usage: collaboration-application-probe LOCAL_IPV4 [--accept-virtual]\n"; return 2; }
    CollaborationApplicationProbe probe{QHostAddress(app.arguments()[1]),app.arguments().size()==3}; return app.exec();
}
