#include "main_window.h"

#include <QApplication>

/**
 * @brief Starts the PCM post-processing Qt Widgets frontend.
 */
int main(int argc, char *argv[])
{
    QApplication application(argc, argv);

    MainWindow window;
    window.show();

    return application.exec();
}
