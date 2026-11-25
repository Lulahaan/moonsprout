// Online harness, M5.1a (docs/ONLINE_MULTIPLAYER.md 2.3, 4 item 2): direct boot into 2P Battle; M5.3b:
// 2P Challenge (--ch-stage, a version 5 replay, an online session) and the online session's stage,
// seats and handicaps (docs/online/M5_3B_LOCKSTEP.md "Session start").
// Host-only file, built into the game library (not in the matching build).
//
// The first section cannot be VsGameSection: Title::Section::loadResident mounts pikis.szs and
// onyon/arc.szs and loads the ARAM set, and Title::Section::run loads the card options. So a
// --vs-stage run goes boot, title section, forced exit to SN_VSGame, VsGameSection, TitleState,
// and here the VS config is filled the way the stage select fills it on confirm.
#include "Game/VsGame.h"
#include "Game/VsGameSection.h"
#include "Game/ChallengeGame.h"
#include "Game/GameSystem.h"
#include "Game/gamePlayData.h"
#include "Dolphin/os.h"
#include "p2_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// Normal title frames before the hook leaves the title (half a second at 60 Hz).
const int kTitleFramesBeforeExit = 30;
// TMainTitleMgr::Select_Vs and Select_Challenge.
const int kSelectVs        = 2;
const int kSelectChallenge = 1;

int sTitleCalls  = 0;
bool sTitleLeft  = false;
uint32_t sMatchIndex = 0;

// A direct boot is wanted: a VS stage or a Challenge course (harness flag, replay header or START).
bool boot_wanted() { return p2_net_vs_stage() >= 0 || p2_net_ch_stage() >= 0; }

bool title_exit_due()
{
	return boot_wanted() && !sTitleLeft && sTitleCalls >= kTitleFramesBeforeExit;
}

// MPPM_VS_HANDICAP: 1..10, else 0 (off). Prints one line the first time it is on.
int vs_handicap_aid()
{
	static int sValue = -1;
	if (sValue < 0) {
		sValue          = 0;
		const char* env = getenv("MPPM_VS_HANDICAP");
		if (env && *env) {
			char* end = nullptr;
			long v    = strtol(env, &end, 10);
			if (end && *end == '\0' && v >= 1 && v <= 10) {
				sValue = (int)v;
				printf("mppm: net: perf aid: VS handicap %d (checksums differ from a run without it)\n", sValue);
				fflush(stdout);
			}
		}
	}
	return sValue;
}

} // namespace

extern "C" int p2_net_vs_title_pending(void)
{
	if (!boot_wanted() || sTitleLeft) {
		return 0;
	}
	sTitleCalls++;
	return title_exit_due() ? 1 : 0;
}

extern "C" int p2_net_vs_title_select(int stock)
{
	if (!title_exit_due()) {
		return stock;
	}
	sTitleLeft = true;
	return p2_net_ch_stage() >= 0 ? kSelectChallenge : kSelectVs;
}

