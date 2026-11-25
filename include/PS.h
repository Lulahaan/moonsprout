#ifndef _PS_H
#define _PS_H

#include "types.h"

struct Vec;
struct JAISound;

namespace Game {
struct EnemyBase;
} // namespace Game

namespace PSM {
struct Creature;
struct SeSound;
} // namespace PSM

void PSChangeBgm_ChallengeGame();

void PSPause_StartMenuOn();
void PSPause_StartMenuOff();

void PSPauseOn(u8, u8);
void PSPauseOff();

void PSStart2DStream(u32);
void PSStop2DStream();

void PSStartChallengeTimeUpStream();
JAISound* PSStartEnemyDownSmokeSE(Game::EnemyBase*, f32);
JAISound* PSStartEnemyDownWatSE(Game::EnemyBase*, f32);
PC_SWITCH(struct JAISound*, void) PSStartEnemyFatalHitSE(Game::EnemyBase*, f32); // PC: the definition's return type (MSVC mangles it)
JAISound* PSStartEnemyGhostSE(Game::EnemyBase*, f32);
PSM::SeSound* PSStartSoundVec(u32, Vec*);
PC_SWITCH(struct JAISe*, void) PSStartTreasureLaderSE(f32);
PC_SWITCH(struct JAISe*, void) PSStartTresureLaderNoiseSE(u8, f32, f32);
void PSMuteSE_on2D();
void PSMuteOffSE_on2D();

PC_SWITCH(void, unknown) PSPlayCaveHoleSound(PSM::Creature*);

#endif
