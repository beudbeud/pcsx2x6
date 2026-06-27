// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_JVSControlsWidget.h"

#include <QtWidgets/QWidget>

class ControllerSettingsWindow;

class JVSControlsWidget final : public QWidget
{
	Q_OBJECT

public:
	JVSControlsWidget(QWidget* parent, ControllerSettingsWindow* dialog);
	~JVSControlsWidget() override;

private:
	void bindDIPSwitchWidgets();
	void bindSystemButtonWidgets();
	void bindDrumWidgets();

	Ui::JVSControlsWidget m_ui;
	ControllerSettingsWindow* m_dialog;
};
