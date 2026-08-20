#ifndef AIAGENT_H
#define AIAGENT_H

#include <QJsonArray>
#include <QList>
#include <QMainWindow>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>

class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QTimer;

QT_BEGIN_NAMESPACE
namespace Ui {
class AIagent;
}
QT_END_NAMESPACE

struct ChatMessage
{
    QString text;
    bool fromUser;
    QString emotion;
    QString time;
};

class AIagent : public QMainWindow
{
    Q_OBJECT

public:
    explicit AIagent(QWidget *parent = nullptr);
    ~AIagent() override;

private slots:
    void onSendMessage();
    void onOllamaReadyRead();
    void onOllamaFinished();
    void onTypeTick();
    void onSuggestionFinished();
    void onSuggestClicked();

    void onPersonModeChanged();
    void onPersonStartClicked();
    void onPersonNewConnection();
    void onPersonConnected();
    void onPersonReadyRead();
    void onPersonDisconnected();
    void onPersonError();
    void onPersonSend();

    void onUploadImage();
    void onClearImage();

    void onNewChat();
    void onContinueChat();

private:
    void sendToOllama();
    void renderAll();
    QString bubbleHtml(const QString &name, const QString &text, bool fromUser,
                       const QString &emotion, const QString &time) const;
    void initDatabase();
    void loadConversationList();
    void loadMessages(int conversationId);
    void openConversation(int conversationId);
    void newChat();
    int createConversationRow(const QString &title);
    void setConversationControlsEnabled(bool enabled);
    bool tryExecuteTool(const QString &reply, QString *result);
    int saveMessage(const ChatMessage &msg);
    void deleteMessage(int id);
    QString emotionForUserText(const QString &text) const;
    void updateAiEmotion();
    void finishAiReply();
    void requestSuggestions();
    QStringList parseSuggestions(const QByteArray &data) const;
    void applySuggestions(const QStringList &items);

    void setupPersonSocket(QTcpSocket *socket);
    void setPersonConnected(bool connected);
    void appendPersonMessage(const QString &text, bool fromMe);

    Ui::AIagent *ui;
    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    QNetworkReply *m_suggestReply = nullptr;
    QTimer *m_typeTimer = nullptr;
    QByteArray m_buffer;
    QString m_aiText;
    QString m_pendingText;
    QString m_aiEmotion;
    bool m_aiEmotionReady = false;
    bool m_finishPending = false;
    QString m_errorText;
    bool m_aiThinking = false;
    int m_lastUserMsgId = -1;
    QList<ChatMessage> m_messages;
    QJsonArray m_history;
    QSqlDatabase m_db;

    QTcpServer *m_tcpServer = nullptr;
    QTcpSocket *m_tcpSocket = nullptr;
    QByteArray m_tcpBuffer;
    bool m_personConnected = false;

    QString m_imagePath;
    QByteArray m_imageBase64;

    int m_currentConvId = -1;
    QList<QPushButton *> m_convButtons;
};
#endif // AIAGENT_H
