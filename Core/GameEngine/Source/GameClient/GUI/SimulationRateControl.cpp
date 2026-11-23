/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "PreRTS.h"
#include "GameClient/SimulationRateControl.h"

#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
#include <algorithm>
#include <vector>
#include "Common/NameKeyGenerator.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"
#include "GameLogic/GameLogic.h"

static GameWindow *createRateCombo(GameWindow *parent, GameWindow *fieldLabel, Int x, Int y, Int width, Int height)
{
	WinInstanceData look;
	look.m_decoratedNameString = "GeneralsXMenu.wnd:ComboSimulationRate";
	look.m_id = TheNameKeyGenerator->nameToKey(look.m_decoratedNameString);
	look.m_style = GWS_COMBO_BOX | GWS_MOUSE_TRACK | GWS_TAB_STOP;

	ComboBoxData data{};
	data.maxDisplay = 2;
	data.maxChars = 5;
	// The gadget factory consumes these two templates when it creates its children.
	data.entryData = NEW EntryData{};
	data.entryData->maxTextLen = data.maxChars;
	data.listboxData = NEW ListboxData{};
	data.listboxData->listLength = data.maxDisplay;
	data.listboxData->columns = 1;
	data.listboxData->forceSelect = TRUE;

	GameWindow *combo = TheWindowManager->gogoGadgetComboBox(parent, WIN_STATUS_ENABLED | WIN_STATUS_IMAGE | WIN_STATUS_ABOVE,
		x, y, width, height, &look, &data, fieldLabel->winGetFont(), TRUE);
	if (!combo)
	{
		delete data.entryData;
		delete data.listboxData;
		return nullptr;
	}

	// Use the same arrow art and dark list as the game's resolution dropdown.
	GameWindow *arrow = GadgetComboBoxGetDropDownButton(combo);
	const Image *pressedArrow = TheWindowManager->winFindImage("VSliderDownButtonHiliteSelected");
	GadgetButtonSetEnabledImage(arrow, TheWindowManager->winFindImage("VSliderDownButtonEnabled"));
	GadgetButtonSetEnabledSelectedImage(arrow, pressedArrow);
	GadgetButtonSetDisabledImage(arrow, TheWindowManager->winFindImage("VSliderDownButtonDisabled"));
	GadgetButtonSetDisabledSelectedImage(arrow, nullptr);
	GadgetButtonSetHiliteImage(arrow, TheWindowManager->winFindImage("VSliderDownButtonHilite"));
	GadgetButtonSetHiliteSelectedImage(arrow, pressedArrow);

	const Color background = fieldLabel->winGetEnabledTextBorderColor();
	const Color accent = parent->winGetEnabledBorderColor(0);
	const Color disabled = fieldLabel->winGetDisabledTextColor();
	GadgetComboBoxSetColors(combo, background, accent, accent, accent, background, disabled, disabled, disabled,
		background, accent, accent, accent);
	GadgetComboBoxSetFont(combo, fieldLabel->winGetFont());
	GadgetComboBoxSetEnabledTextColors(combo, fieldLabel->winGetEnabledTextColor(), background);
	GadgetComboBoxSetHiliteTextColors(combo, fieldLabel->winGetHiliteTextColor(), fieldLabel->winGetHiliteTextBorderColor());
	GadgetComboBoxSetDisabledTextColors(combo, disabled, fieldLabel->winGetDisabledTextBorderColor());
	GadgetComboBoxAddEntry(combo, L"30 Hz", fieldLabel->winGetEnabledTextColor());
	GadgetComboBoxAddEntry(combo, L"60 Hz", fieldLabel->winGetEnabledTextColor());
	return combo;
}

static IRegion2D boundsOf(GameWindow *window)
{
	IRegion2D bounds;
	window->winGetRegion(&bounds);
	return bounds;
}

static Int contentBottom(GameWindow *panel)
{
	Int bottom = 0;
	for (GameWindow *child = panel->winGetChild(); child; child = child->winGetNext())
		if (!child->winIsHidden())
			bottom = max(bottom, boundsOf(child).hi.y);
	return bottom;
}

// Keep the existing columns and control sizes. Only reduce blank gaps between rows.
static Int fitPanel(GameWindow *panel, Int padding, Int maxGap)
{
	std::vector<GameWindow *> children;
	for (GameWindow *child = panel->winGetChild(); child; child = child->winGetNext())
		if (!child->winIsHidden())
			children.push_back(child);
	std::sort(children.begin(), children.end(), [](GameWindow *left, GameWindow *right) {
		return boundsOf(left).lo.y < boundsOf(right).lo.y;
	});

	Int previousBottom = 0;
	Int offset = 0;
	for (GameWindow *child : children)
	{
		const IRegion2D rect = boundsOf(child);
		if (previousBottom)
			offset += max(0, rect.lo.y - previousBottom - maxGap);
		child->winSetPosition(rect.lo.x, rect.lo.y - offset);
		previousBottom = max(previousBottom, rect.hi.y);
	}
	const Int height = previousBottom - offset + padding;
	panel->winSetSize(boundsOf(panel).width(), height);
	return height;
}

static void nameControl(GameWindow *window, const char *name)
{
	window->winSetWindowId(NAMEKEY(name));
	window->winGetInstanceData()->m_decoratedNameString = name;
}

