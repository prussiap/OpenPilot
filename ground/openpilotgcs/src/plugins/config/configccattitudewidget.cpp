/**
 ******************************************************************************
 *
 * @file       configccattitudewidget.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup ConfigPlugin Config Plugin
 * @{
 * @brief Configure Attitude module on CopterControl
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include "configccattitudewidget.h"
#include "ui_ccattitude.h"
#include "utils/coordinateconversions.h"
#include "attitudesettings.h"
#include <QMutexLocker>
#include <QMessageBox>
#include <QDebug>
#include <QDesktopServices>
#include <QUrl>

ConfigCCAttitudeWidget::ConfigCCAttitudeWidget(QWidget *parent) :
        ConfigTaskWidget(parent),
        ui(new Ui_ccattitude)
{
    ui->setupUi(this);
    connect(ui->zeroBias,SIGNAL(clicked()),this,SLOT(startAccelCalibration()));


    setupButtons(ui->applyButton,ui->saveButton);
    addUAVObject("AttitudeSettings");

    // Connect the help button
    connect(ui->ccAttitudeHelp, SIGNAL(clicked()), this, SLOT(openHelp()));
    addUAVObjectToWidgetRelation("AttitudeSettings","ZeroDuringArming",ui->zeroGyroBiasOnArming);

    addUAVObjectToWidgetRelation("AttitudeSettings","BoardRotation",ui->rollBias,AttitudeSettings::BOARDROTATION_ROLL);
    addUAVObjectToWidgetRelation("AttitudeSettings","BoardRotation",ui->pitchBias,AttitudeSettings::BOARDROTATION_PITCH);
    addUAVObjectToWidgetRelation("AttitudeSettings","BoardRotation",ui->yawBias,AttitudeSettings::BOARDROTATION_YAW);
    addWidget(ui->zeroBias);
}

ConfigCCAttitudeWidget::~ConfigCCAttitudeWidget()
{
    delete ui;
}

void ConfigCCAttitudeWidget::attitudeRawUpdated(UAVObject * obj) {
    QMutexLocker locker(&startStop);

    ui->zeroBiasProgress->setValue((float) updates / NUM_ACCEL_UPDATES * 100);

    if(updates < NUM_ACCEL_UPDATES) {
        updates++;
        UAVObjectField * field = obj->getField(QString("accels"));
        x_accum.append(field->getDouble(0));
        y_accum.append(field->getDouble(1));
        z_accum.append(field->getDouble(2));
        field = obj->getField(QString("gyros"));
        x_gyro_accum.append(field->getDouble(0));
        y_gyro_accum.append(field->getDouble(1));
        z_gyro_accum.append(field->getDouble(2));;
    } else if ( updates == NUM_ACCEL_UPDATES ) {
	updates++;
        timer.stop();
        disconnect(obj,SIGNAL(objectUpdated(UAVObject*)),this,SLOT(attitudeRawUpdated(UAVObject*)));
        disconnect(&timer,SIGNAL(timeout()),this,SLOT(timeout()));

        float x_bias = listMean(x_accum) / ACCEL_SCALE;
        float y_bias = listMean(y_accum) / ACCEL_SCALE;
        float z_bias = (listMean(z_accum) + 9.81) / ACCEL_SCALE;

        float x_gyro_bias = listMean(x_gyro_accum) * 100.0f;
        float y_gyro_bias = listMean(y_gyro_accum) * 100.0f;
        float z_gyro_bias = listMean(z_gyro_accum) * 100.0f;
        obj->setMetadata(initialMdata);

        AttitudeSettings::DataFields attitudeSettingsData = AttitudeSettings::GetInstance(getObjectManager())->getData();
        // We offset the gyro bias by current bias to help precision
        attitudeSettingsData.AccelBias[0] += x_bias;
        attitudeSettingsData.AccelBias[1] += y_bias;
        attitudeSettingsData.AccelBias[2] += z_bias;
        attitudeSettingsData.GyroBias[0] = -x_gyro_bias;
        attitudeSettingsData.GyroBias[1] = -y_gyro_bias;
        attitudeSettingsData.GyroBias[2] = -z_gyro_bias;
        attitudeSettingsData.BiasCorrectGyro = AttitudeSettings::BIASCORRECTGYRO_TRUE;
        AttitudeSettings::GetInstance(getObjectManager())->setData(attitudeSettingsData);

    } else {
	// Possible to get here if weird threading stuff happens.  Just ignore updates.
	qDebug("Unexpected accel update received.");
    }
}

void ConfigCCAttitudeWidget::timeout() {
    QMutexLocker locker(&startStop);
    UAVDataObject * obj = dynamic_cast<UAVDataObject*>(getObjectManager()->getObject(QString("AttitudeRaw")));
    disconnect(obj,SIGNAL(objectUpdated(UAVObject*)),this,SLOT(attitudeRawUpdated(UAVObject*)));
    disconnect(&timer,SIGNAL(timeout()),this,SLOT(timeout()));

    QMessageBox msgBox;
    msgBox.setText(tr("Calibration timed out before receiving required updates."));
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.setDefaultButton(QMessageBox::Ok);
    msgBox.exec();

}

void ConfigCCAttitudeWidget::startAccelCalibration() {
    QMutexLocker locker(&startStop);

    updates = 0;
    x_accum.clear();
    y_accum.clear();
    z_accum.clear();
    x_gyro_accum.clear();
    y_gyro_accum.clear();
    z_gyro_accum.clear();

    // Disable gyro bias correction to see raw data
    AttitudeSettings::DataFields attitudeSettingsData = AttitudeSettings::GetInstance(getObjectManager())->getData();
    attitudeSettingsData.BiasCorrectGyro = AttitudeSettings::BIASCORRECTGYRO_FALSE;
    AttitudeSettings::GetInstance(getObjectManager())->setData(attitudeSettingsData);

    // Set up to receive updates
    UAVDataObject * obj = dynamic_cast<UAVDataObject*>(getObjectManager()->getObject(QString("AttitudeRaw")));
    connect(obj,SIGNAL(objectUpdated(UAVObject*)),this,SLOT(attitudeRawUpdated(UAVObject*)));

    // Set up timeout timer
    timer.start(10000);
    connect(&timer,SIGNAL(timeout()),this,SLOT(timeout()));

    // Speed up updates
    initialMdata = obj->getMetadata();
    UAVObject::Metadata mdata = initialMdata;
    mdata.flightTelemetryUpdateMode = UAVObject::UPDATEMODE_PERIODIC;
    mdata.flightTelemetryUpdatePeriod = 100;
    obj->setMetadata(mdata);

}

void ConfigCCAttitudeWidget::openHelp()
{

    QDesktopServices::openUrl( QUrl("http://wiki.openpilot.org/display/Doc/CopterControl+Attitude+Configuration", QUrl::StrictMode) );
}

void ConfigCCAttitudeWidget::enableControls(bool enable)
{
    if(ui->zeroBias)
        ui->zeroBias->setEnabled(enable);
    ConfigTaskWidget::enableControls(enable);

}
