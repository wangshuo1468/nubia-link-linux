#include "window.h"
#include "file_browser.h"
#include "collaboration_window.h"
#include "link_shell.h"
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCheckBox>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QNetworkInterface>
#include <QMouseEvent>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {
int androidKey(int key) {
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return 29 + key - Qt::Key_A;
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return 7 + key - Qt::Key_0;
    switch (key) {
    case Qt::Key_Escape: return 4;
    case Qt::Key_Tab: case Qt::Key_Backtab: return 61;
    case Qt::Key_Return: case Qt::Key_Enter: return 66;
    case Qt::Key_Backspace: return 67;
    case Qt::Key_Delete: return 112;
    case Qt::Key_Up: return 19; case Qt::Key_Down: return 20;
    case Qt::Key_Left: return 21; case Qt::Key_Right: return 22;
    case Qt::Key_PageUp: return 92; case Qt::Key_PageDown: return 93;
    case Qt::Key_Home: return 122; case Qt::Key_End: return 123;
    case Qt::Key_Space: return 62;
    default: return -1;
    }
}
quint32 androidModifiers(Qt::KeyboardModifiers modifiers) {
    return (modifiers.testFlag(Qt::ShiftModifier) ? 1u : 0u)
        | (modifiers.testFlag(Qt::AltModifier) ? 2u : 0u)
        | (modifiers.testFlag(Qt::ControlModifier) ? 4096u : 0u)
        | (modifiers.testFlag(Qt::MetaModifier) ? 65536u : 0u);
}
void tap(ProjectionSession &session, quint32 code) { session.send(Nubia::key(0, code)); session.send(Nubia::key(1, code)); }
}
PhoneView::PhoneView(QWidget *parent) : QYUVOpenGLWidget(parent) {
    setFocusPolicy(Qt::StrongFocus); setAttribute(Qt::WA_InputMethodEnabled); setMouseTracking(true);
}
void PhoneView::showFrame(const std::shared_ptr<VideoFrame> &frame) {
    displayed_ = frame; videoSize_ = frame->size;
    setColorSpace(frame->bt709, frame->fullRange);
    setFrameSize(videoSize_);
    setSourceRect(frame->visibleRect());
    updateTextures(reinterpret_cast<quint8 *>(displayed_->y.data()), reinterpret_cast<quint8 *>(displayed_->u.data()),
                   reinterpret_cast<quint8 *>(displayed_->v.data()), videoSize_.width(), (videoSize_.width()+1)/2, (videoSize_.width()+1)/2);
}
QPoint PhoneView::phonePoint(QPointF point) const {
    const auto rect=displayed_->visibleRect();
    return QPoint(rect.x()+qBound(0,int(point.x()*rect.width()/qMax(1,width())),rect.width()-1),
                  rect.y()+qBound(0,int(point.y()*rect.height()/qMax(1,height())),rect.height()-1));
}
void PhoneView::mousePressEvent(QMouseEvent *event) {
    if (!videoSize_.isValid()) return;
    setFocus();
    if (event->button() == Qt::RightButton) { emit control(Nubia::key(0,4)); emit control(Nubia::key(1,4)); }
    else if (event->button() == Qt::LeftButton) {
        lastPoint_ = phonePoint(event->position()); touching_ = true;
        emit control(Nubia::touch(0, lastPoint_, videoSize_));
    }
    event->accept();
}
void PhoneView::mouseMoveEvent(QMouseEvent *event) {
    if (touching_) { lastPoint_ = phonePoint(event->position()); emit control(Nubia::touch(2, lastPoint_, videoSize_)); }
    event->accept();
}
void PhoneView::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && touching_) {
        lastPoint_ = phonePoint(event->position()); touching_ = false;
        emit control(Nubia::touch(1, lastPoint_, videoSize_, 0));
    }
    event->accept();
}
void PhoneView::wheelEvent(QWheelEvent *event) {
    if (videoSize_.isValid()) emit control(Nubia::scroll(phonePoint(event->position()), videoSize_,
                                                       event->angleDelta().x()/120, event->angleDelta().y()/120));
    event->accept();
}
void PhoneView::sendText(const QString &text) {
    if (!text.isEmpty()) emit control(Nubia::text(11, text));
}
void PhoneView::keyPressEvent(QKeyEvent *event) {
    const auto modifiers = event->modifiers();
    const bool shortcut = modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::AltModifier) || modifiers.testFlag(Qt::MetaModifier);
    if (!shortcut && !event->text().isEmpty() && event->text().front().isPrint()) sendText(event->text());
    else {
        const auto code = androidKey(event->key());
        if (code >= 0) { heldKeys_.insert(event->key()); emit control(Nubia::key(0, quint32(code), androidModifiers(modifiers))); }
    }
    event->accept();
}
void PhoneView::keyReleaseEvent(QKeyEvent *event) {
    if (!event->isAutoRepeat() && heldKeys_.remove(event->key()))
        emit control(Nubia::key(1, quint32(androidKey(event->key())), androidModifiers(event->modifiers())));
    event->accept();
}
void PhoneView::inputMethodEvent(QInputMethodEvent *event) { sendText(event->commitString()); event->accept(); }
QVariant PhoneView::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImEnabled) return true;
    if (query == Qt::ImCursorRectangle) return QRect(width()/2, height()/2, 1, 20);
    if (query == Qt::ImSurroundingText || query == Qt::ImCurrentSelection) return QString();
    if (query == Qt::ImCursorPosition || query == Qt::ImAnchorPosition) return 0;
    return QYUVOpenGLWidget::inputMethodQuery(query);
}
void PhoneView::releaseInput() {
    if (touching_) { touching_ = false; emit control(Nubia::touch(1, lastPoint_, videoSize_, 0)); }
    for (auto key : heldKeys_) emit control(Nubia::key(1, quint32(androidKey(key))));
    heldKeys_.clear();
}
void PhoneView::focusOutEvent(QFocusEvent *event) { releaseInput(); QYUVOpenGLWidget::focusOutEvent(event); }

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("努比亚连接 · 智慧投屏")); resize(1280,800); setMinimumSize(760,620);
    pages_=new QStackedWidget(this); setCentralWidget(pages_);
    shell_=new LinkShell(false,pages_); pages_->addWidget(shell_);
    connect(shell_,&LinkShell::modeRequested,this,[this](bool collab) { if(collab) openCollaboration(phoneAddress_); });
    connect(shell_->files,&QPushButton::clicked,this,[this] { openFiles(phoneAddress_); });
    expanded_ = new QCheckBox(QStringLiteral("扩展视野"));
    expanded_->setToolTip(QStringLiteral("手机原厂扩展视野开关，需要实机验证；不代表多个独立应用窗口。"));
    mute_ = new QPushButton; mute_->setCheckable(true); mute_->setIcon(LinkShell::icon("volume")); mute_->setFixedWidth(36); mute_->setToolTip(QStringLiteral("静音"));
    audioState_ = new QLabel; audioState_->setObjectName("detailStatus"); audioState_->setWordWrap(true);
    audioState_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); shell_->contentLayout->addWidget(audioState_);
    stage_ = shell_->stage; stage_->installEventFilter(this);
    view_ = new PhoneView(stage_); view_->hide();
    placeholder_ = shell_->placeholder;
    auto nav = shell_->controls;
    const QList<QPair<QString,quint32>> actions = {{QStringLiteral("返回"),4},{QStringLiteral("主页"),3},{QStringLiteral("最近任务"),187}};
    for (const auto &action : actions) {
        auto button = new QPushButton; button->setToolTip(action.first); button->setFixedWidth(36);
        button->setIcon(LinkShell::icon(action.second==4 ? "back" : action.second==3 ? "home" : "recent"));
        navigation_.append(button); nav->addWidget(button);
        connect(button,&QPushButton::clicked,this,[this,code=action.second] { tap(session_,code); view_->setFocus(); });
    }
    nav->addWidget(mute_);
    auto more=new QPushButton; more->setToolTip(QStringLiteral("更多")); more->setObjectName("moreControls"); more->setFixedWidth(36); more->setIcon(LinkShell::icon("more")); more->setCheckable(true); nav->addWidget(more);
    nav->addStretch(); auto hint=new QLabel(QStringLiteral("连接后可使用工具")); hint->setObjectName("muted"); nav->addWidget(hint);
    auto extras=new QWidget; auto extraLayout=new QHBoxLayout(extras); extraLayout->setContentsMargins(0,0,0,0);
    pushClipboard_ = new QPushButton(QStringLiteral("剪贴板 → 手机")); getClipboard_ = new QPushButton(QStringLiteral("读取手机剪贴板"));
    extraLayout->addWidget(pushClipboard_); extraLayout->addWidget(getClipboard_); extraLayout->addWidget(expanded_); extraLayout->addStretch();
    shell_->contentLayout->insertWidget(shell_->contentLayout->count()-1,extras); extras->hide();
    connect(more,&QPushButton::toggled,extras,&QWidget::setVisible);
    details_ = shell_->help;
    details_->setText(QStringLiteral("手机与电脑连接同一 Wi-Fi，在手机上确认共享屏幕。多屏协同使用独立入口，切换时会结束智慧投屏。开启接收后，手机发起连接会自动接通；剪贴板读取和扩展视野也需兼容性验证。"));
    state_ = shell_->status;
    connect(shell_->receiveButton,&QPushButton::clicked,this,&MainWindow::toggleReceiving);
    connect(&lan_,&LanProjection::incomingPeerChanged,this,[this](QHostAddress phone) {
        phoneAddress_=phone.toString();
    });
    connect(&session_,&ProjectionSession::status,state_,&QLabel::setText);
    connect(&lan_,&LanProjection::status,state_,&QLabel::setText);
    connect(&lan_,&LanProjection::status,this,[](const QString &text) { qInfo().noquote()<<text; });
    connect(&lan_,&LanProjection::status,this,[this] {
        if(receiverEnabled_ && !lan_.running() && !lebo_.running()) {
            receiverEnabled_=false; shell_->setReceiving(false);
        }
    });
    connect(&lebo_,&LeboReceiver::status,state_,&QLabel::setText);
    connect(&lebo_,&LeboReceiver::status,this,[](const QString &text) { qInfo().noquote()<<text; });
    connect(&lebo_,&LeboReceiver::publicationFailed,this,[this](QString message) {
        receiverEnabled_=false; lan_.stop(); shell_->setReceiving(false);
        state_->setText(message);
    });
    connect(&lebo_,&LeboReceiver::peerChanged,this,[this](QHostAddress phone) { phoneAddress_=phone.toString(); });
    connect(&lebo_,&LeboReceiver::activeChanged,this,[this](bool active) {
        if(active) lan_.stop();
        setActive(active);
        if(active) audioState_->setText(QStringLiteral("等待手机声音 · 电脑反控尚未实现"));
        else if(receiverEnabled_) {
            const auto interface=LinkShell::receiverInterface();
            if(!interface.address.isNull()) lan_.listen(interface.address,interface.prefix,interface.name);
        }
    });
    connect(&lebo_,&LeboReceiver::videoPacket,&decoder_,&VideoDecoder::submitSmartcast);
    connect(&lebo_,&LeboReceiver::audioData,&audio_,&PhoneAudio::append);
    connect(&lebo_,&LeboReceiver::audioStatus,audioState_,&QLabel::setText);
    connect(&session_,&ProjectionSession::activeChanged,this,&MainWindow::setActive);
    connect(&session_,&ProjectionSession::videoPacket,&decoder_,&VideoDecoder::submit);
    connect(&session_,&ProjectionSession::audioData,&audio_,&PhoneAudio::append);
    connect(&session_,&ProjectionSession::deviceMessage,this,&MainWindow::deviceMessage);
    connect(&decoder_,&VideoDecoder::error,this,[this](const QString &text,quint64 epoch) {
        if (epoch != decoder_.epoch()) return;
        if(lebo_.running()) { lebo_.disconnectPhone(); decoder_.reset(); }
        else lan_.stop();
        state_->setText(text);
    },Qt::QueuedConnection);
    connect(&audio_,&PhoneAudio::warning,audioState_,&QLabel::setText);
    connect(view_,&PhoneView::control,&session_,&ProjectionSession::send);
    connect(expanded_,&QCheckBox::toggled,this,[this](bool enabled) { session_.send(Nubia::text(13, enabled ? "1" : "0")); });
    connect(mute_,&QPushButton::toggled,&audio_,&PhoneAudio::setMuted);
    connect(mute_,&QPushButton::toggled,this,[this](bool muted) { mute_->setIcon(LinkShell::icon(muted ? "muted" : "volume")); });
    connect(pushClipboard_,&QPushButton::clicked,this,[this] {
        const auto text = QApplication::clipboard()->text();
        if (text.toUtf8().size() > Nubia::MaxText) { state_->setText(QStringLiteral("剪贴板文本超过原厂协议单次上限（4092 字节）")); return; }
        session_.send(Nubia::clipboard(text,false)); state_->setText(QStringLiteral("已发送剪贴板文本，等待实机确认"));
    });
    clipboardTimeout_.setSingleShot(true); clipboardTimeout_.setInterval(3000);
    connect(&clipboardTimeout_,&QTimer::timeout,this,[this] {
        clipboardRequested_ = false;
        state_->setText(QStringLiteral("未收到手机剪贴板文本，可能为空或受系统限制"));
    });
    connect(getClipboard_,&QPushButton::clicked,this,[this] {
        clipboardRequested_ = true; clipboardTimeout_.start();
        session_.send(QByteArray(1,char(7)));
        state_->setText(QStringLiteral("正在读取手机剪贴板文本"));
    });
    displayTimer_.setInterval(16);
    connect(&displayTimer_,&QTimer::timeout,this,[this] {
        if (auto frame = decoder_.takeFrame()) {
            const bool changed = frameSize_ != frame->displaySize();
            if (changed) view_->releaseInput();
            frameSize_ = frame->displaySize(); fitView(); view_->show(); shell_->setWaitingVisible(false); view_->showFrame(frame);
            if (changed) {
                state_->setText(QStringLiteral("正在接收手机画面 · %1 × %2").arg(frameSize_.width()).arg(frameSize_.height()));
                qInfo("Receiving phone video: %d x %d", frameSize_.width(), frameSize_.height());
            }
        }
    });
    displayTimer_.start(); setActive(false);
}
MainWindow::~MainWindow() {
    if(collaborationPage_) {
        QObject::disconnect(collaborationPage_,nullptr,this,nullptr);
        delete collaborationPage_; collaborationPage_=nullptr;
    }
    view_->releaseInput();
    // Member destructors run before QObject removes signal connections. The
    // LAN controller may stop media after UI state/decoder members are gone.
    QObject::disconnect(&session_,nullptr,nullptr,nullptr);
    QObject::disconnect(&lan_,nullptr,nullptr,nullptr);
    QObject::disconnect(&lebo_,nullptr,nullptr,nullptr);
    lebo_.stop(); lan_.stop(); audio_.stop();
}
void MainWindow::connectDevice(const QString &serial) {
    if(collaborationPage_ && collaborationPage_->running()) {
        state_->setText(QStringLiteral("请先在多屏协同窗口断开连接，再使用智慧投屏")); return;
    }
    if (serial.isEmpty() || connecting_ || session_.connected()) return;
    decoder_.reset(); session_.startAdb(serial,expanded_->isChecked());
    connecting_ = session_.port() != 0;
}
void MainWindow::toggleReceiving() {
    if(receiverEnabled_) {
        if(lebo_.active()) {
            view_->releaseInput(); lebo_.disconnectPhone();
            state_->setText(QStringLiteral("已断开，继续等待手机连接")); return;
        }
        if(session_.connected()) {
            view_->releaseInput(); lan_.stop();
            const auto interface=LinkShell::receiverInterface();
            if(!interface.address.isNull()) lan_.listen(interface.address,interface.prefix,interface.name);
            state_->setText(QStringLiteral("已断开，继续等待手机连接")); return;
        }
        receiverEnabled_=false; view_->releaseInput(); lebo_.stop(); lan_.stop(); shell_->setReceiving(false);
        state_->setText(QStringLiteral("接收已暂停")); return;
    }
    const auto interface=LinkShell::receiverInterface();
    if(interface.address.isNull()) { state_->setText(QStringLiteral("未找到可用的 Wi-Fi / 以太网接口")); return; }
    receiverEnabled_=lebo_.listen(interface.address,interface.prefix,interface.name);
    if(receiverEnabled_) {
        lan_.listen(interface.address,interface.prefix,interface.name);
        state_->setText(QStringLiteral("接收已开启，在手机“智慧投屏 → 搜索设备”选择 Nubia Link Linux"));
    }
    shell_->setReceiving(receiverEnabled_);
}
void MainWindow::receivePhone() {
    if(!receiverEnabled_) toggleReceiving();
}
void MainWindow::connectLan(const QString &phone,const QString &local) {
    if(collaborationPage_ && collaborationPage_->running()) {
        state_->setText(QStringLiteral("请先在多屏协同窗口断开连接，再使用智慧投屏")); return;
    }
    if(connecting_ || session_.connected()) return;
    const QHostAddress target(phone);
    QHostAddress source(local);
    if(local.isEmpty()) {
        int bestPrefix=-1;
        for(const auto &interface:QNetworkInterface::allInterfaces()) {
            if(!interface.flags().testFlag(QNetworkInterface::IsUp)) continue;
            for(const auto &entry:interface.addressEntries()) {
                if(entry.ip().protocol()==QAbstractSocket::IPv4Protocol && entry.prefixLength()>bestPrefix &&
                   target.isInSubnet(entry.ip(),entry.prefixLength())) {
                    source=entry.ip(); bestPrefix=entry.prefixLength();
                }
            }
        }
    }
    if(target.protocol()!=QAbstractSocket::IPv4Protocol || source.protocol()!=QAbstractSocket::IPv4Protocol) {
        state_->setText(QStringLiteral("请输入手机 Wi-Fi IPv4 地址，并让手机与电脑连接同一局域网")); return;
    }
    phoneAddress_=phone; decoder_.reset();
    connecting_=lan_.start(source,target) && session_.port();
}
void MainWindow::openCollaboration(const QString &phone,const QString &local,bool autoConnect,QString package) {
    if(closing_) return;
    receiverEnabled_=false; shell_->setReceiving(false);
    view_->releaseInput(); lebo_.stop(); lan_.stop(); session_.stop();
    auto window=ensureCollaborationPage(); pages_->setCurrentWidget(window);
    setWindowTitle(QStringLiteral("努比亚连接 · 多屏协同"));
    if(autoConnect) window->connectPhone(phone,local,package);
    else window->receivePhone();
}
CollaborationWindow *MainWindow::ensureCollaborationPage() {
    if(!collaborationPage_) {
        collaborationPage_=new CollaborationWindow(pages_);
        pages_->addWidget(collaborationPage_);
        connect(collaborationPage_,&CollaborationWindow::smartcastRequested,this,&MainWindow::showSmartcast);
        connect(collaborationPage_,&CollaborationWindow::shutdownFinished,this,[this] {
            // A listening-only session can stop synchronously inside closeEvent.
            // QWidget ignores recursive close() calls until that event returns.
            if(closing_) QTimer::singleShot(0,this,&QWidget::close);
        });
    }
    return collaborationPage_;
}
void MainWindow::showSmartcast() {
    if(closing_) return;
    pages_->setCurrentWidget(shell_);
    setWindowTitle(QStringLiteral("努比亚连接 · 智慧投屏"));
    receivePhone();
}
void MainWindow::closeEvent(QCloseEvent *event) {
    closing_=true;
    if(collaborationPage_ && collaborationPage_->running()) {
        event->ignore(); collaborationPage_->stopForClose();
    } else QMainWindow::closeEvent(event);
}
void MainWindow::openFiles(const QString &phone,const QString &local) {
    const QHostAddress target(phone); QHostAddress source(local);
    if(local.isEmpty()) {
        int bestPrefix=-1;
        for(const auto &interface:QNetworkInterface::allInterfaces()) {
            if(!interface.flags().testFlag(QNetworkInterface::IsUp)) continue;
            for(const auto &entry:interface.addressEntries()) {
                if(entry.ip().protocol()==QAbstractSocket::IPv4Protocol && entry.prefixLength()>bestPrefix &&
                   target.isInSubnet(entry.ip(),entry.prefixLength())) { source=entry.ip(); bestPrefix=entry.prefixLength(); }
            }
        }
    }
    if(target.protocol()!=QAbstractSocket::IPv4Protocol || source.protocol()!=QAbstractSocket::IPv4Protocol) {
        state_->setText(QStringLiteral("文件互传需要手机 Wi-Fi IPv4 地址及同一局域网")); return;
    }
    // Each phone exposes a single DFS remote identity. Reuse this window while
    // it is open so a second click cannot replace a transfer's identity.
    if(auto existing=findChild<FileBrowser *>("nubiaFileBrowser",Qt::FindDirectChildrenOnly)) {
        existing->show(); existing->raise(); existing->activateWindow(); return;
    }
    auto browser=new FileBrowser(source,target,this); browser->setAttribute(Qt::WA_DeleteOnClose); browser->show();
}
void MainWindow::setActive(bool active) {
    shell_->setMediaActive(active);
    connecting_ = false;
    shell_->connectionState->setText(active ? QStringLiteral("已连接") : receiverEnabled_ ? QStringLiteral("等待手机") : QStringLiteral("未连接")); mute_->setEnabled(active); expanded_->setEnabled(active);
    if(receiverEnabled_) {
        shell_->receiveButton->setText(active ? QStringLiteral("断开连接") : QStringLiteral("暂停接收"));
        shell_->receiverState->setText(active ? QStringLiteral("●  手机已连接") : QStringLiteral("●  接收已开启"));
    }
    const bool controlAvailable=active && session_.connected();
    shell_->findChild<QPushButton *>("moreControls")->setEnabled(controlAvailable);
    for (auto button : navigation_) button->setEnabled(controlAvailable);
    pushClipboard_->setEnabled(controlAvailable); getClipboard_->setEnabled(controlAvailable);
    expanded_->setEnabled(controlAvailable);
    if (active) { audioState_->clear(); audio_.start(); }
    else {
        view_->releaseInput(); audio_.stop(); decoder_.reset(); frameSize_ = QSize(); clipboardRequested_ = false; clipboardTimeout_.stop();
        view_->hide(); shell_->setWaitingVisible(true); fitView();
    }
}
void MainWindow::fitView() {
    placeholder_->setGeometry(stage_->rect());
    if (!frameSize_.isValid()) return;
    shell_->setFrameSize(frameSize_);
    const auto size = frameSize_.scaled(stage_->size(),Qt::KeepAspectRatio);
    view_->setGeometry((stage_->width()-size.width())/2,(stage_->height()-size.height())/2,size.width(),size.height());
}
bool MainWindow::eventFilter(QObject *object,QEvent *event) {
    if (object == stage_ && event->type() == QEvent::Resize) fitView();
    return QMainWindow::eventFilter(object,event);
}
void MainWindow::deviceMessage(quint8 type,const QString &value) {
    if (type == 0 && clipboardRequested_) { QApplication::clipboard()->setText(value); clipboardRequested_ = false; clipboardTimeout_.stop(); state_->setText(QStringLiteral("已读取手机剪贴板")); }
    if (type == 6) details_->setText(QStringLiteral("智慧投屏组件 %1。电脑发起已实测画面、声音、键鼠及中文输入。开启接收后，手机发起连接会自动接通；多屏协同使用独立入口。").arg(value.left(100)));
}
