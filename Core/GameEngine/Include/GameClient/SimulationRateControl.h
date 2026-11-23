/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

class GameWindow;

GameWindow *createGeneralsXSimulationRateButton(GameWindow *defaults, GameWindow *accept, GameWindow *cancel, int preferredRate);
void setSimulationRateButtonText(GameWindow *button, int rate);
