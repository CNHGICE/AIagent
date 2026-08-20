#include "aiagent.h"
#include "mathparser.h"
#include "ui_aiagent.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QPixmap>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTime>
#include <QTimer>
#include <QUrl>

// Ollama 服务地址与模型名（与 `ollama list` 中的名称保持一致）
static const QString kOllamaUrl = QStringLiteral("http://localhost:11434/api/chat");
static const QString kModelName = QStringLiteral("qwen2.5vl:7b");

// 系统指令：要求模型在回复开头输出情绪标签，程序解析后显示对应表情；
// 工具类请求时只输出一行工具指令，由程序本地执行
static const QString kSystemPrompt = QStringLiteral(
    "你是AI助手，请始终使用中文回复。每次回复时，第一行先输出情绪标签："
    "【情绪：开心】、【情绪：中性】或【情绪：难过】三选一，然后换行输出回复内容。"
    "不要输出多余格式。"
    "当用户要求进行数学计算、打开网址或打开文件夹时，忽略情绪标签要求，"
    "只输出一行工具指令，不要输出任何其他内容：\n"
    "- 计算：【计算】数学表达式，只写表达式不要自己计算，例如【计算】123*456\n"
    "- 打开网址：【网址】地址，例如【网址】https://www.baidu.com\n"
    "- 打开文件夹：【文件夹】路径，例如【文件夹】C:/Users/Public");

static QString truncateTitle(const QString &text)
{
    QString t = text.simplified();
    if (t.size() > 12)
        t = t.left(12) + QStringLiteral("…");
    return t;
}

static QString emojiForLabel(const QString &label)
{
    if (label == QStringLiteral("开心") || label == QStringLiteral("高兴")
        || label == QStringLiteral("愉快"))
        return QStringLiteral("😊");
    if (label == QStringLiteral("难过") || label == QStringLiteral("伤心")
        || label == QStringLiteral("悲伤"))
        return QStringLiteral("😟");
    return QStringLiteral("😐");
}

AIagent::AIagent(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::AIagent)
{
    ui->setupUi(this);
    ui->stackedWidget->setCurrentWidget(ui->menuPage);
    ui->suggestBar->hide();

    m_network = new QNetworkAccessManager(this);

    // 打字机效果：定时器把待显示缓冲逐字搬到显示文本
    m_typeTimer = new QTimer(this);
    m_typeTimer->setInterval(20);
    connect(m_typeTimer, &QTimer::timeout, this, &AIagent::onTypeTick);

    setStyleSheet(QStringLiteral(
        "QPushButton { background-color:#4A90D9; color:white; border:none;"
        " border-radius:8px; padding:8px 18px; }"
        "QPushButton:hover { background-color:#5BA3E8; }"
        "QPushButton:pressed { background-color:#3A7BC0; }"
        "QPushButton:disabled { background-color:#B0C4DE; }"
        "QLineEdit { border:1px solid #CCCCCC; border-radius:8px; padding:8px; }"
        "QLineEdit:focus { border:1px solid #4A90D9; }"
        "#suggestBar QPushButton { background-color:#EAF4FF; color:#4A90D9;"
        " border:1px solid #4A90D9; border-radius:14px; padding:4px 14px;"
        " font-size:9pt; }"
        "#suggestBar QPushButton:hover { background-color:#4A90D9; color:white; }"
        "#suggestBar QLabel { color:#888888; font-size:9pt; }"
        "#labelImagePreview { border:1px dashed #AAAAAA; border-radius:4px; }"
        "#convButton { background-color:#F5F7FA; color:#333333; border:1px solid #E0E0E0;"
        " border-radius:6px; padding:6px 12px; text-align:left; font-size:10pt; }"
        "#convButton:hover { background-color:#EAF4FF; border-color:#4A90D9; color:#4A90D9; }"
        "#labelHistory { color:#888888; font-size:9pt; }"));

    // ---- 主界面导航 ----
    connect(ui->btnNewChat, &QPushButton::clicked, this, &AIagent::onNewChat);
    connect(ui->btnNewChatTop, &QPushButton::clicked, this, &AIagent::onNewChat);
    connect(ui->btnAiChat, &QPushButton::clicked, this, &AIagent::onContinueChat);
    connect(ui->btnPersonChat, &QPushButton::clicked, this, [this] {
        ui->stackedWidget->setCurrentWidget(ui->personChatPage);
    });
    connect(ui->btnBack, &QPushButton::clicked, this, [this] {
        loadConversationList();
        ui->stackedWidget->setCurrentWidget(ui->menuPage);
    });
    connect(ui->btnBackPerson, &QPushButton::clicked, this, [this] {
        loadConversationList();
        ui->stackedWidget->setCurrentWidget(ui->menuPage);
    });

    // ---- AI 聊天 ----
    connect(ui->btnSend, &QPushButton::clicked, this, &AIagent::onSendMessage);
    connect(ui->msgInput, &QLineEdit::returnPressed, this, &AIagent::onSendMessage);
    // 推荐回复按钮：点击即发送
    connect(ui->btnSuggest1, &QPushButton::clicked, this, &AIagent::onSuggestClicked);
    connect(ui->btnSuggest2, &QPushButton::clicked, this, &AIagent::onSuggestClicked);
    connect(ui->btnSuggest3, &QPushButton::clicked, this, &AIagent::onSuggestClicked);

    // ---- 图片上传 ----
    connect(ui->btnUploadImage, &QPushButton::clicked, this, &AIagent::onUploadImage);
    connect(ui->btnClearImage, &QPushButton::clicked, this, &AIagent::onClearImage);

    // ---- 人与人聊天 ----
    connect(ui->comboPersonMode, &QComboBox::currentIndexChanged,
            this, &AIagent::onPersonModeChanged);
    connect(ui->btnPersonStart, &QPushButton::clicked, this, &AIagent::onPersonStartClicked);
    connect(ui->btnPersonSend, &QPushButton::clicked, this, &AIagent::onPersonSend);
    connect(ui->personMsgInput, &QLineEdit::returnPressed, this, &AIagent::onPersonSend);
    onPersonModeChanged();

    initDatabase();
    loadConversationList();
}

