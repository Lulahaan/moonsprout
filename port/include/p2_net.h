/* Online harness, milestones M5.1a and M5.1b (docs/ONLINE_MULTIPLAYER.md 2.1, 2.3, 2.4, 2.5, 2.6, 2.7,
 * 2.10, 4, 8), M5.2 (content) and M5.3a (N captains, docs/online/M5_3A_NPLAYER.md).
 *
 * One C interface for the input seam, the session seed, input-log record and replay, the
 * per-30-tick checksum, the headless tick runner, the direct VS boot and the latency probe.
 * Game code reaches it through p2_host.h (types.h), always inside PC_ONLY / PC_SWITCH on an
 * existing source line. With no flag and no environment variable set, every function here is a
 * no-op (or returns 0 / -1 / its argument): offline behaviour is unchanged.
 *
 * Flags (each also has an environment variable; the flag wins). Both "--flag value" and
 * "--flag=value" are accepted. The disc image stays the first argument.
 *   --seed N               MPPM_SEED               session seed (u32) for the MSL LCG
 *   --record FILE          MPPM_RECORD             write the input log
 *   --replay FILE          MPPM_REPLAY             feed the input log through the seam
 *   --checksum-log FILE    MPPM_CHECKSUM_LOG       write one checksum line every 30 ticks
 *   --checksum-verify FILE MPPM_CHECKSUM_VERIFY    compare against a checksum log; exit code 3 on mismatch
 *   --vs-stage N           MPPM_VS_STAGE           enter 2P Battle stage N (0..254) with no menu; the session starts there
 *   --headless-ticks       MPPM_HEADLESS_TICKS=1   every tick is due at once, nothing is presented
 *   --exit-after-ticks N   MPPM_EXIT_AFTER_TICKS   quit after N ticks
 *   --latency-probe        MPPM_LATENCY_PROBE=1    software press-to-present probe
 *   --tick-order update|draw  MPPM_TICK_ORDER      update-before-draw (default) or the stock draw-before-update (M5.1b)
 *   --sim-k X              MPPM_SIM_K              session-constant widescreen factor of the sim frustum, 1.0 to 3.6 (default 3.6)
 *   --vs-players N         MPPM_VS_PLAYERS         M5.3a: captains of a --vs-stage session, 2 (default) or 4; anything else exits 2
 *   --view-captain K       MPPM_VIEW_CAPTAIN       M5.3a: full-screen view of captain K (0..3), presentation only, never recorded
 *
 * Terms.
 *   harness  any of --record, --replay, --checksum-log, --checksum-verify, --vs-stage is given.
 *   session  the deterministic window. It starts at the VS start barrier when --vs-stage is given
 *            (p2_net_session_begin from p2_net_vs_inject), otherwise at the first pad read after
 *            boot. At the start the MSL LCG is seeded with the session seed and the tick is 0.
 *   tick     one JUTGamePad::read() from System::beginFrame inside the session and outside a
 *            nonsim region (M5.1b: the loading loops, see p2_net_nonsim_begin). p2_net_tick() is
 *            the number of such reads the session has consumed, so it is 1 during the first pass.
 *   nonsim   a loop that only waits for a load (plan 2.3 step 5). Its pad reads are not ticks: they
 *            consume no input record and every slot reads released, so their number may differ
 *            between machines.
 *
 * Ownership: "seam" = port/net/net.cpp and port/sdk/msl_compat.c, "gameside" = port/game/net/,
 * "host" = port/main.cpp and port/net/probe.cpp, "launcher" = port/launcher/.
 */
#ifndef P2_NET_H
#define P2_NET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PADStatus;

/* ---- seam: port/net/net.cpp (host side, no game headers) ------------------------------------ */

/* main(), before the launcher's preinit: reads the flags and the environment, opens the replay
 * log and reads its header (so the seed is known before System::construct). */
void p2_net_init(int argc, char** argv);
/* Flushes and closes the logs, writes the checksum log's end line. Idempotent. Call before any
 * exit path (window close, --exit-after-ticks, end of the replay log). */
