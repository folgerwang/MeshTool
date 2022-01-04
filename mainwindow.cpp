#include <iostream>
#include <fstream>
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "kmlfileparser.h"
#include "GpaDumpAnalyzeTool.h"
#include <QDoubleValidator>
#include <QFileDialog>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpacerItem>
#include <QPushButton>
#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QLoggingCategory>
#include <QClipboard>
#include <QPixmap>

#include "base.h"
#include "worlddata.h"
#include "coregeographic.h"
#include "debugout.h"

WorldData g_world;

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_download(new FileDownloader(this))
{
    ui->setupUi(this);

    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);

    // load config file.

    earth_mesh_init();

    ifstream in_file;
    in_file.open("mesh_tool.cfg", ifstream::binary);
    if (in_file)
    {
        // get length of file:
        in_file.seekg (0, in_file.end);
        ifstream::pos_type length = in_file.tellg();
        in_file.seekg (0, in_file.beg);

        if (length > 8)
        {
            in_file.read(reinterpret_cast<char*>(&g_world.reference_pos), sizeof(core::vec2d));
            in_file.read(reinterpret_cast<char*>(&g_world.scissor_bbox), sizeof(core::bounds2d));
        }

        ui->longitudeValue->setText(to_string(g_world.reference_pos.x).c_str());
        ui->latitudeValue->setText(to_string(g_world.reference_pos.y).c_str());
        ui->longitudeStart->setText(to_string(g_world.scissor_bbox.bb_min.x).c_str());
        ui->latitudeStart->setText(to_string(g_world.scissor_bbox.bb_min.y).c_str());
        ui->longitudeEnd->setText(to_string(g_world.scissor_bbox.bb_max.x).c_str());
        ui->latitudeEnd->setText(to_string(g_world.scissor_bbox.bb_max.y).c_str());
        in_file.close();
    }

    ui->loadSaveProgressBar->reset();
    ui->loadSaveProgressBar->setRange(0, 100);
}

MainWindow::~MainWindow()
{
    delete ui;
    delete m_download;
}

QString CheckGpsCoords(vector<double> coords, QList<QLineEdit *> fields, uint32_t idx_ofs)
{
    QString error_body = "";

    if (abs(coords[idx_ofs + 0]) > 180.0 || abs(coords[idx_ofs + 1]) > 90.0)
    {
        if (!(abs(coords[idx_ofs + 0]) <= 180.0))
        {
            error_body += "lon: " + fields[int32_t(idx_ofs) + 0]->text();
        }
        if (!(abs(coords[idx_ofs + 1]) <= 90.0))
        {
            if (!error_body.isEmpty())
            {
                error_body += "; ";
            }

            error_body += "lat: " + fields[int32_t(idx_ofs) + 1]->text();
        }
    }

    return error_body;
}

void MainWindow::on_ImportDumpData_clicked()
{
    ui->loadSaveProgressBar->reset();

    QDialog dialog(this, Qt::WindowSystemMenuHint | Qt::WindowTitleHint);
    dialog.setWindowTitle("Import Usgs Map Data");
    // Use a layout allowing to have a label next to each field
    QFormLayout form(&dialog);

    QButtonGroup buttomGroup(&dialog);

    QPushButton importMapButton(&dialog);
    importMapButton.setText("import usgs map data");
    form.addRow(&importMapButton);

    QObject::connect(&importMapButton, SIGNAL (released()),this, SLOT (handleImportMapButton()));

    // Add some standard buttons (Cancel/Ok) at the bottom of the dialog
    QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                               Qt::Horizontal, &dialog);
    form.addRow(&buttonBox);
    QObject::connect(&buttonBox, SIGNAL(accepted()), &dialog, SLOT(accept()));
    QObject::connect(&buttonBox, SIGNAL(rejected()), &dialog, SLOT(reject()));

    // Show the dialog as modal
    if (dialog.exec() == QDialog::Accepted)
    {
        bool valid_inputs = !selectUsgsMaps.empty();// && !selectUsgsMapTextures.empty();

        if (valid_inputs)
        {
            vector<shared_ptr<string>> tex_file_name_list;
            for (int i = 0; i < selectUsgsMapTextures.size(); i++)
            {
                tex_file_name_list.push_back(make_unique<string>(selectUsgsMapTextures[i].toUtf8().constData()));
            }

            vector<unique_ptr<string>> map_file_name_list;
            for (int i = 0; i < selectUsgsMaps.size(); i++)
            {
                map_file_name_list.push_back(make_unique<string>(selectUsgsMaps[i].toUtf8().constData()));
            }

            vector<DumppedTextureInfo> dumpped_texture_info_list;
            DumpUSGSTexture(tex_file_name_list, dumpped_texture_info_list);

            BatchMeshData* batch_mesh_data = new BatchMeshData;
            batch_mesh_data->reference_pos = g_world.reference_pos;
            batch_mesh_data->scissor_bbox = g_world.scissor_bbox;
            batch_mesh_data->is_google_dump = false;
            batch_mesh_data->is_spline_mesh = false;
            //DumpUSGSData(map_file_name_list, dumpped_texture_info_list, true, batch_mesh_data);
            vector<MapInfo> map_info_list;
            PreLoadUSGSData(m_download, map_file_name_list, map_info_list);
            uint32_t grid_size_by_angle = 256;
            GenerateMeshData(map_info_list, grid_size_by_angle, batch_mesh_data);

            if (batch_mesh_data)
            {
                g_world.mesh_data_batches.push_back(batch_mesh_data);
                g_world.bbox_ws += batch_mesh_data->bbox_ws;
            }
        }
    }
}