AIagent::~AIagent()
{
    delete ui;
}

// ==================== SQLite 持久化 ====================

void AIagent::initDatabase()
{
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
    m_db.setDatabaseName(QCoreApplication::applicationDirPath() + QStringLiteral("/chat.db"));
    if (!m_db.open()) {
        qWarning() << "chat.db 打开失败:" << m_db.lastError().text();
        return;
    }
    QSqlQuery query(m_db);
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS conversations ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "title TEXT NOT NULL,"
        "created_at TEXT NOT NULL)"));
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "role TEXT NOT NULL,"
        "content TEXT NOT NULL,"
        "emotion TEXT,"
        "time TEXT NOT NULL)"));

    // 旧库迁移：补 conversation_id 列
    bool hasConvId = false;
    if (query.exec(QStringLiteral("PRAGMA table_info(messages)"))) {
        while (query.next()) {
            if (query.value(1).toString() == QStringLiteral("conversation_id")) {
                hasConvId = true;
                break;
            }
        }
    }
    if (!hasConvId)
        query.exec(QStringLiteral(
            "ALTER TABLE messages ADD COLUMN conversation_id INTEGER NOT NULL DEFAULT 0"));

    // 把旧版单会话历史归入一个“历史对话”
    QSqlQuery count(m_db);
    if (count.exec(QStringLiteral(
            "SELECT COUNT(*) FROM messages WHERE conversation_id = 0"))
        && count.next() && count.value(0).toInt() > 0) {
        QSqlQuery insert(m_db);
        insert.prepare(QStringLiteral(
            "INSERT INTO conversations (title, created_at) VALUES (?, ?)"));
        insert.addBindValue(QStringLiteral("历史对话"));
        insert.addBindValue(
            QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        if (insert.exec()) {
            QSqlQuery update(m_db);
            update.prepare(QStringLiteral(
                "UPDATE messages SET conversation_id = ? WHERE conversation_id = 0"));
            update.addBindValue(insert.lastInsertId());
            update.exec();
        }
    }
}

void AIagent::loadMessages(int conversationId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT role, content, emotion, time FROM messages "
        "WHERE conversation_id = ? ORDER BY id"));
    query.addBindValue(conversationId);
    if (!query.exec())
        return;
    while (query.next()) {
        const bool fromUser = query.value(0).toString() == QStringLiteral("user");
        const ChatMessage msg{query.value(1).toString(), fromUser,
                              query.value(2).toString(), query.value(3).toString()};
        m_messages.append(msg);
        m_history.append(QJsonObject{{QStringLiteral("role"), query.value(0).toString()},
                                     {QStringLiteral("content"), msg.text}});
    }
}

int AIagent::saveMessage(const ChatMessage &msg)
{
    if (!m_db.isOpen())
        return -1;
    if (m_currentConvId < 0) {
        m_currentConvId = createConversationRow(msg.text);
        if (m_currentConvId < 0)
            return -1;
        ui->labelAiName->setText(truncateTitle(msg.text));
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO messages (conversation_id, role, content, emotion, time) "
        "VALUES (?, ?, ?, ?, ?)"));
    query.addBindValue(m_currentConvId);
    query.addBindValue(msg.fromUser ? QStringLiteral("user") : QStringLiteral("assistant"));
    query.addBindValue(msg.text);
    query.addBindValue(msg.emotion);
    query.addBindValue(msg.time);
    if (!query.exec())
        return -1;
    return query.lastInsertId().toInt();
}