void p2_net_shutdown(void);

/* 1 when the harness is on (see Terms), from p2_net_init on, whether or not the session has
 * started; M5.1b: also 1 while an online session runs (p2_net_set_online). */
int p2_net_active(void);
/* 1 from the session start until shutdown or p2_net_crash_stop. */
int p2_net_session_active(void);
/* Start barrier. Seeds the MSL LCG with the session seed (replay header, else --seed, else a
 * clock value that is then written to the record header), sets the tick to 0, opens the record
 * and checksum logs. Does nothing when the harness is off or the session already runs. */
void p2_net_session_begin(void);
/* Session ticks consumed so far (0 before the session and before its first read). */
uint32_t p2_net_tick(void);

/* The input seam: JUTGamePad::read(), after PADRead and the clamp. Offline: returns at once.
 * In a session: replay overwrites all four slots (err included), record appends what the game is
 * about to see, and the tick advances. Before the start barrier of a --vs-stage run, every slot is
 * released (err kept) so tick 0 inherits the same button state in record and replay. Skipped
 * after p2_net_crash_stop. */
void p2_net_exchange(struct PADStatus* status);
/* System::construct: the seed for srand. Returns the session seed when one is known at boot
 * (--seed / MPPM_SEED, or a replay header), otherwise clockSeed unchanged. */
unsigned p2_net_boot_seed(unsigned clockSeed);
/* 1 while the wall-clock pad logic must stay off (reset combo, long-press, the launcher's Reset
 * Game): same value as p2_net_active(). */
int p2_net_pad_wallclock_off(void);
/* JUTException::readPad: the crash screen owns the pads from here on. Closes the logs and turns
 * the seam into a no-op for the rest of the run; a headless run exits 5 here. */
void p2_net_crash_stop(void);
/* A harness run that cannot reach its start barrier (the --vs-stage inject found no stage data):
 * prints why and exits 2, instead of running on with released pads and never ending. */
void p2_net_abort_run(const char* why);

/* 1 with --headless-ticks. */
int p2_net_headless(void);
/* 1 with --latency-probe. */
int p2_net_latency_probe(void);
/* The 2P Battle stage index of --vs-stage, or of the replay header; -1 when none. */
int p2_net_vs_stage(void);
/* 1 once the run should end: --exit-after-ticks reached (session ticks with the harness on, pad
 * reads since boot otherwise), the replay log is used up, or --checksum-verify found a mismatch.
 * Polled by the host at the end of p2_host_present. */
int p2_net_should_exit(void);
/* Process exit code for that end: 0, 3 after a checksum mismatch, 4 when a write to the record or
 * checksum log failed (the log is short), 6 (M5.1b) with MPPM_RNG_STRICT=1 when another thread drew
 * from the OTHER stream during the session. Read it after p2_net_shutdown,
 * which writes and verifies the last L line and the E line. (Start-up failures exit 2 from
 * p2_net_init: bad replay log, output log not creatable, verify log not openable.) */
int p2_net_exit_code(void);

/* The MSL LCG state (port/sdk/msl_compat.c). */
unsigned int p2_msl_rand_state(void);

/* Checksum (2.7). The game side asks whether this tick wants one, hashes each subsystem and hands
 * the result over; the seam writes the log line and runs the verify. */
typedef struct P2NetChecksum {
	uint32_t rng;    /* p2_msl_rand_state() at the end of doUpdate */
	uint32_t navi;   /* captains: position, health, state */
	uint32_t piki;   /* Pikmin: position, colour, flower stage, state */
	uint32_t enemy;  /* enemies: position, health, type */
	uint32_t pellet; /* pellets and treasures: position, kind */
	uint32_t timers; /* gameSystem timers and flags, TimeMgr; M5.1b: the sim-relevant gP2Config fields */
	uint32_t mode;   /* mode state: VS scores, marbles, sprays, time limit, lose causes */
	uint32_t camera; /* M5.1b: both play cameras: view matrix, view angle */
	uint16_t naviCount, pikiCount, enemyCount, pelletCount; /* objects hashed */
} P2NetChecksum;
/* 1 when a session runs, a checksum log or verify is open, and p2_net_tick() is a multiple of the
 * interval (30; MPPM_CHECKSUM_INTERVAL overrides, 1 = every tick). */
