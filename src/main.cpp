#include <QtWidgets>
#include <QtNetwork>
#include <memory>

static const QString DefaultPeer = "tcp://ygg-msk-1.averyan.ru:8363";
static QString errorText(const QString &s) { return s.left(3000); }
static bool yggAddress(const QString &host) {
    QHostAddress a(host);
    return a.protocol() == QAbstractSocket::IPv6Protocol && a.scopeId().isEmpty() &&
        (a.toIPv6Address()[0] == 2 || a.toIPv6Address()[0] == 3);
}
static bool peerValid(const QString &value) {
    QUrl u(value);
    return u.isValid() && QStringList{"tcp", "tls", "quic"}.contains(u.scheme()) &&
        !u.host().isEmpty() && u.port() > 0 && u.port() <= 65535 && u.userInfo().isEmpty() && value.size() <= 2048;
}

class PowerButton : public QPushButton {
public:
    bool active = false, busy = false;
    PowerButton() { setFixedSize(202, 202); setCursor(Qt::PointingHandCursor); setAccessibleName("Включить или отключить VPN"); }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const QColor teal = active ? QColor("#46e5cf") : QColor("#2dc6b3");
        p.setPen(QPen(QColor(active ? "#215b57" : "#223d3e"), 1));
        p.setBrush(QColor(active ? "#143c3a" : "#102729")); p.drawEllipse(QRectF(1,1,200,200));
        p.setPen(QPen(teal, 2)); p.setBrush(QColor(active ? "#174d47" : "#133b38")); p.drawEllipse(QRectF(20,20,162,162));
        p.setPen(QPen(teal, 5, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(76,69,50,50), 130*16, 280*16); p.drawLine(QPointF(101,61), QPointF(101,89));
        p.setPen(QColor("#d7f8f1")); QFont f = font(); f.setPixelSize(13); f.setWeight(QFont::DemiBold); p.setFont(f);
        p.drawText(QRect(25,135,152,28), Qt::AlignCenter, busy ? "Подключение…" : active ? "Отключить" : "Подключить");
        if (hasFocus()) { p.setPen(QPen(QColor("#b7fff2"),2,Qt::DashLine));p.setBrush(Qt::NoBrush);p.drawEllipse(QRectF(7,7,188,188)); }
    }
};

class Window : public QWidget {
    PowerButton *power;
    QLabel *state, *hint, *mode;
    QComboBox *profiles;
    QPushButton *add, *remove;
    QPlainTextEdit *logs;
    QJsonArray entries;
    QString privateKey, storePath;
    std::unique_ptr<QTemporaryDir> stage, ipcDir;
    QLocalServer ipc;
    QPointer<QLocalSocket> channel;
    QProcess helper;
    QTimer timeout;
    QString token;
    QByteArray input;
    bool connecting = false, connected = false, stopping = false;

