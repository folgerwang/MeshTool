#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QWebEngineView>
#include "imagedownload.h"

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_ImportDumpData_clicked();

    void on_GoogleEarthDump_clicked();

    void on_ExportMesh_clicked();

    void on_ReferencePoint_clicked();

    void handleImportMapButton();

    void on_RegionSelectButton_clicked();

    void on_ImportFbxData_clicked();

    void on_ImportKml_clicked();

private:

    Ui::MainWindow *ui;
    FileDownloader *m_download;

    QStringList selectUsgsMaps;
    QStringList selectUsgsMapTextures;
    QString selectFbxFile;
};

#endif // MAINWINDOW_H