int p2_net_checksum_due(void);
void p2_net_checksum_submit(const P2NetChecksum* sum);

/* ---- gameside: port/game/net/ (built like game code) ---------------------------------------- */

/* End of BaseGameSection::doUpdate: computes and submits the checksum when one is due. */
void p2_net_sim_tick_end(void);
/* DvdThread::sendCommand, after the command is queued. In a session, on the main game thread:
 * returns only when the DVD thread has finished the command, so a load never spans ticks.
 * `cmd` is a DvdThreadCommand*. */
void p2_net_dvd_sync(void* cmd);
/* Title::Section::doUpdateMainTitle. pending: 1 while a --vs-stage boot still has to leave the
 * title screen. select: returns TMainTitleMgr::Select_Vs (2) once for such a boot and clears the
 * pending state, otherwise `stock`. */
int p2_net_vs_title_pending(void);
int p2_net_vs_title_select(int stock);
/* VsGame::TitleState::execVs, once its DVD load has completed. With --vs-stage: fills the VS
 * config the way the stage select does on confirm, calls p2_net_session_begin, moves to VGS_Load
 * and returns 1. Otherwise returns 0. `section` is a Game::VsGameSection*, `titleState` a
 * Game::VsGame::TitleState*. */
int p2_net_vs_inject(void* section, void* titleState);

/* ==== M5.1b ==================================================================================== */

/* ---- seam: port/net/net.cpp ------------------------------------------------------------------ */

/* 1 when the harness is on OR an online session runs (p2_net_set_online). Everything that must
 * be off online keys on this one function: hold_game, Smooth Motion, the wall-clock pad logic,
 * the settings lock. (Declared above; M5.1b only widens its meaning.) */
/* M5.3b: the lobby calls this with 1 at the start barrier and 0 when the session ends. */
void p2_net_set_online(int on);

/* Tick order of BaseGameSection passes (plan 2.10 item 4). A session constant: the replay
 * header's value, else --tick-order / MPPM_TICK_ORDER, else update-first. A --tick-order given
 * with a replay overrides the header (the A/B test) and prints one notice. Without the harness:
 * MPPM_TICK_ORDER, else update-first. */
enum { P2_TICK_ORDER_DRAW_FIRST = 0, P2_TICK_ORDER_UPDATE_FIRST = 1 };
int p2_net_tick_order(void);

/* The widescreen factor k the SIM frustum uses (plan 2.5, section 4 item 5). While
 * p2_net_active(): the session constant (--sim-k / MPPM_SIM_K when given, which overrides a replay
 * header like --tick-order does, else the replay header's value when not 0, else 3.6, the widest
 * k the renderer supports), whatever the window is. Otherwise windowK unchanged, so offline play
 * culls exactly as before. */
float p2_net_sim_k(float windowK);

/* The launcher reports the hash of the Synced settings it locked (net_synced_hash); the seam
 * writes it to the record header (0 = not recorded). */
void p2_net_set_synced_hash(uint32_t hash);

/* Nonsim regions (load barrier). enter/leave nest. While the depth is above 0 in a session,
 * p2_net_exchange releases every slot (err kept), consumes no record, writes none and does not
 * advance the tick. Game code calls the gameside wrappers below, not these. */
void p2_net_nonsim_enter(void);
void p2_net_nonsim_leave(void);
int p2_net_nonsim(void); /* current depth */

/* Start of a floor, round or match inside a session (plan 2.3: "reseed each floor"). index 0 is
 * the first one and changes nothing (the session start seeded it); index n > 0 sets the sim
 * stream to p2_rng_mix(sessionSeed + 0x9E3779B9u * n). No-op outside a session. */
void p2_net_floor_begin(uint32_t index);