void MainWindow::handleImportMapButton()
{
    selectUsgsMaps = QFileDialog::getOpenFileNames(this,
           tr("Open Usgs Map Files"), "",
           tr("Map File (*.img);;All Files (*)"));
}

void MainWindow::on_GoogleEarthDump_clicked()
{
    ui->loadSaveProgressBar->reset();

    QStringList fileNameList = QFileDialog::getOpenFileNames(this,
           tr("Open GPA Dump Files"), "",
           tr("Gpa Dump File (*.gpa_frame);;All Files (*)"));

    if (!fileNameList.empty())
    {
        vector<string> file_name_list;
        for (int i = 0; i < fileNameList.size(); i++)
        {
            file_name_list.push_back(fileNameList[i].toUtf8().constData());
        }

        BatchMeshData* batch_mesh_data = new BatchMeshData;
        batch_mesh_data->reference_pos = g_world.reference_pos;
        batch_mesh_data->scissor_bbox = g_world.scissor_bbox;
        batch_mesh_data->is_google_dump = true;
        batch_mesh_data->is_spline_mesh = false;
        DumpGoogleEarthMeshes(file_name_list, batch_mesh_data, ui->loadSaveProgressBar);

        if (batch_mesh_data)
        {
            g_world.mesh_data_batches.push_back(batch_mesh_data);
            g_world.bbox_ws += batch_mesh_data->bbox_ws;
            g_world.bbox_gps += batch_mesh_data->bbox_gps;
        }

        ui->loadSaveProgressBar->reset();
    }
}

void MainWindow::on_ExportMesh_clicked()
{
    ui->loadSaveProgressBar->reset();

    QString fileName = QFileDialog::getSaveFileName(this,
           tr("Export Fbx/Ma File"), "",
           tr("Fbx File (*.fbx);;Maya File (*.ma);;All Files (*)"));

    if (g_world.mesh_data_batches.size() > 0)
    {
        string file_name = fileName.toUtf8().constData();
        string ext_name = file_name.substr(file_name.rfind('.'));
        if (ext_name == ".ma")
        {
            ExportMaMeshFile(file_name, g_world.mesh_data_batches, ui->loadSaveProgressBar);
        }
        else
        {
            ExportFbxMeshFile(file_name, g_world.mesh_data_batches, ui->loadSaveProgressBar);
        }
    }

    ui->loadSaveProgressBar->reset();
}

