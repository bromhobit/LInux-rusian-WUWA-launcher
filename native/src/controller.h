#pragma once
#include <functional>
class QWidget;
class QComboBox;
class QDialog;
union SDL_Event;
void attachController(QWidget *window, bool hardware, bool externalEvents = false,
                      std::function<bool()> active = {},
                      std::function<bool(int)> popupKey = {});
void dispatchControllerEvent(QWidget *window, const SDL_Event &event);
bool testController(QWidget *window);
bool testControllerPopup(QWidget *window, QComboBox *combo);
bool testControllerModal(QWidget *window, QDialog *dialog);