/* Test events carried by synthetic logs (byte 11 of port 0's slot, see the harness doc): the code
 * of the tick the session is in, 0 when none. 1 = captain 0 takes lethal damage, 2 = captain 1;
 * M5.3a: 3 = captain 2, 4 = captain 3 (4-captain logs only). */
int p2_net_debug_event(void);

/* ---- RNG streams: port/sdk/msl_compat.c (section 4 item 3) ------------------------------------ */

/* rand() (p2_msl_rand) draws from one of three MSL LCG streams:
 *   SIM    the main game thread outside every presentation scope and outside Section::draw, and
 *          any thread while a DVD hand-off is on. The only stream in the checksum.
 *   PRES   the main game thread inside a presentation scope or inside Section::draw.
 *   OTHER  every other thread (the DVD thread outside a hand-off, interrupt handlers on the
 *          timer thread). Guarded by a lock.
 * The main game thread is the thread of the first p2_msl_srand call (System::construct).
 * p2_msl_srand(seed) seeds all three: SIM = seed, PRES = p2_rng_mix(seed ^ 0x50524553),
 * OTHER = p2_rng_mix(seed ^ 0x4F544852). MPPM_RNG_PERTURB="pres=<u32>,other=<u32>" XORs the given
 * values into those two seeds (a test: the checksums must not change). */
enum { P2_RNG_SIM = 0, P2_RNG_PRES = 1, P2_RNG_OTHER = 2 };
uint32_t p2_rng_mix(uint32_t x); /* lowbias32: x ^= x>>16; x *= 0x7feb352d; x ^= x>>15; x *= 0x846ca68b; x ^= x>>16 */
unsigned int p2_rng_state(int stream);
void p2_rng_set_sim(unsigned int state); /* p2_net_floor_begin */
/* Presentation scope (HUD, menus, the launcher scene, 2D particles). Nests. Main thread only. */
void p2_rng_pres_begin(void);
void p2_rng_pres_end(void);
/* Section::draw. Nests. A rand() here outside a presentation scope still goes to PRES and is
 * counted as "draw-unflagged" (the sim-stream assert of plan 2.7: it would have been a sim draw). */
void p2_rng_draw_begin(void);
void p2_rng_draw_end(void);
/* DVD hand-off (p2_net_dvd_sync): the main thread is blocked until the DVD thread has finished
 * one command, so that thread's rand() calls are serialised with the sim and use SIM. */
void p2_rng_handoff(int on);
/* The seam turns the audit on for the session. Counters since then; with MPPM_RNG_AUDIT=1 the
 * first 32 distinct return addresses of each audited kind are printed at shutdown. */
void p2_rng_session(int on);
typedef struct P2RngStats {
	uint64_t sim;           /* SIM draws on the main thread */
	uint64_t simHandoff;    /* SIM draws by another thread during a hand-off */
	uint64_t pres;          /* PRES draws inside a presentation scope */
	uint64_t drawUnflagged; /* PRES draws inside Section::draw but outside every presentation scope */
	uint64_t other;         /* OTHER draws */
	uint64_t otherInSession;/* OTHER draws while a session runs: a hazard, the value may feed the sim */
} P2RngStats;
void p2_rng_stats(P2RngStats* out);

/* ---- gameside: port/game/net/tick.cpp --------------------------------------------------------- */

/* Around the real loading loops (same-line edits in section.cpp and baseGameSection.cpp). begin
 * is p2_net_nonsim_enter. end: in a session, at depth 1, it first runs one JUTGamePad::read()
 * (released, still nonsim) so the button state the next tick starts from never depends on how
 * many passes the loop ran, then p2_net_nonsim_leave. */
void p2_net_nonsim_begin(void);
void p2_net_nonsim_end(void);
/* First statement of BaseGameSection::doUpdate. Applies p2_net_debug_event(), then steps the
 * cameras (cameraMgr->update(), with the controller lock while the game is paused, exactly as
 * doDraw did) and copies each stepped view into mCurViewMatrix. Not in Piklopedia mode.
 * `section` is a Game::BaseGameSection*. */
