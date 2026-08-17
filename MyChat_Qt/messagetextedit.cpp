#include "messagetextedit.h"
#include <QDebug>
#include <QMessageBox>
#include <QClipboard>
#include <QApplication>

MessageTextEdit::MessageTextEdit(QWidget *parent)
    : QTextEdit(parent)
{
    //this->setStyleSheet("border: none;");
    this->setMaximumHeight(60);

    //    connect(this,SIGNAL(textChanged()),this,SLOT(textEditChanged()));
}

MessageTextEdit::~MessageTextEdit()
{

}

QVector<MsgInfo> MessageTextEdit::getMsgList()
{
    mGetMsgList.clear();

    QString doc = this->document()->toPlainText();	// 转成纯文本(包括表情和图片)
    QString text="";//存储文本信息
    int indexUrl = 0;
    int count = mMsgList.size();

    for(int index=0; index<doc.size(); index++)
    {
        if(doc[index]==QChar::ObjectReplacementCharacter)	// 遇到“占位符”，说明这里埋了一张图
        {
            if(!text.isEmpty())
            {
                QPixmap pix;   // 空图，仅占位
                insertMsgList(mGetMsgList,"text",text,pix);
                text.clear();
            }
            while(indexUrl<count)	// 在前一次扫描位置继续找图
            {
                MsgInfo msg =  mMsgList[indexUrl];
                if(this->document()->toHtml().contains(msg.content,Qt::CaseSensitive))	//判断这张图是否存在于文档
                {
                    indexUrl++;
                    mGetMsgList.append(msg);
                    break;
                }
                indexUrl++;
            }
        }
        else
        {
            // 普通字符——继续攒文字
            text.append(doc[index]);
        }
    }
    if(!text.isEmpty())
    {
        QPixmap pix;
        insertMsgList(mGetMsgList,"text",text,pix);
        text.clear();
    }
    mMsgList.clear();   // 已经按顺序搬到 mGetMsgList，原列表不要了
    this->clear();      // 编辑器清空，准备下一次输入
    return mGetMsgList;
}

void MessageTextEdit::dragEnterEvent(QDragEnterEvent *event)
{
    if(event->source()==this)   // 拖动源如果是自己内部，就不给拖
        event->ignore();
    else
        event->accept();    // 进入 dropEvent
}

void MessageTextEdit::dropEvent(QDropEvent *event)
{
    insertFromMimeData(event->mimeData());      // 根据拖入的东西，解析类型
    event->accept();            // 表示“我处理完了”，停止事件传递
}

void MessageTextEdit::keyPressEvent(QKeyEvent *e)
{
    if ((e->modifiers() & Qt::ControlModifier) && e->key() == Qt::Key_V) {
        QClipboard *clipboard = QApplication::clipboard();
        insertPlainText(clipboard->text());
        return;
    }

    // 判断按的是“回车”还是“小键盘回车”, Shift+回车走 else 分支，会插入换行符。
    if((e->key()==Qt::Key_Enter||e->key()==Qt::Key_Return)&& !(e->modifiers() & Qt::ShiftModifier))
    {
        emit send();
        return; // 阻止插入换行
    }
    QTextEdit::keyPressEvent(e);
}


void MessageTextEdit::insertFileFromUrl(const QStringList &urls)
{
    if(urls.isEmpty())
        return;

    foreach (QString url, urls){
         if(isImage(url))
             insertImages(url);
         else
             insertTextFile(url);
    }
}

void MessageTextEdit::insertImages(const QString &url)
{
    QImage image(url);
    //按比例缩放图片
    if(image.width()>120||image.height()>80)
    {
        if(image.width()>image.height())
        {
          image =  image.scaledToWidth(120,Qt::SmoothTransformation);
        }
        else
            image = image.scaledToHeight(80,Qt::SmoothTransformation);
    }
    QTextCursor cursor = this->textCursor();
    // QTextDocument *document = this->document();
    // document->addResource(QTextDocument::ImageResource, QUrl(url), QVariant(image));
    cursor.insertImage(image,url);

    insertMsgList(mMsgList,"image",url,QPixmap::fromImage(image));
}

