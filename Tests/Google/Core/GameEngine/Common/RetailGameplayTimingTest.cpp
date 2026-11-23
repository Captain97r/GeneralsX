/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <cfenv>
#include <gtest/gtest.h>

#include "Common/INI.h"
#include "Common/NameKeyGenerator.h"
#include "GameLogic/Weapon.h"

#include "FileSystemTestFixture.h"

class RetailGameplayTiming : public FileSystemTestFixture
{
protected:
	void SetUp() override
	{
		// INI::load installs the simulation's rounding and precision modes.
		std::fegetenv(&m_previousFpEnvironment);
		FileSystemTestFixture::SetUp();
		m_names.init();
		TheNameKeyGenerator = &m_names;
		TheWeaponStore = &m_weapons;
	}

	void TearDown() override
	{
		TheWeaponStore = m_previousWeapons;
		TheNameKeyGenerator = m_previousNames;
		prepareSimulationRateForGame(m_previousRate);
		FileSystemTestFixture::TearDown();
		std::fesetenv(&m_previousFpEnvironment);
	}

private:
	std::fenv_t m_previousFpEnvironment{};
	const Int m_previousRate = getSimulationRate();
	NameKeyGenerator *const m_previousNames = TheNameKeyGenerator;
	WeaponStore *const m_previousWeapons = TheWeaponStore;
	NameKeyGenerator m_names;
	WeaponStore m_weapons;
};

TEST_F(RetailGameplayTiming, ParsedWeaponDeadlinesPreserveRetailCadence)
{
	writeLooseFile("Weapon.ini",
		"Weapon QuadCannonGunUpgradeOne\n"
		"  DelayBetweenShots = 50\n"
		"  ClipReloadTime = 0\n"
		"End\n"
		"Weapon VariableDelay\n"
		"  DelayBetweenShots = Min:50 Max:150\n"
		"  ClipReloadTime = 150\n"
		"  PreAttackDelay = 250\n"
		"End\n");
	prepareSimulationRateForGame(60);
	INI ini;
	ini.load("Weapon.ini", INI_LOAD_OVERWRITE, nullptr);
	const WeaponTemplate *quad = TheWeaponStore->findWeaponTemplate("QuadCannonGunUpgradeOne");
	const WeaponTemplate *variable = TheWeaponStore->findWeaponTemplate("VariableDelay");
	ASSERT_NE(quad, nullptr);
	ASSERT_NE(variable, nullptr);

	for (const Int rate : {30, 60, 30})
	{
		prepareSimulationRateForGame(rate);
		const Int ticks = rate / BaseFps;
		WeaponBonus bonus;
		EXPECT_EQ(2 * ticks, quad->getDelayBetweenShots(bonus));
		EXPECT_EQ(0, quad->getClipReloadTime(bonus));
		EXPECT_EQ(5 * ticks, variable->getClipReloadTime(bonus));
		EXPECT_EQ(8 * ticks, variable->getPreAttackDelay(bonus));
		for (Int i = 0; i < 16; ++i)
		{
			const Int delay = variable->getDelayBetweenShots(bonus);
			EXPECT_GE(delay, 2 * ticks);
			EXPECT_LE(delay, 5 * ticks);
			EXPECT_EQ(0, delay % ticks);
		}
		bonus.setField(WeaponBonus::RATE_OF_FIRE, 1.5f);
		bonus.setField(WeaponBonus::PRE_ATTACK, 0.7f);
		EXPECT_EQ(ticks, quad->getDelayBetweenShots(bonus));
		EXPECT_EQ(3 * ticks, variable->getClipReloadTime(bonus));
		EXPECT_EQ(5 * ticks, variable->getPreAttackDelay(bonus));
	}
}
