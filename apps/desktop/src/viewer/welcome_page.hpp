#pragma once
class QAction;
class QWidget;
namespace orbitforge::desktop {
QWidget *make_welcome_page(QAction *create, QAction *open, QAction *example);
}
