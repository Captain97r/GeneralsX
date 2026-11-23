/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "PreRTS.h"
#include "GameClient/SimulationRateControl.h"

#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
#include "Common/NameKeyGenerator.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"

// GeneralsX @feature Codex 23/11/2026 Place the selector relative to the buttons supplied by the game's or mod's layout.
static GameWindow *addRateButton(GameWindow *prototype, const char *name, Int x, Int y, Int width, Int height)
{
	GameWindow *parent = prototype->winGetParent();
	const NameKeyType id = TheNameKeyGenerator->nameToKey(name);
	if (GameWindow *existing = TheWindowManager->winGetWindowFromId(parent, id))
		return existing;

	WinInstanceData look;
	look = *prototype->winGetInstanceData();
	look.m_text = nullptr;
	look.m_tooltip = nullptr;
	look.m_videoBuffer = nullptr;
	look.m_id = id;
	look.m_decoratedNameString = name;
	look.m_textLabelString.clear();
	look.m_tooltipString.clear();
	GameWindow *button = TheWindowManager->gogoGadgetPushButton(parent, prototype->winGetStatus(),
		x, y, width, height, &look, prototype->winGetFont(), FALSE);
	if (button)
		button->winSetOwner(prototype->winGetOwner());
	return button;
}

GameWindow *createOptionsSimulationRateButton(GameWindow *options)
{
	GameWindow *back = TheWindowManager->winGetWindowFromId(options, NAMEKEY("OptionsMenu.wnd:ButtonBack"));
	if (!back || !back->winGetParent())
		return nullptr;

	ICoord2D pos, size;
	back->winGetPosition(&pos.x, &pos.y);
	back->winGetSize(&size.x, &size.y);
	// Opposite the Options title, above the divider. All coordinates follow the current UI scale.
	Int y = size.y / 8;
	if (GameWindow *divider = TheWindowManager->winGetWindowFromId(options, NAMEKEY("OptionsMenu.wnd:Line")))
	{
		ICoord2D line, parent;
		divider->winGetScreenPosition(&line.x, &line.y);
		back->winGetParent()->winGetScreenPosition(&parent.x, &parent.y);
		y = max(0, line.y - parent.y - size.y);
	}
	return addRateButton(back, "OptionsMenu.wnd:ButtonSimulationRate", pos.x, y, size.x, size.y);
}

GameWindow *createLanSimulationRateButton(GameWindow *start, GameWindow *back)
{
	if (!start || !back || !back->winGetParent())
		return nullptr;
	ICoord2D startPos, startSize, backPos, backSize, parent;
	start->winGetScreenPosition(&startPos.x, &startPos.y);
	start->winGetSize(&startSize.x, &startSize.y);
	back->winGetScreenPosition(&backPos.x, &backPos.y);
	back->winGetSize(&backSize.x, &backSize.y);
	back->winGetParent()->winGetScreenPosition(&parent.x, &parent.y);

	const Int gapStart = startPos.x + startSize.x;
	const Int gapWidth = backPos.x - gapStart;
	const Int width = min(backSize.x, gapWidth);
	if (width <= 0)
		return nullptr;
	return addRateButton(back, "LanGameOptionsMenu.wnd:ButtonSimulationRate",
		gapStart + (gapWidth - width) / 2 - parent.x, backPos.y - parent.y, width, backSize.y);
}

void setSimulationRateButtonText(GameWindow *button, int rate)
{
	if (button)
		GadgetButtonSetText(button, TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:NativeSimulationRate", L"Simulation: %d Hz", rate));
}
#endif