extern "C" int p2_net_vs_inject(void* sectionPtr, void* statePtr)
{
	if (p2_net_vs_stage() < 0) {
		return 0;
	}
	Game::VsGameSection* section     = (Game::VsGameSection*)sectionPtr;
	Game::VsGame::TitleState* state = (Game::VsGame::TitleState*)statePtr;
	if (section == nullptr || state == nullptr || section->mVsStageList == nullptr) {
		p2_net_abort_run("--vs-stage: no VS stage list");
		return 0;
	}
	int count = section->mVsStageList->mStageData.getChildCount();
	if (count <= 0) {
		p2_net_abort_run("--vs-stage: the VS stage list is empty");
		return 0;
	}

	Game::gameSystem->mMode = Game::GSM_VERSUS_MODE;

	int stage                     = p2_net_vs_stage() % count;
	Game::VsGame::StageData* data = section->mVsStageList->getStageData(stage);
	if (data == nullptr) {
		p2_net_abort_run("--vs-stage: no data for the stage");
		return 0;
	}
	section->mVsStageData = data;
	section->mVsStageNum  = stage;

	strcpy(section->mCaveInfoFilename, data->mCaveInfoFilename);
	strcpy(section->mEditFilename, data->mStageLayoutFilePath);

	section->mContainer1 = data->mPikiContainer;

	Game::playData->setDopeCount(0, data->mStartNumSpicy);
	Game::playData->setDopeCount(1, data->mStartNumBitter);

	// M5.3b: the handicaps of the online session or of a version 5 replay (0 = stock) win; offline
	// without them the M5.3a perf aid MPPM_VS_HANDICAP=1..10 sets both Pikmin handicaps (10 = 50 + 50
	// Pikmin). Unset (or any other value) leaves the stock handicaps alone.
	const int h0 = p2_net_vs_handicap(0), h1 = p2_net_vs_handicap(1);
	if (h0 > 0 || h1 > 0) {
		if (h0 > 0) section->mOlimarHandicap = h0;
		if (h1 > 0) section->mLouieHandicap = h1;
	} else if (!p2_net_online()) {
		int handicap = vs_handicap_aid();
		if (handicap > 0) {
			section->mOlimarHandicap = handicap;
			section->mLouieHandicap  = handicap;
		}
	}

	section->mTimeLimit  = 0.999f + data->mTimeLimit;
	section->mEditNumber = -2; // what the stage select sets with no C-stick input

	// The start barrier, before anything of the match is built. A no-op on later matches.
	p2_net_session_begin();
	// M5.1b: one match per injection; match 0 keeps the session seed, later ones reseed.
	p2_net_floor_begin(sMatchIndex++);

	Game::VsGame::LoadArg load(0, 0, 0);
	load.mGameLoadType = 0;
	state->transit(section, Game::VsGame::VGS_Load, &load);
	return 1;
}

// M5.3b: TitleState::execChallenge once its DVD load has completed (vsGS_Title.cpp, the only src/ edit
// of M5.3b). With a 2P Challenge course (--ch-stage, a version 5 replay, an online session) it does what
// the CHECK2D_ChallengeSelect_ExitFinished branch does for a 2-player run, starts the session and
// moves to VGS_Load; otherwise it returns 0 and the stock course select runs.
extern "C" int p2_net_ch_inject(void* sectionPtr, void* statePtr)
{
	if (p2_net_ch_stage() < 0) {
		return 0;
	}
	Game::VsGameSection* section     = (Game::VsGameSection*)sectionPtr;
	Game::VsGame::TitleState* state = (Game::VsGame::TitleState*)statePtr;
	if (section == nullptr || state == nullptr || section->mChallengeStageList == nullptr) {
		p2_net_abort_run("--ch-stage: no Challenge course list");
		return 0;
	}
	int count = section->mChallengeStageList->mStageData.getChildCount();
	if (count <= 0) {
		p2_net_abort_run("--ch-stage: the Challenge course list is empty");
		return 0;
	}

	Game::gameSystem->mMode = Game::GSM_TWO_PLAYER_CHALLENGE;

	int course                           = p2_net_ch_stage() % count;
	Game::ChallengeGame::StageData* data = section->mChallengeStageList->getStageData(course);
	if (data == nullptr) {
		p2_net_abort_run("--ch-stage: no data for the course");
		return 0;
	}
	section->mChallengeStageData = data;
	section->mChallengeStageNum  = course;

	strcpy(section->mCaveInfoFilename, data->mCaveInfoFilename);

	section->mContainer1 = data->mPikiContainer;

	Game::playData->setDopeCount(0, data->mStartNumSpicy);
	Game::playData->setDopeCount(1, data->mStartNumBitter);

	section->mTimeLimit = 0.0f;

	// The start barrier, before anything of the course is built.
	p2_net_session_begin();
	p2_net_floor_begin(sMatchIndex++);

	Game::VsGame::LoadArg load(0, 0, 0);
	load.mGameLoadType = 0;
	state->transit(section, Game::VsGame::VGS_Load, &load);
	return 1;
}
