#include "collaboration_window.h"
#include "file_browser.h"
#include "link_shell.h"
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkInterface>
#include <QPushButton>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QWheelEvent>

CollaborationWindow::CollaborationWindow(QWidget *parent):QMainWindow(parent) {
    setObjectName("nubiaCollaborationWindow");
    setWindowFlags(parent ? Qt::Widget : Qt::Window);
    setWindowTitle(QStringLiteral("努比亚连接 · 多屏协同"));
    if(!parent) { resize(1280,800); setMinimumSize(760,620); }
    shell_=new LinkShell(true,this); setCentralWidget(shell_);
    shell_->advancedLayout->addWidget(new QLabel(QStringLiteral("应用包名")));
    package_=new QLineEdit(QStringLiteral("com.android.settings"));
    package_->setToolTip(QStringLiteral("独立显示的应用包名。当前需先在手机打开过该应用；一次显示一个应用。"));
    shell_->advancedLayout->addWidget(package_);
    stage_=shell_->stage; stage_->installEventFilter(this);
    view_=new QYUVOpenGLWidget(stage_); view_->hide(); view_->installEventFilter(this);
    view_->setObjectName("collaborationView"); view_->setFocusPolicy(Qt::StrongFocus);
    placeholder_=shell_->placeholder;
    auto actions=shell_->controls;
    back_=new QPushButton(QStringLiteral("返回")); back_->setObjectName("collaborationBack"); back_->setEnabled(false);
    application_=new QPushButton(QStringLiteral("独立显示应用")); application_->setObjectName("collaborationApplication"); application_->setEnabled(false);
    back_->setIcon(LinkShell::icon("back")); application_->setIcon(LinkShell::icon("app"));
    connect(application_,&QPushButton::clicked,this,[this] {
        releasePointer();
        if(!requestApplication(package_->text().trimmed())) status_->setText(QStringLiteral("暂时无法打开应用，请检查应用包名和协同连接"));
    });
    auto files=shell_->files;
    back_->setText({}); back_->setToolTip(QStringLiteral("返回")); back_->setFixedWidth(36);
    auto mute=new QPushButton; mute->setCheckable(true); mute->setIcon(LinkShell::icon("volume")); mute->setFixedWidth(36); mute->setToolTip(QStringLiteral("静音"));
    connect(mute,&QPushButton::toggled,this,[this,mute](bool muted) { session_.setMuted(muted); mute->setIcon(LinkShell::icon(muted ? "muted" : "volume")); });
    actions->addWidget(back_,0,Qt::AlignBottom); actions->addWidget(application_,0,Qt::AlignBottom); actions->addWidget(mute,0,Qt::AlignBottom); mute->setEnabled(false);
    actions->addStretch();
    auto message=new QWidget; auto messageLayout=new QVBoxLayout(message); messageLayout->setContentsMargins(0,0,0,0); messageLayout->setSpacing(6);
    auto messageLabel=new QLabel(QStringLiteral("发送到手机")); messageLabel->setObjectName("muted"); messageLayout->addWidget(messageLabel);
    auto inputRow=new QHBoxLayout; messageLayout->addLayout(inputRow);
    text_=new QLineEdit; text_->setObjectName("collaborationText");
    text_->setPlaceholderText(QStringLiteral("连接后，先选中手机输入框再发送文字")); text_->setEnabled(false);
    text_->setToolTip(QStringLiteral("先点击手机应用的输入框，再输入中文或其他文字。"));
    sendText_=new QPushButton(QStringLiteral("发送文字")); sendText_->setObjectName("collaborationSendText"); sendText_->setEnabled(false);
    sendText_->setProperty("primary",true);
    inputRow->addWidget(text_,1); inputRow->addWidget(sendText_); shell_->setMessageControls(message);
    auto submit=[this] {
        if(session_.commitText(text_->text())) text_->clear();
        else status_->setText(QStringLiteral("文字未发送，请确认手机输入框和输入法通道已就绪"));
    };
    connect(sendText_,&QPushButton::clicked,this,submit); connect(text_,&QLineEdit::returnPressed,this,submit);
    connect(&session_,&CollaborationSession::inputChanged,this,[this] {
        text_->setEnabled(session_.editable()); sendText_->setEnabled(session_.editable());
        application_->setEnabled(session_.applicationReady());
        if(!session_.mouseReady()) pointerDown_=false;
    });
    shell_->help->setText(QStringLiteral("手机与电脑开启蓝牙，并连接同一 Wi-Fi。在手机多屏协同的设备列表选择电脑后会自动接通。连接后点“独立显示应用”，默认显示设置；其他应用可在高级设置修改包名，需先在手机打开过。一次显示一个应用，多个应用并行尚未完成。"));
    statistics_=new QLabel; statistics_->setObjectName("detailStatus"); statistics_->setWordWrap(true);
    statistics_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); shell_->advancedLayout->addWidget(statistics_);
    status_=shell_->status;
    connect(shell_->receiveButton,&QPushButton::clicked,this,&CollaborationWindow::toggleReceiving);
    connect(&session_,&CollaborationSession::incomingPeerChanged,this,[this](QHostAddress local,QHostAddress phone) {
        localAddress_=local; phoneAddress_=phone;
    });
    connect(shell_,&LinkShell::modeRequested,this,[this](bool collab) {
        if(collab) return;
        switchToSmartcast();
    });
    connect(&session_,&CollaborationSession::status,this,[this](QString text) { status_->setText(text); qInfo().noquote()<<text; });
    connect(&session_,&CollaborationSession::activeChanged,this,[this,mute](bool active) {
        shell_->setMediaActive(active);
        back_->setEnabled(active);
        application_->setEnabled(session_.applicationReady());
        package_->setEnabled(active);
        mute->setEnabled(active); shell_->connectionState->setText(active ? QStringLiteral("已连接") : QStringLiteral("未连接"));
        if(receiverEnabled_) {
            shell_->receiveButton->setText(active ? QStringLiteral("断开连接") : QStringLiteral("暂停接收"));
            shell_->receiverState->setText(active ? QStringLiteral("●  手机已连接") : QStringLiteral("●  接收已开启"));
        }
        text_->setEnabled(session_.editable()); sendText_->setEnabled(session_.editable());
        if(!active) { displayed_.reset(); view_->hide(); shell_->setWaitingVisible(true); statistics_->clear(); }
    });
    connect(&session_,&CollaborationSession::stopped,this,[this] {
        const bool resume=receiverEnabled_ && !closing_ && !switchingToSmartcast_;
        receiverEnabled_=false; shell_->setReceiving(false);
        package_->setEnabled(true);
        if(closing_) { emit shutdownFinished(); if(isWindow()) close(); }
        else if(switchingToSmartcast_) finishModeSwitch();
        else if(resume) QTimer::singleShot(0,this,[this] { if(isVisible() && !closing_ && !switchingToSmartcast_) receivePhone(); });
    });
    connect(back_,&QPushButton::clicked,this,[this] { session_.key(4); });
    connect(files,&QPushButton::clicked,this,[this] {
        if(!session_.running()) { status_->setText(QStringLiteral("请先连接手机")); return; }
        if(auto existing=findChild<FileBrowser *>("nubiaFileBrowser",Qt::FindDirectChildrenOnly)) {
            existing->show(); existing->raise(); return;
        }
        auto browser=new FileBrowser(localAddress_,phoneAddress_,this); browser->setAttribute(Qt::WA_DeleteOnClose); browser->show();
    });
    display_.setInterval(16);
    connect(&display_,&QTimer::timeout,this,[this] {
        if(auto frame=session_.takeFrame()) {
            const bool first=!displayed_;
            displayed_=std::move(frame); fitView(); view_->show(); shell_->setWaitingVisible(false);
            view_->setColorSpace(displayed_->bt709,displayed_->fullRange); view_->setFrameSize(displayed_->size);
            const int w=displayed_->size.width();
            view_->updateTextures(reinterpret_cast<quint8 *>(displayed_->y.data()),reinterpret_cast<quint8 *>(displayed_->u.data()),
                reinterpret_cast<quint8 *>(displayed_->v.data()),w,(w+1)/2,(w+1)/2);
            if(first) qInfo("Collaboration GUI received frame: %d x %d",w,displayed_->size.height());
            shell_->connectionState->setText(session_.castDisplayType()=="virtualDisplay" ? QStringLiteral("独立应用") : QStringLiteral("手机主屏"));
            statistics_->setText(QStringLiteral("%1 · %2 × %3 · 已解码 %4 帧")
                .arg(session_.castDisplayType()=="virtualDisplay" ? QStringLiteral("独立应用") : QStringLiteral("手机主屏镜像"))
                .arg(w).arg(displayed_->size.height()).arg(session_.frameCount()));
        }
    });
    display_.start(); fitView();
}
void CollaborationWindow::switchToSmartcast() {
    if(switchingToSmartcast_) return;
    switchingToSmartcast_=true; receiverEnabled_=false; shell_->setReceiving(false); releasePointer();
    if(session_.running()) session_.stop();
    else finishModeSwitch();
}
void CollaborationWindow::finishModeSwitch() {
    switchingToSmartcast_=false;
    emit smartcastRequested();
}
void CollaborationWindow::toggleReceiving() {
    if(receiverEnabled_) {
        receiverEnabled_=false; shell_->setReceiving(false); session_.stop(); return;
    }
    const auto interface=LinkShell::receiverInterface();
    if(interface.address.isNull()) { status_->setText(QStringLiteral("未找到可用的 Wi-Fi / 以太网接口")); return; }
    receiverEnabled_=session_.listen(interface.address,interface.prefix,interface.name);
    shell_->setReceiving(receiverEnabled_);
}
void CollaborationWindow::receivePhone() {
    if(session_.running()) return;
    if(!receiverEnabled_) toggleReceiving();
}
CollaborationWindow::~CollaborationWindow() { releasePointer(); QObject::disconnect(&session_,nullptr,this,nullptr); session_.stop(); }
void CollaborationWindow::connectPhone(QString phone,QString local,QString package) {
    if(session_.running()) return;
    const QHostAddress target(phone); QHostAddress source(local);
    if(local.isEmpty()) {
        int prefix=-1;
        for(const auto &interface:QNetworkInterface::allInterfaces()) {
            if(!interface.flags().testFlag(QNetworkInterface::IsUp)) continue;
            for(const auto &entry:interface.addressEntries())
                if(entry.ip().protocol()==QAbstractSocket::IPv4Protocol && entry.prefixLength()>prefix && target.isInSubnet(entry.ip(),entry.prefixLength())) {
                    source=entry.ip(); prefix=entry.prefixLength();
                }
        }
    }
    package_->setText(package); localAddress_=source; phoneAddress_=target;
    if(session_.start(source,target,package)) {
        package_->setEnabled(false);
    }
}
void CollaborationWindow::fitView() {
    placeholder_->setGeometry(stage_->rect());
    if(!displayed_) return;
    shell_->setFrameSize(displayed_->size);
    const auto size=displayed_->size.scaled(stage_->size(),Qt::KeepAspectRatio);
    const QRect geometry((stage_->width()-size.width())/2,(stage_->height()-size.height())/2,size.width(),size.height());
    if(view_->geometry()!=geometry) { releasePointer(); view_->setGeometry(geometry); }
}
bool CollaborationWindow::eventFilter(QObject *object,QEvent *event) {
    if(object==stage_ && event->type()==QEvent::Resize) fitView();
    if(object==view_) {
        if(event->type()==QEvent::MouseButtonPress || event->type()==QEvent::MouseButtonRelease || event->type()==QEvent::MouseMove) {
            auto mouse=static_cast<QMouseEvent *>(event);
            if(!session_.mouseReady() || !displayed_) return false;
            pointerPosition_=mouse->position();
            if(event->type()==QEvent::MouseButtonPress && mouse->button()==Qt::RightButton) { session_.key(4); return true; }
            if(event->type()==QEvent::MouseButtonPress && mouse->button()==Qt::LeftButton) {
                pointerDown_=true; view_->setFocus(); session_.pointer(0,pointerPosition_,view_->size(),1); return true;
            }
            if(event->type()==QEvent::MouseButtonRelease && mouse->button()==Qt::LeftButton) { releasePointer(); return true; }
            if(event->type()==QEvent::MouseMove && pointerDown_) { session_.pointer(2,pointerPosition_,view_->size(),1); return true; }
        } else if(event->type()==QEvent::Wheel && displayed_) {
            auto wheel=static_cast<QWheelEvent *>(event);
            session_.scroll(wheel->position(),view_->size(),QPointF(wheel->angleDelta())/120.0); return true;
        } else if(event->type()==QEvent::FocusOut || event->type()==QEvent::Hide) releasePointer();
    }
    return QMainWindow::eventFilter(object,event);
}
bool CollaborationWindow::event(QEvent *event) {
    if(event->type()==QEvent::WindowDeactivate && pointerDown_) releasePointer();
    return QMainWindow::event(event);
}
void CollaborationWindow::releasePointer() {
    if(!pointerDown_) return;
    session_.pointer(1,pointerPosition_,view_->size(),0); pointerDown_=false;
}
void CollaborationWindow::closeEvent(QCloseEvent *event) {
    if(session_.running()) { event->ignore(); stopForClose(); }
    else QMainWindow::closeEvent(event);
}
void CollaborationWindow::stopForClose() {
    closing_=true; receiverEnabled_=false; switchingToSmartcast_=false;
    shell_->setReceiving(false); releasePointer();
    if(session_.running()) session_.stop();
    else emit shutdownFinished();
}
