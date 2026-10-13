#include "file_browser.h"
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

FileBrowser::FileBrowser(QHostAddress local,QHostAddress phone,QWidget *parent):QDialog(parent) {
    setWindowTitle(QStringLiteral("努比亚 · 文件互传")); resize(760,540);
    setObjectName("nubiaFileBrowser");
    auto root=new QVBoxLayout(this);
    auto intro=new QLabel(QStringLiteral("浏览手机共享存储。选择文件可下载，上传文件会保留原文件名；手机有同名文件时拒绝覆盖。"));
    intro->setWordWrap(true); root->addWidget(intro);
    auto row=new QHBoxLayout;
    up_=new QPushButton(QStringLiteral("上一级")); reload_=new QPushButton(QStringLiteral("刷新"));
    reconnect_=new QPushButton(QStringLiteral("重新连接"));
    reconnect_->setObjectName("reconnectPhoneFiles");
    pathLabel_=new QLabel(QStringLiteral("手机共享存储 /"));
    row->addWidget(up_); row->addWidget(reload_); row->addWidget(pathLabel_,1); row->addWidget(reconnect_); root->addLayout(row);
    tree_=new QTreeWidget; tree_->setObjectName("phoneFileList");
    tree_->setHeaderLabels({QStringLiteral("名称"),QStringLiteral("类型"),QStringLiteral("大小")});
    tree_->setRootIsDecorated(false); tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setColumnWidth(0,410); root->addWidget(tree_,1);
    auto buttons=new QHBoxLayout; upload_=new QPushButton(QStringLiteral("上传到当前目录")); download_=new QPushButton(QStringLiteral("下载所选文件"));
    buttons->addWidget(upload_); buttons->addWidget(download_); buttons->addStretch(); root->addLayout(buttons);
    progress_=new QProgressBar; progress_->setRange(0,1000); progress_->setValue(0); root->addWidget(progress_);
    state_=new QLabel; state_->setWordWrap(true); root->addWidget(state_);
    connect(&session_,&FileSession::status,state_,&QLabel::setText);
    connect(&session_,&FileSession::available,this,[this](bool available) { updateControls(); if(available) refresh(); });
    auto &client=session_.client();
    connect(&client,&DfsClient::busyChanged,this,[this] { updateControls(); });
    connect(&client,&DfsClient::listing,this,[this](QString path,QList<Dfs::Entry> entries) {
        path_=path; entries_=std::move(entries); tree_->clear();
        pathLabel_->setText(QStringLiteral("手机共享存储 /")+path_);
        for(qsizetype i=0;i<entries_.size();++i) {
            const auto &entry=entries_[i];
            auto item=new QTreeWidgetItem(tree_,{entry.name,entry.directory() ? QStringLiteral("文件夹") : entry.regular() ? QStringLiteral("文件") : QStringLiteral("其他"),
                entry.directory() ? QString() : QString::number(entry.size)+QStringLiteral(" 字节")});
            item->setData(0,Qt::UserRole,i);
        }
        state_->setText(QStringLiteral("%1 项").arg(entries_.size())); updateControls();
    });
    connect(&client,&DfsClient::progress,this,[this](qint64 bytes,qint64 total) {
        progress_->setValue(total ? int((double(bytes)/double(total))*1000) : 1000);
        state_->setText(QStringLiteral("已传输 %1 / %2 字节").arg(bytes).arg(total));
    });
    connect(&client,&DfsClient::completed,this,[this](QString path) {
        progress_->setValue(1000); state_->setText(QStringLiteral("传输完成：")+path); refresh();
    });
    connect(&client,&DfsClient::operationFailed,state_,&QLabel::setText);
    connect(tree_,&QTreeWidget::itemSelectionChanged,this,&FileBrowser::updateControls);
    connect(tree_,&QTreeWidget::itemDoubleClicked,this,[this](QTreeWidgetItem *item,int) {
        if(!session_.client().ready() || session_.client().busy()) return;
        const auto index=item->data(0,Qt::UserRole).toInt();
        if(index<0 || index>=entries_.size() || !entries_[index].directory()) return;
        session_.client().list(path_.isEmpty() ? entries_[index].name : path_+"/"+entries_[index].name);
    });
    connect(up_,&QPushButton::clicked,this,[this] {
        const auto slash=path_.lastIndexOf('/'); session_.client().list(slash<0 ? QString() : path_.left(slash));
    });
    connect(reload_,&QPushButton::clicked,this,&FileBrowser::refresh);
    connect(reconnect_,&QPushButton::clicked,this,[this,local,phone] {
        if(!session_.start(local,phone)) state_->setText(QStringLiteral("文件服务正在结束，请稍后重试"));
        updateControls();
    });
    connect(upload_,&QPushButton::clicked,this,&FileBrowser::upload);
    connect(download_,&QPushButton::clicked,this,&FileBrowser::download);
    session_.start(local,phone); updateControls();
}
FileBrowser::~FileBrowser() { QObject::disconnect(&session_,nullptr,this,nullptr); QObject::disconnect(&session_.client(),nullptr,this,nullptr); }
void FileBrowser::refresh() { session_.client().list(path_); }
void FileBrowser::updateControls() {
    const bool ready=session_.client().ready() && !session_.client().busy();
    reconnect_->setEnabled(!session_.running());
    up_->setEnabled(ready && !path_.isEmpty()); reload_->setEnabled(ready); upload_->setEnabled(ready); tree_->setEnabled(ready);
    const auto items=tree_->selectedItems();
    const auto index=items.isEmpty() ? -1 : items.first()->data(0,Qt::UserRole).toInt();
    download_->setEnabled(ready && index>=0 && index<entries_.size() && entries_[index].regular());
}
void FileBrowser::upload() {
    const auto local=QFileDialog::getOpenFileName(this,QStringLiteral("选择上传文件")); if(local.isEmpty()) return;
    const auto name=QFileInfo(local).fileName();
    progress_->setValue(0); session_.client().upload(local,path_.isEmpty() ? name : path_+"/"+name);
}
void FileBrowser::download() {
    const auto items=tree_->selectedItems(); if(items.isEmpty()) return;
    const auto index=items.first()->data(0,Qt::UserRole).toInt(); if(index<0 || index>=entries_.size() || !entries_[index].regular()) return;
    const auto name=entries_[index].name;
    const auto local=QFileDialog::getSaveFileName(this,QStringLiteral("保存手机文件"),name); if(local.isEmpty()) return;
    progress_->setValue(0); session_.client().download(path_.isEmpty() ? name : path_+"/"+name,local);
}
void FileBrowser::closeEvent(QCloseEvent *event) {
    if(session_.client().busy()) { state_->setText(QStringLiteral("文件操作正在进行，请等待完成后关闭")); event->ignore(); return; }
    session_.stop(); QDialog::closeEvent(event);
}