void AIagent::deleteMessage(int id)
{
    if (!m_db.isOpen() || id < 0)
        return;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM messages WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec())
        return;
    // 会话被删空时移除会话行，避免残留空对话
    if (m_currentConvId > 0) {
        QSqlQuery count(m_db);
        count.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM messages WHERE conversation_id = ?"));
        count.addBindValue(m_currentConvId);
        if (count.exec() && count.next() && count.value(0).toInt() == 0) {
            QSqlQuery del(m_db);
            del.prepare(QStringLiteral("DELETE FROM conversations WHERE id = ?"));
            del.addBindValue(m_currentConvId);
            del.exec();
            m_currentConvId = -1;
            ui->labelAiName->setText(tr("新对话"));
        }
    }
}

// ==================== AI 聊天 ====================

QString AIagent::emotionForUserText(const QString &text) const
{
    static const QRegularExpression happyRe(
        QStringLiteral("哈哈|开心|高兴|太好了|真棒|不错|谢谢|喜欢|😊"));
    static const QRegularExpression sadRe(
        QStringLiteral("难过|伤心|生气|讨厌|烦|哭|😟|😢"));
    if (happyRe.match(text).hasMatch())
        return QStringLiteral("😊");
    if (sadRe.match(text).hasMatch())
        return QStringLiteral("😟");
    return QStringLiteral("😐");
}

// ==================== 图片上传 ====================

void AIagent::onUploadImage()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("选择图片"), QString(), tr("图片文件 (*.jpg *.jpeg *.png)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("提示"),
                             tr("无法读取图片文件：%1").arg(file.errorString()));
        return;
    }
    const QByteArray data = file.readAll();
    file.close();

    QPixmap pixmap;
    if (!pixmap.loadFromData(data)) {
        QMessageBox::warning(this, tr("提示"), tr("无法解析图片，请选择有效的 JPG/PNG 文件"));
        return;
    }

    m_imagePath = path;
    // Qt 默认 toBase64 每 76 字符插入换行，Ollama 侧解码会失败，需去除
    QByteArray encoded = data.toBase64();
    encoded.replace('\n', "");
    m_imageBase64 = encoded;

    ui->labelImagePreview->setPixmap(pixmap);
    ui->labelImagePreview->show();
    ui->btnClearImage->show();
    ui->btnUploadImage->setText(tr("更换图片"));
}

void AIagent::onClearImage()
{
    m_imagePath.clear();
    m_imageBase64.clear();
    ui->labelImagePreview->clear();
    ui->labelImagePreview->hide();
    ui->btnClearImage->hide();
    ui->btnUploadImage->setText(tr("上传图片"));
}

// ==================== 多会话管理 ====================

void AIagent::loadConversationList()
{
    while (QLayoutItem *item = ui->convListLayout->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    m_convButtons.clear();

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT id, title FROM conversations ORDER BY id DESC")))
        return;
    while (query.next()) {
        const int id = query.value(0).toInt();
        auto *btn = new QPushButton(query.value(1).toString(), ui->convListWidget);
        btn->setObjectName(QStringLiteral("convButton"));
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::clicked, this, [this, id] {
            openConversation(id);
            ui->stackedWidget->setCurrentWidget(ui->aiChatPage);
            ui->msgInput->setFocus();
        });
        ui->convListLayout->addWidget(btn);
        m_convButtons.append(btn);
    }
    ui->convListLayout->addStretch();
    setConversationControlsEnabled(!m_aiThinking);
}

void AIagent::newChat()
{
    if (m_aiThinking)
        return;
    m_currentConvId = -1;
    m_messages.clear();
    m_history = QJsonArray();
    if (m_suggestReply) {
        m_suggestReply->abort();
        m_suggestReply = nullptr;
    }
    ui->suggestBar->hide();
    onClearImage();
    ui->labelAiName->setText(tr("新对话"));
    m_messages.append({tr("你好！我是AI助手，请问有什么可以帮你的？"), false,
                       QStringLiteral("😊"),
                       QTime::currentTime().toString(QStringLiteral("HH:mm"))});
    renderAll();
}