    QString bundled(const QString &name) const { return QCoreApplication::applicationDirPath() + "/../libexec/" + name; }
    void appendLog(const QString &text) { logs->appendPlainText(text.trimmed()); }
    void save() {
        QSaveFile f(storePath); if (!f.open(QIODevice::WriteOnly)) { appendLog("Не удалось сохранить профили: " + f.errorString()); return; }
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.write(QJsonDocument(QJsonObject{{"profiles",entries},{"selected",profiles->currentIndex()},{"private_key",privateKey}}).toJson());
        if (!f.commit()) appendLog("Не удалось сохранить настройки: " + f.errorString());
    }
    void updateSelection() {
        auto p = entries.at(profiles->currentIndex()).toObject();
        int port = p.value("port").toInt();
        mode->setText(p.isEmpty() ? "Добавьте свой сервер" : port == 2080 ? "Выход через Tor · порт 2080" : "Прямой выход · порт " + QString::number(port));
        remove->setEnabled(!connecting && !connected && !stopping && !p.isEmpty());
    }
    void refresh(int selected) {
        QSignalBlocker b(profiles); profiles->clear();
        for (const auto &v : entries) { auto p=v.toObject(); profiles->addItem(p.value("name").toString()); }
        profiles->setCurrentIndex(entries.isEmpty() ? -1 : qBound(0,selected,int(entries.size())-1));
        updateSelection();
    }
    void setStatus(const QString &s) { state->setText(s); }
    void controls() {
        power->active=connected; power->busy=connecting; power->update();
        power->setEnabled(!stopping); profiles->setEnabled(!connecting && !connected && !stopping);
        add->setEnabled(!connecting && !connected && !stopping); updateSelection();
    }
    void failed(const QString &why) {
        appendLog(why); setStatus("Не удалось подключиться"); hint->setText(errorText(why));
        disconnectVpn();
    }
    bool ensureKey() {
        if (!privateKey.isEmpty()) return true;
        QProcess generate; generate.start(bundled("yggdrasil"), {"-genconf", "-json"});
        if (!generate.waitForFinished(5000) || generate.exitCode()!=0) { generate.kill(); failed("Не удалось создать ключ встроенного Yggdrasil");return false; }
        privateKey=QJsonDocument::fromJson(generate.readAllStandardOutput()).object().value("PrivateKey").toString();
        if (privateKey.size()!=128) {failed("Yggdrasil вернул неверный ключ");return false;}
        save();return true;
    }
    void connectVpn() {
        if (profiles->currentIndex()<0) { addProfile(); return; }
        if (QStandardPaths::findExecutable("pkexec").isEmpty()) { failed("Установите polkit: для создания VPN нужен системный запрос прав администратора.");return; }
        for (const auto &name : {"yggdrasil", "sing-box", "yggguard-helper"}) {
            if (!QFileInfo(bundled(name)).isExecutable()) { failed("В сборке отсутствует компонент: " + QString(name));return; }
        }
        if (!ensureKey()) return;
        stage=std::make_unique<QTemporaryDir>(QDir::tempPath()+"/yggguard-bin-XXXXXX");
        ipcDir=std::make_unique<QTemporaryDir>(QDir::tempPath()+"/yggguard-ipc-XXXXXX");
        if (!stage->isValid() || !ipcDir->isValid()) {failed("Не удалось создать временный каталог");return;}
        // pkexec cannot reliably execute files inside a user's AppImage FUSE mount.
        for (const auto &name : {"yggdrasil", "sing-box", "yggguard-helper"}) {
            QString to=stage->path()+"/"+name;
            if (!QFile::copy(bundled(name),to)) {failed("Не удалось подготовить " + QString(name));return;}
            QFile::setPermissions(to,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
        }
        ipc.close(); ipc.setSocketOptions(QLocalServer::UserAccessOption);
        if (!ipc.listen(ipcDir->path()+"/control")) {failed(ipc.errorString());return;}
        token=QUuid::createUuid().toString(QUuid::WithoutBraces)+QUuid::createUuid().toString(QUuid::WithoutBraces);
        input.clear(); connecting=true; connected=false; stopping=false; controls();
        setStatus("Ожидание разрешения…"); hint->setText("Подтвердите создание VPN в системном окне.");
        QProcessEnvironment env=QProcessEnvironment::systemEnvironment();
        env.remove("LD_LIBRARY_PATH"); env.remove("LD_PRELOAD"); env.remove("QT_PLUGIN_PATH");
        helper.setProcessEnvironment(env);
        helper.start("pkexec", {stage->path()+"/yggguard-helper",ipc.fullServerName(),token});
        timeout.start(150000);
    }
    void disconnectVpn() {
        timeout.stop(); connecting=false; connected=false;
        if (channel) {channel->disconnectFromServer(); channel->close();}
        ipc.close();
        if (helper.state()!=QProcess::NotRunning) {
            stopping=true; controls();
            if (!channel) helper.terminate(); // Cancel an authorization request that has not connected yet.
        } else { stopping=false; controls(); }
    }
    void consume() {
        if (!channel) return;
        input+=channel->readAll();
        if (input.size()>128*1024) {failed("Некорректный ответ VPN-компонента");return;}
        int i;
        while ((i=input.indexOf('\n'))>=0) {
            auto line=input.left(i);input.remove(0,i+1);
            auto event=QJsonDocument::fromJson(line).object(); QString type=event.value("type").toString(), text=event.value("text").toString();
            if (type=="hello") {
                if (text!=token) {failed("Не прошла проверка VPN-компонента");return;}
                auto p=entries.at(profiles->currentIndex()).toObject(); p.insert("private_key",privateKey);
                channel->write(QJsonDocument(p).toJson(QJsonDocument::Compact)+'\n');channel->flush();
            } else if (type=="log") appendLog(text);
            else if (type=="status") {setStatus(text);hint->setText("Первое соединение может занять до двух минут.");}
            else if (type=="error") {failed(text);return;}
            else if (type=="connected") {
                timeout.stop();connecting=false;connected=true;setStatus("Подключено");
                hint->setText("Трафик приложений проходит через выбранный сервер.");controls();
            }
        }
    }
    void addProfile() {
        QDialog d(this);d.setWindowTitle("Добавить прокси");d.setMinimumWidth(410);
        auto layout=new QVBoxLayout(&d); auto form=new QFormLayout;
        auto name=new QLineEdit;name->setObjectName("proxyName");name->setPlaceholderText("Мой сервер");name->setMaxLength(100);
        auto host=new QLineEdit;host->setObjectName("proxyHost");host->setPlaceholderText("200:… — IPv6 сервера в Yggdrasil");
        auto port=new QSpinBox;port->setObjectName("proxyPort");port->setRange(1,65535);port->setValue(1080);
        form->addRow("Название",name);form->addRow("Yggdrasil IP",host);form->addRow("Порт",port);layout->addLayout(form);
        auto note=new QLabel("1080 — прямой выход · 2080 — Tor");note->setObjectName("muted");layout->addWidget(note);
        auto peers=new QPlainTextEdit(DefaultPeer);peers->setMaximumHeight(95);
        layout->addWidget(new QLabel("Пиры Yggdrasil — по одному URI на строку"));layout->addWidget(peers);
        auto error=new QLabel;error->setWordWrap(true);error->setStyleSheet("color:#f4b8ac");layout->addWidget(error);
        auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Save)->setText("Добавить");buttons->button(QDialogButtonBox::Cancel)->setText("Отмена");layout->addWidget(buttons);
        connect(buttons,&QDialogButtonBox::rejected,&d,&QDialog::reject);
        connect(buttons,&QDialogButtonBox::accepted,&d,[&]{
            QString h=host->text().trimmed();if(h.startsWith('[')&&h.endsWith(']'))h=h.mid(1,h.size()-2);
            if(!yggAddress(h)){error->setText("Укажите полный IPv6 Yggdrasil из диапазона 200::/7.");return;}
            QJsonArray list;for(auto peer:peers->toPlainText().split('\n',Qt::SkipEmptyParts)){
                peer=peer.trimmed();if(!peerValid(peer)){error->setText("Неверный пир: нужен tcp://, tls:// или quic:// с адресом и портом.");return;}list.append(peer);
            }
            if(list.isEmpty() || list.size()>16){error->setText("Укажите от 1 до 16 пиров.");return;}
            entries.append(QJsonObject{{"name",name->text().trimmed().isEmpty()?QString("[%1]:%2").arg(h).arg(port->value()):name->text().trimmed()},
                {"host",QHostAddress(h).toString()},{"port",port->value()},{"peers",list}});
            refresh(entries.size()-1);save();d.accept();
        });
        d.exec();
    }
protected:
    void closeEvent(QCloseEvent *e) override { disconnectVpn();if(helper.state()!=QProcess::NotRunning)helper.waitForFinished(13000);e->accept(); }
public:
    Window() {
        setWindowTitle("YggGuard");resize(450,700);setMinimumSize(410,660);
        setStyleSheet("QWidget{background:#0e191d;color:#e7f3f0;font-family:'DejaVu Sans';font-size:13px;}"
            "QLabel#muted{color:#7e999e;}QLabel#brand{font-size:23px;font-weight:700;letter-spacing:1px;}"
            "QComboBox,QLineEdit,QSpinBox,QPlainTextEdit{background:#14272c;border:1px solid #294047;border-radius:9px;padding:10px;selection-background-color:#287d73;}"
            "QPushButton{border:1px solid #294047;border-radius:9px;padding:9px 14px;background:#14272c;}"
            "QPushButton:hover{background:#1c373c;}QPushButton:disabled{color:#567075;}QPushButton:focus{border-color:#46e5cf;}"
            "QDialogButtonBox QPushButton{min-width:76px;}QScrollBar{background:#14272c;width:8px;}");
        auto layout=new QVBoxLayout(this);layout->setContentsMargins(32,30,32,24);layout->setSpacing(12);
        auto brand=new QLabel("YggGuard");brand->setObjectName("brand");layout->addWidget(brand);
        auto tagline=new QLabel("Ваш маршрут через Yggdrasil");tagline->setObjectName("muted");layout->addWidget(tagline);
        layout->addStretch(1);
        state=new QLabel("Не подключено");state->setAlignment(Qt::AlignCenter);state->setStyleSheet("font-size:19px;font-weight:600;");layout->addWidget(state);
        hint=new QLabel("Добавьте сервер и нажмите кнопку подключения.");hint->setWordWrap(true);hint->setAlignment(Qt::AlignCenter);hint->setObjectName("muted");hint->setMinimumHeight(46);layout->addWidget(hint);
        layout->addSpacing(12);power=new PowerButton;layout->addWidget(power,0,Qt::AlignHCenter);
        add=new QPushButton("+");add->setAccessibleName("Добавить прокси");add->setToolTip("Добавить прокси");add->setFixedSize(46,46);add->setStyleSheet("font-size:26px;color:#52decb;border-radius:23px;");layout->addWidget(add,0,Qt::AlignHCenter);
        layout->addSpacing(12);
        auto selector=new QHBoxLayout;profiles=new QComboBox;profiles->setAccessibleName("Выбранный прокси");profiles->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);selector->addWidget(profiles);
        remove=new QPushButton("×");remove->setToolTip("Удалить выбранный прокси");remove->setAccessibleName("Удалить выбранный прокси");selector->addWidget(remove);layout->addLayout(selector);
        mode=new QLabel;mode->setAlignment(Qt::AlignCenter);mode->setObjectName("muted");layout->addWidget(mode);layout->addStretch(1);
        auto diagnostics=new QPushButton("Журнал подключения");diagnostics->setFlat(true);layout->addWidget(diagnostics);
        logs=new QPlainTextEdit;logs->setReadOnly(true);logs->setMaximumBlockCount(200);logs->setMaximumHeight(130);logs->hide();layout->addWidget(logs);
        auto footer=new QLabel("Yggdrasil внутри · VPN для Linux");footer->setAlignment(Qt::AlignCenter);footer->setObjectName("muted");layout->addWidget(footer);
        auto dir=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);QDir().mkpath(dir);QFile::setPermissions(dir,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);storePath=dir+"/profiles.json";
        QFile f(storePath);if(f.open(QIODevice::ReadOnly)){auto obj=QJsonDocument::fromJson(f.readAll()).object();entries=obj.value("profiles").toArray();privateKey=obj.value("private_key").toString();refresh(obj.value("selected").toInt());}else refresh(0);
        connect(profiles,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{updateSelection();save();});
        connect(add,&QPushButton::clicked,this,[this]{addProfile();});
        connect(remove,&QPushButton::clicked,this,[this]{int i=profiles->currentIndex();if(i>=0){entries.removeAt(i);refresh(qMax(0,i-1));save();}});
        connect(power,&QPushButton::clicked,this,[this]{if(connecting||connected){setStatus("Отключение…");hint->setText("Восстанавливаем маршруты.");disconnectVpn();}else connectVpn();});
        connect(diagnostics,&QPushButton::clicked,this,[this]{logs->setVisible(!logs->isVisible());});
        connect(&ipc,&QLocalServer::newConnection,this,[this]{
            if(channel){auto extra=ipc.nextPendingConnection();extra->close();extra->deleteLater();return;}
            channel=ipc.nextPendingConnection();connect(channel,&QLocalSocket::readyRead,this,[this]{consume();});
            connect(channel,&QLocalSocket::disconnected,this,[this]{if(connecting||connected){failed("Связь с VPN-компонентом прервана");}});
        });
        helper.setProcessChannelMode(QProcess::MergedChannels);
        connect(&helper,&QProcess::readyReadStandardOutput,this,[this]{appendLog(QString::fromUtf8(helper.readAllStandardOutput()));});
        connect(&helper,&QProcess::errorOccurred,this,[this](QProcess::ProcessError e){if(e==QProcess::FailedToStart)failed("Не удалось открыть системный запрос прав: "+helper.errorString());});
        connect(&helper,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus){
            timeout.stop();bool unexpected=connecting||connected;connecting=false;connected=false;stopping=false;
            if(channel){channel->deleteLater();channel=nullptr;}ipc.close();stage.reset();ipcDir.reset();
            if(unexpected){setStatus("Соединение прервано");hint->setText(code==126||code==127?"Запрос прав отменён или недоступен. Подробности — в журнале.":"Подробности — в журнале подключения.");}
            else if(state->text()!="Не удалось подключиться"){setStatus("Не подключено");hint->setText("Нажмите кнопку, чтобы подключиться снова.");}controls();
        });
        timeout.setSingleShot(true);connect(&timeout,&QTimer::timeout,this,[this]{failed("Время подключения истекло. Проверьте пиры и доступность VPS.");});
    }
};

int main(int argc,char**argv) {
    QApplication app(argc,argv);QCoreApplication::setOrganizationName("YggGuard");QCoreApplication::setApplicationName("YggGuard");
    QCoreApplication::setApplicationVersion("0.2.0");
    QFontDatabase::addApplicationFont(QCoreApplication::applicationDirPath()+"/../share/yggguard/DejaVuSans.ttf");
    Window w;w.show();
    if(app.arguments().contains("--screenshot")){
        int i=app.arguments().indexOf("--screenshot");QString path=app.arguments().value(i+1,"yggguard.png");
        QTimer::singleShot(300,&app,[&w,path]{w.grab().save(path);qApp->quit();});
    }
    return app.exec();
}
