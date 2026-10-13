#include "collaboration_wire.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>

QByteArray Collaboration::payloadFrame(QByteArrayView payload,qint64 id) {
    if(payload.isEmpty() || payload.size()>MaxPayload || id<0 || id>9007199254740991LL) return {};
    QJsonArray data;
    for(const char byte:payload) data.append(int(qint8(byte)));
    return QJsonDocument(QJsonObject{{"type",0},{"id",id},{"data",data}}).toJson(QJsonDocument::Compact);
}
QByteArray Collaboration::controlFrame(QString action,QJsonObject extra,int id) {
    if(action.isEmpty() || id<0) return {};
    const auto body=QJsonDocument(QJsonObject{{"action",action},{"id",id},{"ackId",-1},
        {"extra",QString::fromUtf8(QJsonDocument(extra).toJson(QJsonDocument::Compact))}}).toJson(QJsonDocument::Compact);
    return payloadFrame(body,id);
}
void Collaboration::MessageReader::reset() {
    buffer_.clear(); error_.clear(); depth_=0; quoted_=false; escaped_=false;
}
bool Collaboration::MessageReader::consume(QList<QByteArray> &payloads) {
    const auto doc=QJsonDocument::fromJson(buffer_);
    if(!doc.isObject()) { error_="Invalid collaboration JSON frame"; return false; }
    const auto object=doc.object();
    if(object.value("type")==1) return true; // Official heartbeat has no payload.
    const auto data=object.value("data");
    if(object.value("type")!=0 || !data.isArray() || data.toArray().size()>MaxPayload) {
        error_="Invalid collaboration payload envelope"; return false;
    }
    QByteArray payload;
    for(const auto &byte:data.toArray()) {
        const auto value=byte.toDouble(1000);
        if(!byte.isDouble() || !std::isfinite(value) || value<-128 || value>127 || std::floor(value)!=value) {
            error_="Invalid Java byte in collaboration payload"; return false;
        }
        payload.append(char(int(value)&255));
    }
    payloads.append(payload); return true;
}
bool Collaboration::MessageReader::append(QByteArrayView bytes,QList<QByteArray> &payloads) {
    if(!error_.isEmpty()) return false;
    for(const char byte:bytes) {
        if(buffer_.isEmpty()) {
            if(byte==' ' || byte=='\r' || byte=='\n' || byte=='\t') continue;
            if(byte!='{') { error_="Invalid collaboration frame start"; return false; }
        }
        if(buffer_.size()>=MaxWire) { error_="Collaboration frame too large"; return false; }
        buffer_.append(byte);
        if(quoted_) {
            if(escaped_) escaped_=false;
            else if(byte=='\\') escaped_=true;
            else if(byte=='"') quoted_=false;
            continue;
        }
        if(byte=='"') quoted_=true;
        else if(byte=='{' || byte=='[') {
            if(++depth_>32) { error_="Collaboration nesting too deep"; return false; }
        } else if(byte=='}' || byte==']') {
            if(--depth_<0) { error_="Invalid collaboration nesting"; return false; }
            if(depth_==0) {
                if(!consume(payloads)) return false;
                buffer_.clear();
            }
        }
    }
    return true;
}