void MainWindow::on_ReferencePoint_clicked()
{
    ui->loadSaveProgressBar->reset();

    QDialog dialog(this, Qt::WindowSystemMenuHint | Qt::WindowTitleHint);
    dialog.setWindowTitle("Reference Gps Coordinate");
    // Use a layout allowing to have a label next to each field
    QFormLayout form(&dialog);

    // Add some text above the fields
    form.addRow(new QLabel("Gps Coordinate:"));

    // Add the lineEdits with their respective labels
    QList<QLineEdit *> fields;
    QLineEdit *lineEdit = new QLineEdit(&dialog);
    lineEdit->setValidator( new QDoubleValidator(-180.0, 180.0, 10, &dialog) );
    QString label_long = QString("Longitude:");
    form.addRow(label_long, lineEdit);
    fields << lineEdit;

    lineEdit = new QLineEdit(&dialog);
    lineEdit->setValidator( new QDoubleValidator(-90.0, 90.0, 10, &dialog) );
    QString label_lat = QString("Latitude:");
    form.addRow(label_lat, lineEdit);
    fields << lineEdit;

    // Add some standard buttons (Cancel/Ok) at the bottom of the dialog
    QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                               Qt::Horizontal, &dialog);
    form.addRow(&buttonBox);
    QObject::connect(&buttonBox, SIGNAL(accepted()), &dialog, SLOT(accept()));
    QObject::connect(&buttonBox, SIGNAL(rejected()), &dialog, SLOT(reject()));

    // Show the dialog as modal
    if (dialog.exec() == QDialog::Accepted)
    {
        // If the user didn't dismiss the dialog, do something with the fields
        vector<double> coords;
        foreach(QLineEdit * lineEdit, fields)
        {
            if (lineEdit->text().isEmpty())
            {
                coords.push_back(-1000.0);
            }
            else
            {
                double value = stod(string(lineEdit->text().toUtf8().constData()));
                coords.push_back(value);
            }
        }

        QString error_body = CheckGpsCoords(coords, fields, 0);

        if (!error_body.isEmpty())
        {
            QMessageBox messageBox;
            messageBox.critical(nullptr,"Error: Gps Coord Out of Range!", error_body);
            messageBox.setFixedSize(500,200);
        }
        else
        {
            ui->longitudeValue->setText(fields[0]->text());
            ui->latitudeValue->setText(fields[1]->text());

            g_world.reference_pos = core::vec2d(coords[0], coords[1]);

            ofstream out_file;
            out_file.open("mesh_tool.cfg", ofstream::binary);
            if (out_file)
            {
                out_file.write(reinterpret_cast<char*>(&g_world.reference_pos), sizeof(core::vec2d));
                out_file.write(reinterpret_cast<char*>(&g_world.scissor_bbox), sizeof(core::bounds2d));
                out_file.close();
            }
        }
    }
}