void p2_net_sim_tick_begin(void* section);
/* Tick order. pass_begin: Section::main, with p2_host_arm_tick (`section` is a Section*).
 * pass_after_wait: JFWDisplay::beginRender, after the pacing wait; when p2_host_update_first()
 * and the armed section is the running BaseGameSection, runs its update() there.
 * pass_updated: 1 once when that happened; Section::main then skips its own update().
 * pass_ran_update_first: 1 when that happened in this pass (not consumed; cleared by pass_begin).
 * p2_host_update_first() only says the order was wanted: sections that are not the running
 * BaseGameSection (title, menus) still run update after draw. */
void p2_net_pass_begin(void* section);
void p2_net_pass_after_wait(void);
int p2_net_pass_updated(void);
int p2_net_pass_ran_update_first(void);

/* ---- host: port/main.cpp ---------------------------------------------------------------------- */

/* 1 when this armed pass wants update before draw: p2_net_tick_order() is update-first and Smooth
 * Motion is not in effect (an interpolated picture is already one tick behind, and draw-first
 * makes the tick's own picture its alpha = 1 end point). Evaluated and latched on its first
 * call of the pass; p2_host_present uses the latched value. */
int p2_host_update_first(void);

/* ==== M5.2 (data-mod overlay, docs/online/M5_2_MODS.md) ======================================== */

/* ---- seam: port/net/net.cpp ------------------------------------------------------------------ */

/* The build id: FNV-1a 64 of the executable file (0 when it cannot be read), the value the record
 * header stores at offset 16. Computed once; thread-safe (the launcher asks from a worker thread
 * for the content hash). */
uint64_t p2_net_build_id(void);

/* Called once by p2_mods_mount (always, also for stock content), after the disc is open and before
 * the game starts. modsHash: 32 bytes, all zero for stock content. list: "id@version,..." in load
 * order, "" for stock (copied).
 * - A loaded replay whose header is not synthetic (flags bit 0) and whose mods hash differs (a
 *   version 2 log counts as stock) is refused here: one line names both mod lists and the process
 *   exits 2. A synthetic log is not checked; with mods on, one notice line says so.
 * - The record header (version 3) stores the mods hash, p2_mods_content_hash() and the list. The
 *   checksum log gets "# mods=<64 hex> <list>" after "# build=" when the content is not stock.
 * - Before it is called (or if it never is), the content is stock. */
void p2_net_set_content(const uint8_t modsHash[32], const char* list);

/* ==== M5.3a ==================================================================================== */

/* ---- seam: port/net/net.cpp ------------------------------------------------------------------ */

/* Captains of a 2P Battle harness session: 2 (stock) or 4. --vs-players N / MPPM_VS_PLAYERS; a version 4
 * replay header wins (a different flag value prints one warning). 2 when there is no VS stage. */
int p2_net_vs_players(void);
/* Presentation only, never recorded: -1 = stock split of captains 0 and 1; 0..3 = full-screen view of
 * that captain (--view-captain K / MPPM_VIEW_CAPTAIN). */
int p2_net_view_captain(void);
/* MPPM_PERF=1: begin/end of BaseGameSection::doUpdate; summary line at session end. */
void p2_net_perf_sim_begin(void);
void p2_net_perf_sim_end(void);
/* More on these: with more than 2 captains the record header is version 4 (version 3 plus byte 33 =
 * captains), the checksum log gets "# players=<N>" after "# build=" (and "# mods="), and the session
 * start line " players=<N>"; with 1-2 captains every output is exactly as before. The perf summary
 * (p2_net_shutdown, MPPM_PERF=1, session ticks only, steady clock) is
 * "mppm: net: perf ticks=<n> sim avg=<ms> p50=<ms> p99=<ms> max=<ms> ms, pass avg=<ms> ms", pass =
 * from one sim begin to the next, passes that held a load barrier left out. */

