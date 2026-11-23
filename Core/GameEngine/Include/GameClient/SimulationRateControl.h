/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

class GameWindow;

GameWindow *createOptionsSimulationRateButton(GameWindow *options);
GameWindow *createLanSimulationRateButton(GameWindow *start, GameWindow *back);
void setSimulationRateButtonText(GameWindow *button, int rate);
