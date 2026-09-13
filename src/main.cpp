#include "main_window.h"

#include <QApplication>

/**
 * @brief Starts the PCM post-processing Qt Widgets frontend.
 */
int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("PcmProcess"));
    QApplication::setOrganizationDomain(QStringLiteral("pcm-process.local"));
    QApplication::setApplicationName(QStringLiteral("PcmProcess"));

    MainWindow window;
    window.show();

    return application.exec();
}