/* ==== M5.3b (online sessions; docs/online/M5_3B_LOCKSTEP.md) ===================================== */
/* An online session is a harness session whose input comes from the network: tick t is the t-th
 * session read, bundle t is what every peer's game reads at tick t (all four ports), and the host's
 * net thread assembles the bundles from each seat's input (port/net/session/). With no online flag
 * and no online UI action none of this runs: every function below returns its offline value.
 * Online flags (port/net/session/session.cpp, docs/online/M5_3B_LOCKSTEP.md "Flags"): --server,
 * --host, --join CODE, --spectate, --name, --password, --mode, --stage, --handicap, --slot, --ready,
 * --expect, --code-file, --net-input, --net-impair, --net-mode, --net-bind, --net-allow-plaintext,
 * --net-test-bots N; harness: --ch-stage N (MPPM_CH_STAGE). */
/* ---- seam: port/net/net.cpp ---- */
int p2_net_online(void);                 /* 1 from START applied until the session ends (p2_net_active() is 1 too) */
int p2_net_local_slot(void);             /* this machine's seat 0..3 in an online session; -1 offline or spectator */
int p2_net_ch_stage(void);               /* 2P Challenge course of --ch-stage, a version 5 replay or the session; -1 none */
int p2_net_vs_handicap(int team);        /* VS handicap of the session or replay for team 0/1; 0 = stock */
int p2_net_tick_ready(uint32_t tick);    /* online: bundle `tick` present and no unreleased barrier before it; offline 1 */
void p2_net_game_poll(void);             /* host, once per pass on the game thread: applies START/END, notices */
void p2_net_build_hash(uint8_t out[32]); /* D19; computed once, thread-safe */
/* ---- gameside: port/game/net/vs_boot.cpp ---- */
int p2_net_ch_inject(void* section, void* titleState); /* TitleState::execChallenge once its DVD load completed */
/* ---- host: port/main.cpp ---- */
int p2_host_net_hold(int (*ready)(void*), void* arg);  /* hold loop until ready(arg) or quit; 1 ready, 0 quit */
long long p2_host_next_tick_due_ns(void); /* steady ns of the retrace the next armed tick is due at; 0 unknown */
void p2_host_align_tick(uint32_t count);  /* barrier: the next armed tick is due at retrace `count` at the earliest */
void p2_host_tick_shift(int ticks);       /* any thread: move the online tick grid `ticks` ticks earlier (< 0: later) */
/* ---- port/sdk/os_thread.cpp ---- */
void p2_vi_phase_slew_set(long long remainingNs); /* remaining grid correction, applied +-1 ms per retrace */
long long p2_vi_phase_slew_get(void);
uint32_t p2_vi_grid_jump(long long localNs);      /* the next retrace fires at localNs (now if past); clears
                                                     the slew; returns the count that retrace will have */
long long p2_vi_retrace_time_ns(unsigned count);  /* (fps_design 5.4) steady ns of retrace `count` on the grid */
/* Existing functions that change meaning online: p2_net_active() (1 with p2_net_set_online),
 * p2_net_vs_stage() (the session's stage for Battle modes), p2_net_vs_players() (2 or 4 seats),
 * p2_net_view_captain() (the local seat with 4 seats, unless --view-captain), p2_net_tick_order() and
 * p2_net_sim_k() (the SessionConfig values), p2_net_checksum_due() (every 30 ticks online even without
 * a log), p2_net_should_exit()/p2_net_exit_code() (0 done, 2 flags, 7 desync, 8 session ended, 9
 * lobby failure). Version 5 .mpnl logs (start mode 2 = Challenge, handicaps at bytes 34/35, flags bit
 * 1 = online) are written only for Challenge or non-stock handicaps. */

#ifdef __cplusplus
}
/* Scope guards for the same-line edits: `{ PC_ONLY(P2RngPres p2RngPres;)`. */
struct P2RngPres {
	P2RngPres() { p2_rng_pres_begin(); }
	~P2RngPres() { p2_rng_pres_end(); }
};
struct P2RngDraw {
	P2RngDraw() { p2_rng_draw_begin(); }
	~P2RngDraw() { p2_rng_draw_end(); }
};
#endif

#endif /* P2_NET_H */