void AIagent::openConversation(int conversationId)
{
    if (m_aiThinking)
        return;
    m_currentConvId = conversationId;
    m_messages.clear();
    m_history = QJsonArray();
    if (m_suggestReply) {
        m_suggestReply->abort();
        m_suggestReply = nullptr;
    }
    ui->suggestBar->hide();
    onClearImage();

    loadMessages(conversationId);

    QString title = tr("新对话");
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT title FROM conversations WHERE id = ?"));
    query.addBindValue(conversationId);
    if (query.exec() && query.next())
        title = query.value(0).toString();
    ui->labelAiName->setText(title);

    if (m_messages.isEmpty()) {
        m_messages.append({tr("你好！我是AI助手，请问有什么可以帮你的？"), false,
                           QStringLiteral("😊"),
                           QTime::currentTime().toString(QStringLiteral("HH:mm"))});
    }
    renderAll();
}

int AIagent::createConversationRow(const QString &title)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO conversations (title, created_at) VALUES (?, ?)"));
    query.addBindValue(truncateTitle(title));
    query.addBindValue(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    if (!query.exec())
        return -1;
    return query.lastInsertId().toInt();
}

void AIagent::setConversationControlsEnabled(bool enabled)
{
    ui->btnNewChat->setEnabled(enabled);
    ui->btnNewChatTop->setEnabled(enabled);
    ui->btnAiChat->setEnabled(enabled);
    for (QPushButton *btn : m_convButtons)
        btn->setEnabled(enabled);
}

void AIagent::onNewChat()
{
    newChat();
    ui->stackedWidget->setCurrentWidget(ui->aiChatPage);
    ui->msgInput->setFocus();
}

void AIagent::onContinueChat()
{
    int latest = -1;
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT MAX(id) FROM conversations")) && query.next())
        latest = query.value(0).toInt();
    if (latest > 0)
        openConversation(latest);
    else
        newChat();
    ui->stackedWidget->setCurrentWidget(ui->aiChatPage);
    ui->msgInput->setFocus();
}

void AIagent::onSendMessage()
{
    if (m_aiThinking)
        return;

    const QString text = ui->msgInput->text().trimmed();
    if (text.isEmpty())
        return;
    ui->msgInput->clear();
    ui->suggestBar->hide();
    if (m_suggestReply) {
        m_suggestReply->abort();
        m_suggestReply = nullptr;
    }

    const ChatMessage msg{text, true, emotionForUserText(text),
                          QTime::currentTime().toString(QStringLiteral("HH:mm"))};
    m_messages.append(msg);
    m_history.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                 {QStringLiteral("content"), text}});
    m_lastUserMsgId = saveMessage(msg);
    sendToOllama();
    onClearImage();
}

void AIagent::sendToOllama()
{
    m_aiThinking = true;
    m_aiText.clear();
    m_pendingText.clear();
    m_aiEmotion.clear();
    m_aiEmotionReady = false;
    m_finishPending = false;
    m_errorText.clear();
    m_buffer.clear();
    ui->btnSend->setEnabled(false);
    setConversationControlsEnabled(false);
    m_typeTimer->start();
    renderAll();

    QJsonArray messages;
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                {QStringLiteral("content"), kSystemPrompt}});
    for (const auto &value : m_history)
        messages.append(value);
    if (!m_imageBase64.isEmpty()) {
        QJsonObject last = messages.last().toObject();
        last.insert(QStringLiteral("images"),
                    QJsonArray{QString::fromLatin1(m_imageBase64)});
        messages[messages.size() - 1] = last;
    }

    QJsonObject payload{
        {QStringLiteral("model"), kModelName},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("stream"), true},
        {QStringLiteral("options"), QJsonObject{{QStringLiteral("temperature"), 0.3}}}};
    QNetworkRequest request{QUrl(kOllamaUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    m_reply = m_network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &AIagent::onOllamaReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &AIagent::onOllamaFinished);
}

void AIagent::updateAiEmotion()
{
    if (m_aiEmotionReady)
        return;
    static const QRegularExpression labelRe(QStringLiteral(
        "^\\s*【\\s*情绪\\s*[:：]\\s*(开心|高兴|愉快|中性|平静|难过|伤心|悲伤)\\s*】"));
    const QRegularExpressionMatch match = labelRe.match(m_pendingText);
    if (match.hasMatch()) {
        m_aiEmotion = emojiForLabel(match.captured(1));
        m_pendingText.remove(0, match.capturedEnd());
        m_pendingText = m_pendingText.trimmed();
        m_aiEmotionReady = true;
    } else if (m_pendingText.contains(QLatin1Char('\n')) || m_pendingText.size() > 40) {
        // 模型没有按要求输出标签，默认中性
        m_aiEmotion = QStringLiteral("😐");
        m_aiEmotionReady = true;
    }
}

