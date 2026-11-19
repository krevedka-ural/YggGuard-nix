#define main yggguardOriginalMain
#include "../src/main.cpp"
#undef main
#include <stdexcept>
static void require(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
static QPushButton *button(QWidget &w,const QString &name){for(auto b:w.findChildren<QPushButton*>())if(b->accessibleName()==name)return b;throw std::runtime_error("button not found");}
static void addServer(Window &w,int port,bool invalidFirst=false){
    QTimer::singleShot(10,[&w,port,invalidFirst]{
        auto dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());require(dialog,"add dialog missing");
        auto host=dialog->findChild<QLineEdit*>("proxyHost");auto name=dialog->findChild<QLineEdit*>("proxyName");
        auto ports=dialog->findChild<QSpinBox*>("proxyPort");auto box=dialog->findChild<QDialogButtonBox*>();
        require(host&&name&&ports&&box,"form missing");
        if(invalidFirst){host->setText("2001:db8::1");box->button(QDialogButtonBox::Save)->click();require(dialog->isVisible(),"public address accepted");}
        name->setText(port==2080?"Tor":"VPS");host->setText("300::1234");ports->setValue(port);box->button(QDialogButtonBox::Save)->click();
    });
    button(w,"Добавить прокси")->click();
}
int main(int argc,char**argv){
    QTemporaryDir temp;require(temp.isValid(),"temp directory failed");
    qputenv("XDG_DATA_HOME",(temp.path()+"/data").toUtf8());qputenv("XDG_CONFIG_HOME",(temp.path()+"/config").toUtf8());qputenv("XDG_CACHE_HOME",(temp.path()+"/cache").toUtf8());
    QApplication app(argc,argv);QCoreApplication::setOrganizationName("YggGuard");QCoreApplication::setApplicationName("YggGuard");
    try{
        {Window w;w.show();app.processEvents();auto combo=w.findChild<QComboBox*>();require(combo&&combo->count()==0,"fresh state not empty");
         addServer(w,1080,true);require(combo->count()==1,"direct profile not added");
         addServer(w,2080);require(combo->count()==2&&combo->currentText()=="Tor","Tor selection not saved");}
        {Window w;w.show();app.processEvents();auto combo=w.findChild<QComboBox*>();require(combo->count()==2&&combo->currentText()=="Tor","profiles not restored");
         button(w,"Удалить выбранный прокси")->click();require(combo->count()==1&&combo->currentText()=="VPS","delete failed");}
        std::puts("UI smoke: add, reject invalid IP, select, persist, reload, delete: OK");return 0;
    }catch(const std::exception &e){std::fprintf(stderr,"UI smoke failed: %s\n",e.what());return 1;}
}