void MessageTextEdit::insertTextFile(const QString &url)
{
    QFileInfo fileInfo(url);
    if(fileInfo.isDir())
    {
        QMessageBox::information(this,"提示","只允许拖拽单个文件!");
        return;
    }

    if(fileInfo.size()>100*1024*1024)
    {
        QMessageBox::information(this,"提示","发送的文件大小不能大于100M");
        return;
    }

    QPixmap pix = getFileIconPixmap(url);
    QTextCursor cursor = this->textCursor();
    cursor.insertImage(pix.toImage(),url);
    insertMsgList(mMsgList,"file",url,pix);
}

bool MessageTextEdit::canInsertFromMimeData(const QMimeData *source) const
{
    return QTextEdit::canInsertFromMimeData(source);
}

void MessageTextEdit::insertFromMimeData(const QMimeData *source)
{
    QStringList urls = getUrl(source->text());

    if(urls.isEmpty())
        return;

    foreach (QString url, urls)
    {
         if(isImage(url))
             insertImages(url);
         else
             insertTextFile(url);
    }
}

bool MessageTextEdit::isImage(QString url)
{
    QString imageFormat = "bmp,jpg,png,tif,gif,pcx,tga,exif,fpx,svg,psd,cdr,pcd,dxf,ufo,eps,ai,raw,wmf,webp";
    QStringList imageFormatList = imageFormat.split(",");
    QFileInfo fileInfo(url);
    QString suffix = fileInfo.suffix();
    if(imageFormatList.contains(suffix,Qt::CaseInsensitive)){
        return true;
    }
    return false;
}

void MessageTextEdit::insertMsgList(QVector<MsgInfo> &list, QString flag, QString text, QPixmap pix)
{
    MsgInfo msg;
    msg.msgFlag=flag;
    msg.content = text;
    msg.pixmap = pix;
    list.append(msg);
}

QStringList MessageTextEdit::getUrl(QString text)
{
    QStringList urls;
    if(text.isEmpty()) return urls;

    QStringList list = text.split("\n");
    foreach (QString url, list) {
        if(!url.isEmpty()){
            QStringList str = url.split("///");
            if(str.size()>=2)
                urls.append(str.at(1));
        }
    }
    return urls;
}

QPixmap MessageTextEdit::getFileIconPixmap(const QString &url)
{
    QFileIconProvider provder;
    QFileInfo fileinfo(url);
    QIcon icon = provder.icon(fileinfo);

    QString strFileSize = getFileSize(fileinfo.size());
    //qDebug() << "FileSize=" << fileinfo.size();

    QFont font(QString("宋体"),10,QFont::Normal,false);
    QFontMetrics fontMetrics(font);
    QSize textSize = fontMetrics.size(Qt::TextSingleLine, fileinfo.fileName());

    QSize FileSize = fontMetrics.size(Qt::TextSingleLine, strFileSize);
    int maxWidth = textSize.width() > FileSize.width() ? textSize.width() :FileSize.width();
    QPixmap pix(50 + maxWidth + 10, 50);
    pix.fill();

    QPainter painter;
   // painter.setRenderHint(QPainter::Antialiasing, true);
    //painter.setFont(font);
    painter.begin(&pix);
    // 文件图标
    QRect rect(0, 0, 50, 50);
    painter.drawPixmap(rect, icon.pixmap(40,40));
    painter.setPen(Qt::black);
    // 文件名称
    QRect rectText(50+10, 3, textSize.width(), textSize.height());
    painter.drawText(rectText, fileinfo.fileName());
    // 文件大小
    QRect rectFile(50+10, textSize.height()+5, FileSize.width(), FileSize.height());
    painter.drawText(rectFile, strFileSize);
    painter.end();
    return pix;
}

QString MessageTextEdit::getFileSize(qint64 size)
{
    QString Unit;
    double num;
    if(size < 1024){
        num = size;
        Unit = "B";
    }
    else if(size < 1024 * 1224){
        num = size / 1024.0;
        Unit = "KB";
    }
    else if(size <  1024 * 1024 * 1024){
        num = size / 1024.0 / 1024.0;
        Unit = "MB";
    }
    else{
        num = size / 1024.0 / 1024.0/ 1024.0;
        Unit = "GB";
    }
    return QString::number(num,'f',2) + " " + Unit;
}

void MessageTextEdit::textEditChanged()
{
    //qDebug() << "text changed!" << endl;
}