void AIagent::onOllamaReadyRead()
{
    m_buffer += m_reply->readAll();
    int newline;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newline).trimmed();
        m_buffer.remove(0, newline + 1);
        if (line.isEmpty())
            continue;

        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        const QString err = obj.value(QStringLiteral("error")).toString();
        if (!err.isEmpty()) {
            m_errorText = err;
            continue;
        }
        if (obj.value(QStringLiteral("done")).toBool())
            continue;

        const QString token = obj.value(QStringLiteral("message"))
                                  .toObject()
                                  .value(QStringLiteral("content"))
                                  .toString();
        if (!token.isEmpty()) {
            // 先进入待显示缓冲，由打字机定时器逐字显示
            m_pendingText += token;
            updateAiEmotion();
        }
    }
}

void AIagent::onOllamaFinished()
{
    if (m_reply->error() == QNetworkReply::NoError && m_errorText.isEmpty()) {
        if (!m_aiEmotionReady) {
            m_aiEmotion = QStringLiteral("😐");
            m_aiEmotionReady = true;
        }
        if (m_pendingText.isEmpty() && m_aiText.isEmpty())
            m_pendingText = tr("（模型没有返回内容）");
        // 网络已结束，等打字机把剩余缓冲排空后再正式落盘
        m_finishPending = true;
        onTypeTick();
    } else {
        m_typeTimer->stop();
        m_pendingText.clear();
        m_finishPending = false;
        QString errMsg = !m_errorText.isEmpty() ? m_errorText : m_reply->errorString();
        if (m_reply->error() == QNetworkReply::ConnectionRefusedError)
            errMsg = tr("无法连接本地模型，请确认 Ollama 已启动（http://localhost:11434）");
        m_aiText = tr("【错误】%1").arg(errMsg);
        // 请求失败：回滚已保存的用户消息，保持数据库与上下文一致
        if (!m_history.isEmpty()
            && m_history.last().toObject().value(QStringLiteral("role")).toString()
                   == QStringLiteral("user")) {
            m_history.removeAt(m_history.size() - 1);
        }
        deleteMessage(m_lastUserMsgId);
        const QString time = QTime::currentTime().toString(QStringLiteral("HH:mm"));
        m_messages.append({m_aiText, false, QStringLiteral("😐"), time});
        m_lastUserMsgId = -1;
        m_aiText.clear();
        m_aiThinking = false;
        ui->btnSend->setEnabled(true);
        setConversationControlsEnabled(true);
        renderAll();
    }

    m_reply->deleteLater();
    m_reply = nullptr;
}

// 打字机：每次把待显示缓冲的字符搬 1~2 个到显示文本
void AIagent::onTypeTick()
{
    if (!m_aiThinking || !m_aiEmotionReady)
        return;
    if (!m_pendingText.isEmpty()) {
        const int n = m_pendingText.size() > 60 ? 2 : 1;
        m_aiText += m_pendingText.left(n);
        m_pendingText.remove(0, n);
        renderAll();
        return;
    }
    if (m_finishPending)
        finishAiReply();
}

void AIagent::finishAiReply()
{
    m_typeTimer->stop();
    m_finishPending = false;
    const QString time = QTime::currentTime().toString(QStringLiteral("HH:mm"));

    QString toolResult;
    if (tryExecuteTool(m_aiText, &toolResult)) {
        m_aiText = toolResult;
        m_aiEmotion = QStringLiteral("🔧");
    }

    m_history.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                 {QStringLiteral("content"), m_aiText}});
    const ChatMessage msg{m_aiText, false, m_aiEmotion, time};
    m_messages.append(msg);
    saveMessage(msg);
    m_lastUserMsgId = -1;
    m_aiText.clear();
    m_aiThinking = false;
    ui->btnSend->setEnabled(true);
    setConversationControlsEnabled(true);
    renderAll();
    requestSuggestions();
}

// ==================== 工具调用 ====================

