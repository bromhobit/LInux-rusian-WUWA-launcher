#include "controller.h"
#include <SDL3/SDL.h>
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFocusFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtGlobal>
#include <cmath>
#include <algorithm>
#include <limits>

class Controller final : public QObject {
public:
 Controller(QWidget *window,bool useHardware):QObject(window),root(window) {
  setObjectName("controllerObject");
  auto *hint=new QLabel("🎮 A — выбрать  ·  B — назад  ·  LB/RB — раздел",root);
  hint->setObjectName("controllerHint");
  hint->setAlignment(Qt::AlignCenter);
  hint->hide();
  status=hint;
  if (auto *layout=qobject_cast<QVBoxLayout*>(root->layout()))layout->addWidget(hint);
  ensureFocusFrame();
  connect(qApp,&QApplication::focusChanged,this,[this](QWidget *,QWidget *now){
   if(gamepadMode)showFocus(now);
  });
  if(!useHardware)return;
  initialized=SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if(!initialized){qWarning("SDL gamepad: %s",SDL_GetError());return;}
  int n=0;auto *ids=SDL_GetGamepads(&n);if(n>0&&ids)open(ids[0]);SDL_free(ids);
  auto *timer=new QTimer(this);
  connect(timer,&QTimer::timeout,this,[this]{poll();});
  timer->start(40);
 }
 ~Controller() override {if(pad)SDL_CloseGamepad(pad);if(initialized)SDL_QuitSubSystem(SDL_INIT_GAMEPAD);}
 void press(SDL_GamepadButton b){
  gamepadMode=true;
  showFocus(QApplication::focusWidget());
  switch(b){
   case SDL_GAMEPAD_BUTTON_DPAD_UP: navigate(0,-1);break;
   case SDL_GAMEPAD_BUTTON_DPAD_LEFT: navigate(-1,0);break;
   case SDL_GAMEPAD_BUTTON_DPAD_DOWN: navigate(0,1);break;
   case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: navigate(1,0);break;
   case SDL_GAMEPAD_BUTTON_SOUTH: activate();break;
   case SDL_GAMEPAD_BUTTON_EAST: back();break;
   case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: page(false);break;
   case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: page(true);break;
   default:break;
  }
 }
 bool focusVisibleFor(QWidget *w)const{return focusFrame&&focusFrame->isVisible()&&focusFrame->widget()==w;}
private:
 void open(SDL_JoystickID id){if(pad)return;pad=SDL_OpenGamepad(id);if(status)status->setVisible(pad!=nullptr);}
 void poll(){
  SDL_Event e;
  while(SDL_PollEvent(&e)){
   if(e.type==SDL_EVENT_GAMEPAD_ADDED)open(e.gdevice.which);
   else if(e.type==SDL_EVENT_GAMEPAD_REMOVED&&pad&&SDL_GetGamepadID(pad)==e.gdevice.which){
    SDL_CloseGamepad(pad);pad=nullptr;status->hide();
    int n=0;auto *ids=SDL_GetGamepads(&n);if(n>0&&ids)open(ids[0]);SDL_free(ids);
   }else if(e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN&&pad&&e.gbutton.which==SDL_GetGamepadID(pad)
             &&interactive()){
    const auto b=static_cast<SDL_GamepadButton>(e.gbutton.button);
    if(b!=SDL_GAMEPAD_BUTTON_DPAD_UP&&b!=SDL_GAMEPAD_BUTTON_DPAD_DOWN&&
       b!=SDL_GAMEPAD_BUTTON_DPAD_LEFT&&b!=SDL_GAMEPAD_BUTTON_DPAD_RIGHT)press(b);
   }
  }
  if(!pad||!interactive()){heldDirection=0;return;}
  const auto now=QDateTime::currentMSecsSinceEpoch();
  const int lx=SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFTX);
  const int ly=SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFTY);
  const int ry=SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_RIGHTY);
  int direction=0;
  if(SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_DPAD_UP))direction=1;
  else if(SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_DPAD_DOWN))direction=2;
  else if(SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_DPAD_LEFT))direction=3;
  else if(SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_DPAD_RIGHT))direction=4;
  else if(std::abs(ly)>18000||std::abs(lx)>18000)
   direction=std::abs(ly)>std::abs(lx)?(ly>0?2:1):(lx>0?4:3);
  if(direction){
   if(direction!=heldDirection||now>=nextRepeat){
    press(direction==1?SDL_GAMEPAD_BUTTON_DPAD_UP:direction==2?SDL_GAMEPAD_BUTTON_DPAD_DOWN:
          direction==3?SDL_GAMEPAD_BUTTON_DPAD_LEFT:SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    nextRepeat=now+(direction!=heldDirection?350:140);
   }
   heldDirection=direction;
  } else if(std::abs(ry)>18000&&now-lastAxis>=150){
   heldDirection=0;
   auto *pages=root->findChild<QStackedWidget*>("pageStack");
   if(pages)if(auto *area=qobject_cast<QScrollArea*>(pages->currentWidget())){
    auto *s=area->verticalScrollBar();s->setValue(s->value()+(ry>0?90:-90));
   }
   lastAxis=now;
  }else heldDirection=0;
 }
 bool belongs(QWidget *w)const{return w&&(w==root||root->isAncestorOf(w));}
 bool interactive()const{
  if(auto *popup=QApplication::activePopupWidget())return belongs(popup)||belongs(popup->parentWidget());
  if(auto *modal=QApplication::activeModalWidget())return belongs(modal)||belongs(modal->parentWidget());
  return root->isActiveWindow();
 }
 void ensureFocusFrame(){
  if(focusFrame)return;
  focusFrame=new QFocusFrame(root);
  focusFrame->setObjectName("gamepadFocusFrame");
  focusFrame->setAttribute(Qt::WA_TransparentForMouseEvents);
  focusFrame->setStyleSheet("QFocusFrame { border: 3px solid #ffd477; border-radius: 7px; background: transparent; }");
  focusFrame->hide();
 }
 void showFocus(QWidget *w){
  ensureFocusFrame();
  auto *popup=QApplication::activePopupWidget();
  auto *modal=QApplication::activeModalWidget();
  if(w&&w->isVisible()&&w->isEnabled()&&(belongs(w)||(popup&&belongs(popup->parentWidget()))||(modal&&belongs(modal->parentWidget())))){
   focusFrame->setWidget(w);focusFrame->show();focusFrame->raise();
  }else focusFrame->hide();
 }
 static void key(QWidget *w,int value,Qt::KeyboardModifiers modifiers=Qt::NoModifier){
  if(!w)return;
  QPointer<QWidget> guarded=w;
  QKeyEvent down(QEvent::KeyPress,value,modifiers);QApplication::sendEvent(w,&down);
  if(guarded){QKeyEvent up(QEvent::KeyRelease,value,modifiers);QApplication::sendEvent(guarded,&up);}
 }
 QWidget *modalFocus(QWidget *modal)const{
  auto *focus=QApplication::focusWidget();
  if(focus&&(focus==modal||modal->isAncestorOf(focus)))return focus;
  return modal->focusWidget()?modal->focusWidget():modal;
 }
 void messageBoxNavigate(QMessageBox *box,int dx,int dy){
  QList<QAbstractButton*> buttons;
  for(auto *button:box->buttons())if(button->isVisible()&&button->isEnabled()&&button->focusPolicy()!=Qt::NoFocus)buttons.append(button);
  if(buttons.isEmpty())return;
  std::sort(buttons.begin(),buttons.end(),[](QAbstractButton *a,QAbstractButton *b){
   const QPoint pa=a->mapToGlobal(a->rect().center()),pb=b->mapToGlobal(b->rect().center());
   return pa.y()!=pb.y()?pa.y()<pb.y():pa.x()<pb.x();
  });
  QWidget *focus=modalFocus(box);int current=buttons.indexOf(qobject_cast<QAbstractButton*>(focus));
  if(current<0){auto *preferred=box->defaultButton();current=buttons.indexOf(preferred);if(current<0)current=0;}
  else {const int step=(dx<0||dy<0)?-1:1;current=(current+step+buttons.size())%buttons.size();}
  buttons.at(current)->setFocus(Qt::OtherFocusReason);
  showFocus(buttons.at(current));
 }
 void navigate(int dx,int dy){
  const int arrow=dy<0?Qt::Key_Up:dy>0?Qt::Key_Down:dx<0?Qt::Key_Left:Qt::Key_Right;
  if(auto *popup=QApplication::activePopupWidget()){
   auto *f=QApplication::focusWidget();key(f?f:popup,arrow);return;
  }
  if(auto *modal=QApplication::activeModalWidget()){
   if(auto *box=qobject_cast<QMessageBox*>(modal)){messageBoxNavigate(box,dx,dy);return;}
   key(modalFocus(modal),arrow);return;
  }
  auto *f=QApplication::focusWidget();
  if(!f||!(f==root||root->isAncestorOf(f))){
   if(auto *b=root->findChild<QAbstractButton*>("playGameButton"))b->setFocus(Qt::OtherFocusReason);
   return;
  }
  QVector<QWidget*> choices;
  for(auto *w:root->findChildren<QWidget*>()){
   if(!w->isVisible()||!w->isEnabled()||w->focusPolicy()==Qt::NoFocus||w==f)continue;
   if(qobject_cast<QAbstractButton*>(w)||qobject_cast<QComboBox*>(w)||qobject_cast<QLineEdit*>(w))choices.push_back(w);
  }
  const QPoint origin=f->mapToGlobal(f->rect().center());
  QWidget *target=nullptr;
  double best=std::numeric_limits<double>::infinity();
  for(auto *w:choices){
   const QPoint delta=w->mapToGlobal(w->rect().center())-origin;
   const int along=dx?delta.x()*dx:delta.y()*dy;
   if(along<=3)continue;
   const int across=dx?std::abs(delta.y()):std::abs(delta.x());
   const double rank=along+1.8*across+0.005*across*across;
   if(rank<best){best=rank;target=w;}
  }
  if(!target)return;
  target->setFocus(Qt::OtherFocusReason);
  for(auto *area:root->findChildren<QScrollArea*>())
   if(area->widget()&&(area->widget()==target||area->widget()->isAncestorOf(target)))
    area->ensureWidgetVisible(target,18,18);
 }
 void activate(){
  if(auto *popup=QApplication::activePopupWidget()){
   auto *f=QApplication::focusWidget();key(f?f:popup,Qt::Key_Return);return;
  }
  auto *f=QApplication::focusWidget();
  if(auto *modal=QApplication::activeModalWidget()){
   f=modalFocus(modal);
   if(f==modal)if(auto *box=qobject_cast<QMessageBox*>(modal))if(box->defaultButton())f=box->defaultButton();
   if(auto *button=qobject_cast<QAbstractButton*>(f)){if(button->isEnabled())button->click();}
   else key(f?f:modal,Qt::Key_Return);
   return;
  }
  if(!f||!(f==root||root->isAncestorOf(f))){navigate(0,1);return;}
  if(auto *b=qobject_cast<QAbstractButton*>(f)){if(b->isEnabled())b->click();}
  else if(auto *c=qobject_cast<QComboBox*>(f))c->showPopup();
  else key(f,Qt::Key_Return);
 }
 void back(){
  if(auto *popup=QApplication::activePopupWidget()){
   key(popup,Qt::Key_Escape);return;
  }
  if(auto *modal=QApplication::activeModalWidget()){
   key(modal,Qt::Key_Escape);return;
  }
  auto *stack=root->findChild<QStackedWidget*>("pageStack");
  if(stack&&stack->currentIndex()!=0)page(false);
  else if(auto *play=root->findChild<QAbstractButton*>("playGameButton"))play->setFocus(Qt::OtherFocusReason);
 }
 void page(bool settings){
  if(auto *modal=QApplication::activeModalWidget()){
   key(modalFocus(modal),settings?Qt::Key_Tab:Qt::Key_Backtab,settings?Qt::NoModifier:Qt::ShiftModifier);
   return;
  }
  for(auto *b:root->findChildren<QAbstractButton*>("navButton"))
   if(b->text()==(settings?QStringLiteral("Настройки"):QStringLiteral("Перевод"))){
    b->click();b->setFocus(Qt::OtherFocusReason);break;
   }
 }
 QWidget *root{};
 QLabel *status{};
 QPointer<QFocusFrame> focusFrame;
 SDL_Gamepad *pad{};
 bool initialized{};
 bool gamepadMode{};
 int heldDirection{};
 qint64 nextRepeat{};
 qint64 lastAxis{};
};
void attachController(QWidget *window,bool hardware){new Controller(window,hardware);}
bool testController(QWidget *window){
 auto *c=dynamic_cast<Controller*>(window->findChild<QObject*>("controllerObject"));auto *stack=window->findChild<QStackedWidget*>("pageStack");
 if(!c||!stack)return false;
 c->press(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);QApplication::processEvents();
 if(stack->currentIndex()!=1)return false;
 c->press(SDL_GAMEPAD_BUTTON_EAST);QApplication::processEvents();
 if(stack->currentIndex()!=0)return false;
 c->press(SDL_GAMEPAD_BUTTON_DPAD_DOWN);QApplication::processEvents();
 auto *focus=QApplication::focusWidget();
 auto *frame=window->findChild<QFocusFrame*>("gamepadFocusFrame");
 if(!focus||!focus->isVisible()||!focus->isEnabled()||!frame||!frame->isVisible()||frame->widget()!=focus)return false;

 QMessageBox question(QMessageBox::Question,"Проверка","Продолжить?",QMessageBox::Yes|QMessageBox::No,window);
 question.setWindowModality(Qt::WindowModal);question.setDefaultButton(QMessageBox::No);
 question.show();question.activateWindow();QApplication::processEvents();
 if(QApplication::activeModalWidget()!=&question)return false;
 auto *no=question.button(QMessageBox::No);auto *yes=question.button(QMessageBox::Yes);
 if(!no||!yes)return false;
 no->setFocus(Qt::OtherFocusReason);QApplication::processEvents();
 c->press(SDL_GAMEPAD_BUTTON_DPAD_LEFT);QApplication::processEvents();
 if(QApplication::focusWidget()!=yes||!c->focusVisibleFor(yes))return false;
 c->press(SDL_GAMEPAD_BUTTON_SOUTH);QApplication::processEvents();
 if(question.isVisible()||question.result()!=QMessageBox::Yes)return false;

 QMessageBox cancel(QMessageBox::Question,"Проверка","Продолжить?",QMessageBox::Yes|QMessageBox::No,window);
 cancel.setWindowModality(Qt::WindowModal);cancel.show();cancel.activateWindow();QApplication::processEvents();
 c->press(SDL_GAMEPAD_BUTTON_EAST);QApplication::processEvents();
 if(cancel.isVisible())return false;

 QTemporaryDir temp;if(!temp.isValid())return false;
 const QString file=temp.filePath("translation.pak");QFile fixture(file);
 if(!fixture.open(QIODevice::WriteOnly)||fixture.write("test")!=4)return false;
 fixture.close();
 QFileDialog dialog(window,"Выберите пакет",temp.path());
 dialog.setOption(QFileDialog::DontUseNativeDialog);dialog.setFileMode(QFileDialog::ExistingFile);
 dialog.setWindowModality(Qt::WindowModal);dialog.show();dialog.activateWindow();QApplication::processEvents();
 auto *edit=dialog.findChild<QLineEdit*>("fileNameEdit");if(!edit)return false;
 edit->setText(file);edit->setFocus(Qt::OtherFocusReason);QApplication::processEvents();
 c->press(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);QApplication::processEvents();
 auto *afterTab=QApplication::focusWidget();
 if(!afterTab||afterTab==edit||!dialog.isAncestorOf(afterTab))return false;
 edit->setFocus(Qt::OtherFocusReason);QApplication::processEvents();
 c->press(SDL_GAMEPAD_BUTTON_SOUTH);QApplication::processEvents();
 return !dialog.isVisible()&&dialog.result()==QDialog::Accepted;
}
