// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Settings/JVSControlsWidget.h"

#include "Settings/ControllerSettingsWindow.h"
#include "Settings/ControllerSettingWidgetBinder.h"
#include "Settings/InputBindingWidget.h"

#include "pcsx2/DEV9/ACJV.h"

#include <QtCore/QCoreApplication>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>

JVSControlsWidget::JVSControlsWidget(QWidget* parent, ControllerSettingsWindow* dialog)
	: QWidget(parent)
	, m_dialog(dialog)
{
	m_ui.setupUi(this);

	for (int i = RAYS_PCB; i <= MIU_IO_JPN_GUN_EXTENTI; i++)
		m_ui.boardType->addItem(QString::fromUtf8(ACJV::GetBoardDisplayName(static_cast<BOARDID>(i))));
	m_ui.boardType->setCurrentIndex(static_cast<int>(ACJV::GetCurrentBoardID()));

	bindDIPSwitchWidgets();
	bindSystemButtonWidgets();
	bindDrumWidgets();

	ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(m_dialog->getProfileSettingsInterface(), m_ui.suppressDaemon,
		ACJV::CONFIG_SECTION, "SuppressDaemon", true);
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

void JVSControlsWidget::bindSystemButtonWidgets()
{
	QGridLayout* layout = m_ui.systemButtonsLayout;
	SettingsInterface* sif = m_dialog->getProfileSettingsInterface();

	struct ButtonEntry {
		const char* label;
		const char* config_key;
	};
	static constexpr ButtonEntry entries[] = {
		{"P1 Service", "P1_Service"},
		{"P2 Service", "P2_Service"},
		{"Insert Coin P1", "Coin1"},
		{"Insert Coin P2", "Coin2"},
	};

	for (int i = 0; i < static_cast<int>(std::size(entries)); i++)
	{
		QLabel* label = new QLabel(tr(entries[i].label), this);
		layout->addWidget(label, i, 0);

		InputBindingWidget* bind = new InputBindingWidget(
			this, sif, InputBindingInfo::Type::Button, ACJV::CONFIG_SECTION, entries[i].config_key);
		bind->setMinimumWidth(225);
		bind->setMaximumWidth(225);
		layout->addWidget(bind, i, 1);
	}
}

void JVSControlsWidget::bindDrumWidgets()
{
	SettingsInterface* sif = m_dialog->getProfileSettingsInterface();

	QGroupBox* group = new QGroupBox(tr("Taiko Drum"), this);
	QGridLayout* layout = new QGridLayout(group);

	struct DrumEntry {
		const char* label;
		const char* config_key;
	};
	// config_key must match the s_jvs_drum_bindings names in ACJV.cpp (which carry the real analog channels).
	static constexpr DrumEntry entries[] = {
		{"P1 Don Left (inner, red)", "P1_DonLeft"},
		{"P1 Don Right (inner, red)", "P1_DonRight"},
		{"P1 Ka Left (outer, blue)", "P1_KaLeft"},
		{"P1 Ka Right (outer, blue)", "P1_KaRight"},
		{"P2 Don Left (inner, red)", "P2_DonLeft"},
		{"P2 Don Right (inner, red)", "P2_DonRight"},
		{"P2 Ka Left (outer, blue)", "P2_KaLeft"},
		{"P2 Ka Right (outer, blue)", "P2_KaRight"},
	};

	for (int i = 0; i < static_cast<int>(std::size(entries)); i++)
	{
		layout->addWidget(new QLabel(tr(entries[i].label), group), i, 0);

		InputBindingWidget* bind = new InputBindingWidget(
			group, sif, InputBindingInfo::Type::Button, ACJV::CONFIG_SECTION, entries[i].config_key);
		bind->setMinimumWidth(225);
		bind->setMaximumWidth(225);
		layout->addWidget(bind, i, 1);
	}

	m_ui.scrollLayout->addWidget(group);
}

#include "moc_JVSControlsWidget.cpp"