bool AIagent::tryExecuteTool(const QString &reply, QString *result)
{
    // 兼容模型输出的多种格式：【计算】xxx、计算：xxx、计算 xxx
    static const QRegularExpression toolRe(QStringLiteral(
        "^\\s*(?:【\\s*)?(计算|计算器|网址|网页|文件夹|目录)\\s*(?:】|：|:|\\s+)([\\s\\S]+?)\\s*$"));
    static const QRegularExpression urlLikeRe(QStringLiteral(
        "^[\\w.-]+\\.[a-zA-Z]{2,}(?:[/?#][^\\s]*)?$"));

    const QRegularExpressionMatch match = toolRe.match(reply);
    if (!match.hasMatch())
        return false;
    const QString action = match.captured(1);
    QString arg = match.captured(2).trimmed();
    while (arg.endsWith(QLatin1Char('。')) || arg.endsWith(QLatin1Char('.')))
        arg.chop(1);

    if (action == QStringLiteral("计算") || action == QStringLiteral("计算器")) {
        // 模型可能输出“表达式=结果”，截断等号右侧
        const int eq = arg.indexOf(QLatin1Char('='));
        if (eq >= 0)
            arg = arg.left(eq).trimmed();
        // 必须以数字/括号/正负号开头，避免把普通回复误判为工具指令；
        // 解析失败时按普通文本显示，不生成错误气泡
        static const QRegularExpression headRe(QStringLiteral("^[0-9(（+\\-.×÷]"));
        if (!headRe.match(arg).hasMatch() || arg.size() > 100)
            return false;
        QString expr = arg;
        expr.replace(QStringLiteral("×"), QStringLiteral("*"));
        expr.replace(QStringLiteral("÷"), QStringLiteral("/"));
        expr.replace(QStringLiteral("（"), QStringLiteral("("));
        expr.replace(QStringLiteral("）"), QStringLiteral(")"));
        double value = 0;
        if (!MathParser(expr).parse(&value))
            return false;
        *result = tr("🧮 %1 = %2").arg(arg, QString::number(value, 'g', 12));
        return true;
    }

    if (action == QStringLiteral("网址") || action == QStringLiteral("网页")) {
        QString url = arg;
        if (url.startsWith(QStringLiteral("http://"))
            || url.startsWith(QStringLiteral("https://"))) {
            QDesktopServices::openUrl(QUrl(url));
            *result = tr("🔗 已打开网址：%1").arg(url);
            return true;
        }
        if (urlLikeRe.match(url).hasMatch()) {
            url = QStringLiteral("https://") + url;
            QDesktopServices::openUrl(QUrl(url));
            *result = tr("🔗 已打开网址：%1").arg(url);
            return true;
        }
        return false;
    }

    // 文件夹 / 目录
    QString path = arg;
    path.remove(QLatin1Char('"'));
    static const struct
    {
        const char *keyword;
        QStandardPaths::StandardLocation location;
    } knownFolders[] = {
        {"桌面", QStandardPaths::DesktopLocation},
        {"文档", QStandardPaths::DocumentsLocation},
        {"下载", QStandardPaths::DownloadLocation},
        {"图片", QStandardPaths::PicturesLocation},
        {"音乐", QStandardPaths::MusicLocation},
        {"视频", QStandardPaths::MoviesLocation},
    };
    for (const auto &known : knownFolders) {
        if (path.contains(QLatin1String(known.keyword))) {
            path = QStandardPaths::writableLocation(known.location);
            break;
        }
    }
    if (path.isEmpty() || !QDir(path).exists())
        return false;
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    *result = tr("📂 已打开文件夹：%1").arg(path);
    return true;
}

// ==================== 智能回复 ====================

void AIagent::requestSuggestions()
{
    // 取最近一条用户消息和助手回复作为上下文
    QString lastUser, lastAssistant;
    for (int i = m_history.size() - 1; i >= 0; --i) {
        const QJsonObject obj = m_history.at(i).toObject();
        const QString role = obj.value(QStringLiteral("role")).toString();
        const QString content = obj.value(QStringLiteral("content")).toString();
        if (role == QStringLiteral("user") && lastUser.isEmpty())
            lastUser = content;
        if (role == QStringLiteral("assistant") && lastAssistant.isEmpty())
            lastAssistant = content;
        if (!lastUser.isEmpty() && !lastAssistant.isEmpty())
            break;
    }
    if (lastUser.isEmpty() || lastAssistant.isEmpty())
        return;

    QJsonArray messages;
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                {QStringLiteral("content"), QStringLiteral(
                                     "你要预测用户接下来会回复什么。给出3条用户最可能回复的"
                                     "简短中文内容（每条不超过15个字），每条单独一行，不要编号，"
                                     "不要多余解释，只输出这3行。")}});
    // 消息数组必须以 user 结尾：以 assistant 结尾时 qwen:4b 会立即输出结束符（空内容）
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"),
                                 QStringLiteral("对话内容：\n用户：") + lastUser
                                     + QStringLiteral("\n助手：") + lastAssistant
                                     + QStringLiteral("\n\n请根据以上对话内容，"
                                                      "给出3条用户接下来最可能回复的内容。")}});

    QJsonObject payload{{QStringLiteral("model"), kModelName},
                        {QStringLiteral("messages"), messages},
                        {QStringLiteral("stream"), false},
                        {QStringLiteral("options"),
                         QJsonObject{{QStringLiteral("temperature"), 0.3}}}};
    QNetworkRequest request{QUrl(kOllamaUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    m_suggestReply = m_network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(m_suggestReply, &QNetworkReply::finished, this, &AIagent::onSuggestionFinished);
}

QStringList AIagent::parseSuggestions(const QByteArray &data) const
{
    const QString content = QJsonDocument::fromJson(data)
                                .object()
                                .value(QStringLiteral("message"))
                                .toObject()
                                .value(QStringLiteral("content"))
                                .toString();
    static const QRegularExpression numRe(
        QStringLiteral("^\\s*[0-9０-９]+[.、．)）]\\s*"));
    static const QRegularExpression bulletRe(QStringLiteral("^\\s*[-*•·]\\s*"));
    QStringList items;
    const QStringList lines = content.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        QString s = line.trimmed();
        s.remove(numRe);
        s.remove(bulletRe);
        if (s.isEmpty())
            continue;
        items << s;
        if (items.size() >= 3)
            break;
    }
    return items;
}

