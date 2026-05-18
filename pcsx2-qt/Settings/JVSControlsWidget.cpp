// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Settings/JVSControlsWidget.h"

#include "Settings/ControllerSettingsWindow.h"
#include "Settings/ControllerSettingWidgetBinder.h"
#include "Settings/InputBindingWidget.h"

#include "pcsx2/DEV9/ACJV.h"

#include <QtCore/QCoreApplication>

JVSControlsWidget::JVSControlsWidget(QWidget* parent, ControllerSettingsWindow* dialog)
	: QWidget(parent)
	, m_dialog(dialog)
{
	m_ui.setupUi(this);
	bindDIPSwitchWidgets();
}

JVSControlsWidget::~JVSControlsWidget() = default;

void JVSControlsWidget::bindDIPSwitchWidgets()
{
	const ACJV::DIPSwitchInfo& test_mode = ACJV::GetTestModeDIPSwitch();
	m_ui.testMode->setText(QCoreApplication::translate(ACJV::TRANSLATION_CONTEXT, test_mode.display_name));
	ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(m_dialog->getProfileSettingsInterface(), m_ui.testMode,
		ACJV::CONFIG_SECTION, test_mode.name, test_mode.default_value);
	m_ui.toggleTestMode->initialize(m_dialog->getProfileSettingsInterface(),
		InputBindingInfo::Type::Button, ACJV::CONFIG_SECTION, test_mode.toggle_bind_name);

	const ACJV::DIPSwitchInfo& video_voltage = ACJV::GetVideoVoltageDIPSwitch();
	m_ui.videoVoltage->setText(QCoreApplication::translate(ACJV::TRANSLATION_CONTEXT, video_voltage.display_name));
	ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(m_dialog->getProfileSettingsInterface(), m_ui.videoVoltage,
		ACJV::CONFIG_SECTION, video_voltage.name, video_voltage.default_value);
	m_ui.toggleVideoVoltage->initialize(m_dialog->getProfileSettingsInterface(),
		InputBindingInfo::Type::Button, ACJV::CONFIG_SECTION, video_voltage.toggle_bind_name);

	const ACJV::DIPSwitchInfo& monitor_sync_frequency = ACJV::GetMonitorSyncFrequencyDIPSwitch();
	m_ui.monitorSyncFrequency->setText(QCoreApplication::translate(ACJV::TRANSLATION_CONTEXT, monitor_sync_frequency.display_name));
	ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(m_dialog->getProfileSettingsInterface(), m_ui.monitorSyncFrequency,
		ACJV::CONFIG_SECTION, monitor_sync_frequency.name, monitor_sync_frequency.default_value);
	m_ui.toggleMonitorSyncFrequency->initialize(m_dialog->getProfileSettingsInterface(),
		InputBindingInfo::Type::Button, ACJV::CONFIG_SECTION, monitor_sync_frequency.toggle_bind_name);

	const ACJV::DIPSwitchInfo& video_sync_split = ACJV::GetVideoSyncSplitDIPSwitch();
	m_ui.videoSyncSplit->setText(QCoreApplication::translate(ACJV::TRANSLATION_CONTEXT, video_sync_split.display_name));
	ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(m_dialog->getProfileSettingsInterface(), m_ui.videoSyncSplit,
		ACJV::CONFIG_SECTION, video_sync_split.name, video_sync_split.default_value);
	m_ui.toggleVideoSyncSplit->initialize(m_dialog->getProfileSettingsInterface(),
		InputBindingInfo::Type::Button, ACJV::CONFIG_SECTION, video_sync_split.toggle_bind_name);
}

#include "moc_JVSControlsWidget.cpp"
