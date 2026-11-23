/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "PreRTS.h"
#include "GameClient/SimulationRateControl.h"

#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
#include "Common/NameKeyGenerator.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"
#include "GameLogic/GameLogic.h"

// GeneralsX @feature Codex 23/11/2026 Add the match clock to existing GeneralsX menu layouts, including installed BIG archives.
GameWindow *createGeneralsXSimulationRateButton(GameWindow *defaults, GameWindow *accept, GameWindow *cancel, Int preferredRate)
{
	if (!defaults || !accept || !cancel)
		return nullptr;
	GameWindow *parent = accept->winGetParent();
	if (!parent || defaults->winGetParent() != parent || cancel->winGetParent() != parent)
		return nullptr;

	const NameKeyType id = NAMEKEY("GeneralsXMenu.wnd:ButtonSimulationRate");
	GameWindow *button = TheWindowManager->winGetWindowFromId(parent, id);
	if (!button)
	{
		// Put the new setting in its own row above the three footer buttons.
		ICoord2D pos, size, parentSize;
		accept->winGetPosition(&pos.x, &pos.y);
		accept->winGetSize(&size.x, &size.y);
		parent->winGetSize(&parentSize.x, &parentSize.y);
		const Int gap = size.y / 5;
		const Int footerOffset = min(size.y + gap, max(0, parentSize.y - pos.y - size.y - gap));
		const Int settingY = pos.y + footerOffset - size.y - gap;

		WinInstanceData look;
		look = *accept->winGetInstanceData();
		look.m_text = nullptr;
		look.m_tooltip = nullptr;
		look.m_videoBuffer = nullptr;
		look.m_id = id;
		look.m_decoratedNameString = "GeneralsXMenu.wnd:ButtonSimulationRate";
		look.m_textLabelString.clear();
		look.m_tooltipString.clear();
		button = TheWindowManager->gogoGadgetPushButton(parent, accept->winGetStatus(),
			pos.x, settingY, size.x, size.y, &look, accept->winGetFont(), FALSE);
		if (!button)
			return nullptr;
		button->winSetOwner(accept->winGetOwner());
		for (GameWindow *footer : { defaults, accept, cancel })
		{
			footer->winGetPosition(&pos.x, &pos.y);
			footer->winSetPosition(pos.x, pos.y + footerOffset);
		}
	}

	const Bool inMatch = TheGameLogic->isInGame() && !TheGameLogic->isInShellGame();
	button->winEnable(!inMatch);
	setSimulationRateButtonText(button, inMatch ? getSimulationRate() : preferredRate);
	return button;
}

void setSimulationRateButtonText(GameWindow *button, int rate)
{
	if (button)
		GadgetButtonSetText(button, TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:NativeSimulationRate", L"Simulation: %d Hz", rate));
}
#endif