void AIagent::applySuggestions(const QStringList &items)
{
    const QList<QPushButton *> btns{ui->btnSuggest1, ui->btnSuggest2, ui->btnSuggest3};
    if (items.isEmpty()) {
        ui->suggestBar->hide();
        return;
    }
    for (int i = 0; i < btns.size(); ++i) {
        if (i < items.size()) {
            btns[i]->setText(items[i]);
            btns[i]->show();
        } else {
            btns[i]->hide();
        }
    }
    ui->suggestBar->show();
}

void AIagent::onSuggestionFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) {
        return;
    }
    const bool ok = reply->error() == QNetworkReply::NoError;
    QStringList items;
    if (ok)
        items = parseSuggestions(reply->readAll());
    reply->deleteLater();
    if (m_suggestReply == reply)
        m_suggestReply = nullptr;
    if (ok)
        applySuggestions(items);
}

void AIagent::onSuggestClicked()
{
    if (m_aiThinking)
        return;
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (!btn)
        return;
    ui->msgInput->setText(btn->text());
    onSendMessage();
}

void AIagent::renderAll()
{
    QString html;
    for (const ChatMessage &msg : m_messages) {
        const QString name = msg.fromUser ? tr("我") : tr("AI助手");
        html += bubbleHtml(name, msg.text, msg.fromUser, msg.emotion, msg.time);
    }
    if (m_aiThinking) {
        if (m_aiEmotionReady && !m_aiText.isEmpty())
            html += bubbleHtml(tr("AI助手"), m_aiText + QStringLiteral("▍"), false,
                               m_aiEmotion,
                               QTime::currentTime().toString(QStringLiteral("HH:mm")));
        else
            html += bubbleHtml(tr("AI助手"), tr("正在思考..."), false, QString(), QString());
    }

    ui->chatDisplay->setHtml(html);
    ui->chatDisplay->verticalScrollBar()->setValue(
        ui->chatDisplay->verticalScrollBar()->maximum());
}

QString AIagent::bubbleHtml(const QString &name, const QString &text, bool fromUser,
                           const QString &emotion, const QString &time) const
{
    const QString nameLine = emotion.isEmpty() ? name : name + QLatin1Char(' ') + emotion;
    const QString align = fromUser ? QStringLiteral("right") : QStringLiteral("left");
    const QString bg = fromUser ? QStringLiteral("#4A90D9") : QStringLiteral("#F2F2F2");
    const QString fg = fromUser ? QStringLiteral("#ffffff") : QStringLiteral("#333333");
    const QString body = text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));

    return QStringLiteral(
        "<div style=\"margin-top:4px;\">"
        "<span style=\"color:#888888; font-size:8pt;\">%1 %2</span><br>"
        "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr><td align=\"%3\">"
        "<table cellspacing=\"0\" cellpadding=\"0\" style=\"background-color:%4;\">"
        "<tr><td style=\"color:%5; padding:6px 10px;\">%6</td></tr></table>"
        "</td></tr></table>"
        "</div>")
        .arg(nameLine, time, align, bg, fg, body);
}

// ==================== 人与人聊天（TCP） ====================

void AIagent::setupPersonSocket(QTcpSocket *socket)
{
    m_tcpSocket = socket;
    connect(m_tcpSocket, &QTcpSocket::connected, this, &AIagent::onPersonConnected);
    connect(m_tcpSocket, &QTcpSocket::readyRead, this, &AIagent::onPersonReadyRead);
    connect(m_tcpSocket, &QTcpSocket::disconnected, this, &AIagent::onPersonDisconnected);
    connect(m_tcpSocket, &QTcpSocket::errorOccurred, this, &AIagent::onPersonError);
}