void MainWindow::on_RegionSelectButton_clicked()
{
    ui->loadSaveProgressBar->reset();

    QDialog dialog(this, Qt::WindowSystemMenuHint | Qt::WindowTitleHint);
    dialog.setWindowTitle("Import Usgs Map Data");
    // Use a layout allowing to have a label next to each field
    QFormLayout form(&dialog);

    // Add some text above the fields
    form.addRow(new QLabel("Scissor Gps Coordinates:"));
    form.addRow(new QLabel("Top Left Gps Coordinate:"));
    // Add the lineEdits with their respective labels
    QList<QLineEdit *> fields;
    QLineEdit *lineEdit = new QLineEdit(&dialog);
    lineEdit->setValidator( new QDoubleValidator(-180.0, 180.0, 10, &dialog) );
    QString label_long = QString("Longitude:");
    form.addRow(label_long, lineEdit);
    fields << lineEdit;

    lineEdit = new QLineEdit(&dialog);
    lineEdit->setValidator( new QDoubleValidator(-90.0, 90.0, 10, &dialog) );
    QString label_lat = QString("Latitude:");
    form.addRow(label_lat, lineEdit);
    fields << lineEdit;

    form.addRow(new QLabel("Bottom Right Gps Coordinate:"));
    // Add the lineEdits with their respective labels
    lineEdit = new QLineEdit(&dialog);
    lineEdit->setValidator( new QDoubleValidator(-180.0, 180.0, 10, &dialog) );
    QString label_long_1 = QString("Longitude:");
    form.addRow(label_long_1, lineEdit);
    fields << lineEdit;

    lineEdit = new QLineEdit(&dialog);
    lineEdit->setValidator( new QDoubleValidator(-90.0, 90.0, 10, &dialog) );
    QString label_lat_1 = QString("Latitude:");
    form.addRow(label_lat_1, lineEdit);
    fields << lineEdit;

    // Add some standard buttons (Cancel/Ok) at the bottom of the dialog
    QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                               Qt::Horizontal, &dialog);
    form.addRow(&buttonBox);
    QObject::connect(&buttonBox, SIGNAL(accepted()), &dialog, SLOT(accept()));
    QObject::connect(&buttonBox, SIGNAL(rejected()), &dialog, SLOT(reject()));

    // Show the dialog as modal
    if (dialog.exec() == QDialog::Accepted)
    {
        vector<double> coords;
        foreach(QLineEdit * lineEdit, fields)
        {
            if (lineEdit->text().isEmpty())
            {
                coords.push_back(-1000.0);
            }
            else
            {
                double value = stod(string(lineEdit->text().toUtf8().constData()));
                coords.push_back(value);
            }
        }

        QString error_body_0 = CheckGpsCoords(coords, fields, 0);
        QString error_body_1 = CheckGpsCoords(coords, fields, 2);

        if (!error_body_1.isEmpty())
        {
            if (!error_body_0.isEmpty())
            {
                error_body_0 += "<==>" + error_body_1;
            }
            else
            {
                error_body_0 = error_body_1;
            }
        }

        bool valid_inputs = true;

        if (!error_body_0.isEmpty() || max(abs(coords[0] - coords[2]), abs(coords[1] - coords[3])) < 1e-30)
        {
            if (!error_body_0.isEmpty())
            {
                QMessageBox messageBox;
                messageBox.critical(nullptr,"Error: Gps Coord Out of Range!", error_body_0);
                messageBox.setFixedSize(500,200);

                valid_inputs = false;
            }
        }

        if (valid_inputs)
        {
            g_world.scissor_bbox.Reset();
            g_world.scissor_bbox += core::vec2d(coords[0], coords[1]);
            g_world.scissor_bbox += core::vec2d(coords[2], coords[3]);

            ui->longitudeStart->setText(to_string(g_world.scissor_bbox.bb_min.x).c_str());
            ui->latitudeStart->setText(to_string(g_world.scissor_bbox.bb_min.y).c_str());
            ui->longitudeEnd->setText(to_string(g_world.scissor_bbox.bb_max.x).c_str());
            ui->latitudeEnd->setText(to_string(g_world.scissor_bbox.bb_max.y).c_str());

            ofstream out_file;
            out_file.open("mesh_tool.cfg", ofstream::binary);
            if (out_file)
            {
                out_file.write(reinterpret_cast<char*>(&g_world.reference_pos), sizeof(core::vec2d));
                out_file.write(reinterpret_cast<char*>(&g_world.scissor_bbox), sizeof(core::bounds2d));
                out_file.close();
            }
        }
    }
}

void MainWindow::on_ImportFbxData_clicked()
{
    ui->loadSaveProgressBar->reset();

    selectFbxFile = QFileDialog::getOpenFileName(this,
           tr("Open Fbx File"), "",
           tr("Fbx File (*.fbx);;All Files (*)"));

    ImportAndTransformFbxMeshFile(selectFbxFile.toUtf8().constData());
}

void MainWindow::on_ImportKml_clicked()
{
    ui->loadSaveProgressBar->reset();

    QStringList fileNameList = QFileDialog::getOpenFileNames(this,
           tr("Open Kml Files"), "",
           tr("Kml File (*.kml);;All Files (*)"));

    if (!fileNameList.empty())
    {
        vector<string> file_name_list;
        for (int i = 0; i < fileNameList.size(); i++)
        {
            file_name_list.push_back(fileNameList[i].toUtf8().constData());
        }

        BatchMeshData* batch_mesh_data = new BatchMeshData;
        batch_mesh_data->reference_pos = g_world.reference_pos;
        batch_mesh_data->scissor_bbox = g_world.scissor_bbox;
        batch_mesh_data->is_google_dump = false;
        batch_mesh_data->is_spline_mesh = true;

        DumpKmlSplineMeshes(file_name_list, batch_mesh_data, ui->loadSaveProgressBar);

        if (batch_mesh_data)
        {
            g_world.mesh_data_batches.push_back(batch_mesh_data);
            g_world.bbox_ws += batch_mesh_data->bbox_ws;
            g_world.bbox_gps += batch_mesh_data->bbox_gps;
        }
    }
}
