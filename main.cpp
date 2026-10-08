#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>

#include <exiv2/exiv2.hpp>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("exif-editor");
    QApplication::setApplicationDisplayName("EXIF Editor");
    QApplication::setApplicationVersion("1.0");
    QApplication::setDesktopFileName("exif-editor");

    Exiv2::XmpParser::initialize();

#if EXIV2_TEST_VERSION(0, 27, 4)
    Exiv2::enableBMFF(true);   // HEIC / AVIF / CR3 support where Exiv2 was built with it
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription("View and edit EXIF metadata in image files.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "Image file to open.");
    parser.process(app);

    MainWindow w;
    w.show();
    if (!parser.positionalArguments().isEmpty())
        w.openFile(parser.positionalArguments().first());

    return app.exec();
}