void AIagent::setPersonConnected(bool connected)
{
    m_personConnected = connected;
    ui->personMsgInput->setEnabled(connected);
    ui->btnPersonSend->setEnabled(connected);
    ui->comboPersonMode->setEnabled(!connected);
    ui->portEdit->setEnabled(!connected);
    ui->btnPersonStart->setEnabled(!connected);
    const bool isServer = ui->comboPersonMode->currentIndex() == 0;
    ui->ipEdit->setEnabled(!connected && !isServer);
    if (connected)
        ui->personMsgInput->setFocus();
}

void AIagent::onPersonModeChanged()
{
    const bool isServer = ui->comboPersonMode->currentIndex() == 0;
    ui->ipEdit->setEnabled(!isServer);
    ui->btnPersonStart->setText(isServer ? tr("开始监听") : tr("连接"));
}

void AIagent::onPersonStartClicked()
{
    const bool isServer = ui->comboPersonMode->currentIndex() == 0;
    const quint16 port = ui->portEdit->text().toUShort();

    if (isServer) {
        if (!m_tcpServer) {
            m_tcpServer = new QTcpServer(this);
            connect(m_tcpServer, &QTcpServer::newConnection,
                    this, &AIagent::onPersonNewConnection);
        }
        if (m_tcpServer->isListening())
            m_tcpServer->close();
        if (m_tcpServer->listen(QHostAddress::Any, port)) {
            ui->personStatus->setText(
                tr("正在监听端口 %1，等待客户端连接...").arg(port));
        } else {
            ui->personStatus->setText(tr("监听失败：%1").arg(m_tcpServer->errorString()));
        }
    } else {
        const QString ip = ui->ipEdit->text().trimmed();
        m_tcpSocket = new QTcpSocket(this);
        setupPersonSocket(m_tcpSocket);
        ui->personStatus->setText(tr("正在连接 %1:%2 ...").arg(ip).arg(port));
        m_tcpSocket->connectToHost(ip, port);
    }
}

void AIagent::onPersonNewConnection()
{
    while (m_tcpServer->hasPendingConnections()) {
        QTcpSocket *socket = m_tcpServer->nextPendingConnection();
        if (m_personConnected) {
            // 已有客户端连接，拒绝多余的连接
            socket->disconnectFromHost();
            socket->deleteLater();
            continue;
        }
        setupPersonSocket(socket);
        ui->personStatus->setText(tr("客户端已连接，可以开始聊天了"));
        setPersonConnected(true);
    }
}

void AIagent::onPersonConnected()
{
    ui->personStatus->setText(tr("已连接到服务器，可以开始聊天了"));
    setPersonConnected(true);
}

void AIagent::onPersonReadyRead()
{
    m_tcpBuffer += m_tcpSocket->readAll();
    int newline;
    while ((newline = m_tcpBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_tcpBuffer.left(newline);
        m_tcpBuffer.remove(0, newline + 1);
        const QString text = QString::fromUtf8(line).trimmed();
        if (!text.isEmpty())
            appendPersonMessage(text, false);
    }
}

void AIagent::onPersonDisconnected()
{
    if (m_personConnected) {
        const bool isServer = ui->comboPersonMode->currentIndex() == 0;
        ui->personStatus->setText(isServer
                                      ? tr("对方已断开，等待新客户端连接...")
                                      : tr("连接已断开"));
    }
    setPersonConnected(false);
    if (m_tcpSocket) {
        m_tcpSocket->deleteLater();
        m_tcpSocket = nullptr;
    }
}

void AIagent::onPersonError()
{
    if (!m_personConnected && m_tcpSocket) {
        ui->personStatus->setText(tr("连接失败：%1").arg(m_tcpSocket->errorString()));
        m_tcpSocket->deleteLater();
        m_tcpSocket = nullptr;
    }
}

void AIagent::onPersonSend()
{
    if (!m_tcpSocket || m_tcpSocket->state() != QAbstractSocket::ConnectedState)
        return;
    const QString text = ui->personMsgInput->text().trimmed();
    if (text.isEmpty())
        return;
    m_tcpSocket->write(text.toUtf8() + '\n');
    ui->personMsgInput->clear();
    appendPersonMessage(text, true);
}

void AIagent::appendPersonMessage(const QString &text, bool fromMe)
{
    const QString name = fromMe ? tr("我") : tr("对方");
    const QString time = QTime::currentTime().toString(QStringLiteral("HH:mm"));
    ui->personChatDisplay->append(bubbleHtml(name, text, fromMe, QString(), time));
    ui->personChatDisplay->verticalScrollBar()->setValue(
        ui->personChatDisplay->verticalScrollBar()->maximum());
}