static void addSectionLabel(GameWindow *section, GameWindow *source, const char *name, const char *stringKey,
	Int x, Int y, Int width, Int height)
{
	TextData text = *static_cast<TextData *>(source->winGetUserData());
	GameWindow *label = TheWindowManager->gogoGadgetStaticText(section, source->winGetStatus() | WIN_STATUS_NO_INPUT,
		x, y, width, height, source->winGetInstanceData(), &text, source->winGetFont(), FALSE);
	if (label)
	{
		nameControl(label, name);
		label->winGetInstanceData()->m_textLabelString = stringKey;
		UnicodeString caption = TheGameText->fetch(stringKey);
		label->winSetText(caption);
		GadgetStaticTextSetText(label, caption);
	}
}

// GeneralsX @feature Codex 23/11/2026 Reuse the theme and actual bounds of the installed GeneralsX layout.
static GameWindow *addSimulationSection(GameWindow *menu, GameWindow *defaults, GameWindow *accept, GameWindow *cancel)
{
	GameWindow *camera = TheWindowManager->winGetWindowFromId(menu, NAMEKEY("GeneralsXMenu.wnd:PanelCamera"));
	GameWindow *graphics = TheWindowManager->winGetWindowFromId(menu, NAMEKEY("GeneralsXMenu.wnd:PanelGraphics"));
	GameWindow *title = TheWindowManager->winGetWindowFromId(menu, NAMEKEY("GeneralsXMenu.wnd:LabelGraphicsTitle"));
	GameWindow *field = TheWindowManager->winGetWindowFromId(menu, NAMEKEY("GeneralsXMenu.wnd:LabelDrawDistance"));
	if (!camera || !graphics || !title || !field)
		return nullptr;

	const IRegion2D header = boundsOf(title);
	const IRegion2D fieldRect = boundsOf(field);
	const IRegion2D graphicsRect = boundsOf(graphics);
	const IRegion2D footer = boundsOf(accept);
	const Int padding = max(1, header.lo.y);
	const Int rowGap = max(1, padding / 2);
	const Int rowY = header.hi.y + rowGap;
	const Int rowHeight = max(fieldRect.height(), TheWindowManager->winFontHeight(field->winGetFont()) + rowGap);
	// The native combo expands downward by (font height + 2) per entry, plus four pixels of border.
	const Int popupHeight = 2 * (TheWindowManager->winFontHeight(field->winGetFont()) + 2) + 4;
	const Int sectionHeight = rowY + rowHeight + popupHeight + padding;
	const Int cameraTop = boundsOf(camera).lo.y;
	const Int menuHeight = boundsOf(menu).height();
	const Int neededHeight = cameraTop + contentBottom(camera) + contentBottom(graphics) +
		sectionHeight + footer.height() + 6 * padding;
	const Int maxGap = neededHeight > menuHeight ? rowGap : menuHeight;

	const Int graphicsY = cameraTop + fitPanel(camera, padding, maxGap) + padding;
	graphics->winSetPosition(graphicsRect.lo.x, graphicsY);
	const Int sectionY = graphicsY + fitPanel(graphics, padding, maxGap) + padding;
	GameWindow *section = TheWindowManager->winCreate(menu, graphics->winGetStatus(),
		graphicsRect.lo.x, sectionY, graphicsRect.width(), sectionHeight, nullptr, graphics->winGetInstanceData());
	if (!section)
		return nullptr;
	nameControl(section, "GeneralsXMenu.wnd:PanelSimulation");
	section->winSetDrawFunc(graphics->winGetDrawFunc());

	const Int comboWidth = footer.width();
	const Int comboX = graphicsRect.width() - fieldRect.lo.x - comboWidth;
	addSectionLabel(section, title, "GeneralsXMenu.wnd:LabelSimulationTitle", "GUI:SimulationSettings",
		header.lo.x, header.lo.y, header.width(), header.height());
	addSectionLabel(section, field, "GeneralsXMenu.wnd:LabelSimulationRate", "GUI:SimulationRate",
		fieldRect.lo.x, rowY, comboX - fieldRect.lo.x - padding, rowHeight);

	const Int footerY = max(footer.lo.y, sectionY + sectionHeight + padding);
	for (GameWindow *button : { defaults, accept, cancel })
		button->winSetPosition(boundsOf(button).lo.x, footerY);
	return createRateCombo(section, field, comboX, rowY, comboWidth, rowHeight);
}

GameWindow *createGeneralsXSimulationRateCombo(GameWindow *defaults, GameWindow *accept, GameWindow *cancel, Int preferredRate)
{
	if (!defaults || !accept || !cancel)
		return nullptr;
	GameWindow *menu = accept->winGetParent();
	if (!menu || defaults->winGetParent() != menu || cancel->winGetParent() != menu)
		return nullptr;
	GameWindow *combo = TheWindowManager->winGetWindowFromId(menu, NAMEKEY("GeneralsXMenu.wnd:ComboSimulationRate"));
	if (!combo)
		combo = addSimulationSection(menu, defaults, accept, cancel);
	if (combo)
	{
		const Bool inMatch = TheGameLogic->isInGame() && !TheGameLogic->isInShellGame();
		combo->winEnable(!inMatch);
		GadgetComboBoxSetSelectedPos(combo, (inMatch ? getSimulationRate() : preferredRate) == 60 ? 1 : 0);
	}
	return combo;
}
#endif
