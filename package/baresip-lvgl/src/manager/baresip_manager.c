#define _GNU_SOURCE 1
#define _DEFAULT_SOURCE 1
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "baresip_manager.h"
// Moved rem_vid.h after baresip_manager.h to ensure dependencies (re.h) are
// satisfied
// satisfied
#include "lvgl.h"
#include <rem_vid.h>
#include <rem_vidconv.h>
// #include <SDL.h> // Removed
#include "../applets/call_applet.h"
#include "applet_manager.h"
#include "config_manager.h"
#include "database_manager.h"

// Global LVGL Lock from Main
extern pthread_mutex_t g_lvgl_lock;
#include "history_manager.h"
#include "logger.h"
// Includes cleaned

struct message *uag_message(void);
struct ua *uag_current(void);

#include <net/if.h>
#include <pthread.h>
#include <re_dbg.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
// Helper to check if string is empty
// str_isset is provided by re_fmt.h from re.h

extern const struct mod_export exports_g711;
extern const struct mod_export exports_opus;
extern const struct mod_export exports_account;
extern const struct mod_export exports_stun;
extern const struct mod_export exports_turn;
extern const struct mod_export exports_ice;
#ifdef __APPLE__
extern const struct mod_export exports_audiounit;
extern const struct mod_export exports_coreaudio;
#elif defined(__linux__)
extern const struct mod_export exports_alsa;
extern const struct mod_export exports_v4l2;
extern const struct mod_export exports_avcodec;
extern const struct mod_export exports_selfview;
extern const struct mod_export exports_swscale;
#endif

#define MAX_ACCOUNTS 10

// Account registration status tracking
typedef struct {
	char aor[256];
	reg_status_t status;
} account_status_t;

// MAX_CALLS defined in header

// Video cleanup queue item - Manual queue instead of lv_async_call to avoid
// freezes
typedef struct {
	struct le le;
	bool is_local;
	const void *old_src;
	uint64_t id;
} video_cleanup_item_t;

static struct list video_cleanup_list; // Protected by vidisp_list_lock

typedef struct {
	struct call *call;
	char peer_uri[256];
	enum call_state state;
	bool is_held;
} active_call_t;

// Video Display Module Pointers
static struct vidisp *vid = NULL; // Remote (sdl_vidisp)
static struct vidisp *vid2 = NULL; // Local (window)

// baresip_manager_register_vidisp declaration removed

// Global state
static struct {
	enum call_state state;
	char peer_uri[256];
	bool muted;
	call_event_cb callback;
	reg_event_cb reg_callback;
	struct call *current_call;
	active_call_t active_calls[MAX_CALLS];
	account_status_t accounts[MAX_ACCOUNTS];
	int account_count;
	bool is_video_call;
} g_call_state = { .state = CALL_STATE_IDLE,
		   .peer_uri = "",
		   .muted = false,
		   .callback = NULL,
		   .reg_callback = NULL,
		   .current_call = NULL,
		   .account_count = 0,
		   .is_video_call = false };

static message_event_cb g_message_callback = NULL;

#define MAX_LISTENERS 4
static struct {
	call_event_cb listeners[MAX_LISTENERS];
	int count;
} g_listener_mgr = { 0 };

void baresip_manager_add_listener(call_event_cb cb)
{
	if (g_listener_mgr.count < MAX_LISTENERS) {
		g_listener_mgr.listeners[g_listener_mgr.count++] = cb;
		log_info("BaresipManager", "Added listener %p (Total: %d)", (void *)cb, g_listener_mgr.count);
	} else {
		log_warn("BaresipManager", "Listener list full!");
	}
}

// Command Queue for Thread Safety
typedef enum { CMD_NONE = 0, CMD_ADD_ACCOUNT, CMD_SEND_MESSAGE, CMD_HANGUP, CMD_DIAL } cmd_type_t;

typedef struct {
	cmd_type_t type;
	union {
		voip_account_t acc;
		struct {
			char peer[256];
			char text[512];
		} msg;
		struct {
			char uri[256];
			char account_aor[256];
			bool video;
		} dial;
	} data;
} cmd_t;

#define CMD_QUEUE_SIZE 10
static struct {
	cmd_t items[CMD_QUEUE_SIZE];
	int head;
	int tail;
	int count;
} g_cmd_queue = { 0 };

static pthread_mutex_t g_cmd_mutex = PTHREAD_MUTEX_INITIALIZER;

// Queue Helper
static bool cmd_enqueue(const cmd_t *cmd)
{
	bool ret = false;
	pthread_mutex_lock(&g_cmd_mutex);
	if (g_cmd_queue.count < CMD_QUEUE_SIZE) {
		g_cmd_queue.items[g_cmd_queue.tail] = *cmd;
		g_cmd_queue.tail = (g_cmd_queue.tail + 1) % CMD_QUEUE_SIZE;
		g_cmd_queue.count++;
		ret = true;
	}
	pthread_mutex_unlock(&g_cmd_mutex);
	return ret;
}

// Watchdog
static struct tmr watchdog_tmr;
static void check_call_watchdog(void *arg);

// Messaging Subsystem
static struct message *g_message = NULL;

// Forward declaration
static void safe_strncpy(char *dest, const char *src, size_t size)
{
	if (size == 0 || !dest)
		return;
	if (!src) {
		dest[0] = '\0';
		return;
	}
	size_t len = strlen(src);
	if (len >= size)
		len = size - 1;
	memcpy(dest, src, len);
	dest[len] = '\0';
}

static int internal_add_account(const voip_account_t *acc);
static int internal_send_message(const char *peer, const char *text);

static void add_or_update_call(struct call *call, enum call_state state, const char *peer)
{
	if (!call)
		return;

	// Check if exists
	for (int i = 0; i < MAX_CALLS; i++) {
		if (g_call_state.active_calls[i].call == call) {
			// Update
			g_call_state.active_calls[i].state = state;
			if (peer) {
				safe_strncpy(g_call_state.active_calls[i].peer_uri, peer, 256);
			}
			// log_info("BaresipManager", "Updated call %p in slot %d", call, i);
			return;
		}
	}

	// Add new
	int free_slot = -1;

	// 1. Try to find a truly empty slot
	for (int i = 0; i < MAX_CALLS; i++) {
		if (g_call_state.active_calls[i].call == NULL) {
			free_slot = i;
			break;
		}
	}

	// 2. If no empty slot, find a "dead" slot (IDLE/TERMINATED) to recycle
	if (free_slot == -1) {
		for (int i = 0; i < MAX_CALLS; i++) {
			enum call_state s = g_call_state.active_calls[i].state;
			if (s == CALL_STATE_IDLE || s == CALL_STATE_TERMINATED || s == CALL_STATE_UNKNOWN) {
				log_warn("BaresipManager", "Recycling dead slot %d (State=%d, Call=%p)", i, s,
					 g_call_state.active_calls[i].call);
				free_slot = i;
				break;
			}
		}
	}

	if (free_slot == -1) {
		log_warn("BaresipManager", "CRITICAL: Max calls reached! Dumping slots:");
		for (int i = 0; i < MAX_CALLS; i++) {
			log_warn("BaresipManager", "  Slot %d: State=%d | Call=%p | Peer='%s'", i,
				 g_call_state.active_calls[i].state, g_call_state.active_calls[i].call,
				 g_call_state.active_calls[i].peer_uri);
		}
	}

	if (free_slot != -1) {
		g_call_state.active_calls[free_slot].call = call;
		g_call_state.active_calls[free_slot].state = state;
		g_call_state.active_calls[free_slot].is_held = call_is_onhold(call);
		if (peer) {
			safe_strncpy(g_call_state.active_calls[free_slot].peer_uri, peer, 256);
		} else {
			g_call_state.active_calls[free_slot].peer_uri[0] = '\0';
		}
		log_info("BaresipManager", "Added/Updated call %p in slot %d (State=%d, Held=%d)", call, free_slot, state,
			 g_call_state.active_calls[free_slot].is_held);
	} else {
		log_warn("BaresipManager", "Max calls reached, could not track call %p", call);
	}
}

static void remove_call(struct call *call)
{
	bool found_in_list = false;

	for (int i = 0; i < MAX_CALLS; i++) {
		if (g_call_state.active_calls[i].call == call) {
			log_info("BaresipManager", "Removed call %p from slot %d", call, i);
			g_call_state.active_calls[i].call = NULL;
			g_call_state.active_calls[i].state = CALL_STATE_IDLE;
			g_call_state.active_calls[i].peer_uri[0] = '\0';
			found_in_list = true;
			break;
		}
	}

	// Handle Current Call removal (Even if not in list due to overflow!)
	if (g_call_state.current_call == call) {
		log_info("BaresipManager", "Removing current_call %p (Found in list: %d)", call, found_in_list);
		g_call_state.current_call = NULL;
		g_call_state.state = CALL_STATE_IDLE; // Temporary

		// Auto-switch to first available active call
		bool switched = false;
		for (int j = 0; j < MAX_CALLS; j++) {
			if (g_call_state.active_calls[j].call) {
				g_call_state.current_call = g_call_state.active_calls[j].call;
				g_call_state.state = g_call_state.active_calls[j].state;
				log_info("BaresipManager", "Auto-switched to call %p", g_call_state.current_call);
				switched = true;
				break;
			}
		}

		if (!switched) {
			log_info("BaresipManager", "No active calls remaining. State forced to IDLE.");
			g_call_state.state = CALL_STATE_IDLE;
		}
	}
}

// Signal handler for call events
static void signal_handler(int sig)
{
	log_info("BaresipManager", "Signal: %d", sig);

	switch (sig) {
	case SIGINT:
	case SIGTERM:
		re_cancel();
		break;
	default:
		break;
	}
}

// Find account status by AOR
static account_status_t *find_account(const char *aor)
{
	if (!aor)
		return NULL;

	for (int i = 0; i < g_call_state.account_count; i++) {
		if (strcmp(g_call_state.accounts[i].aor, aor) == 0) {
			return &g_call_state.accounts[i];
		}
	}
	return NULL;
}

// Add or update account status
/* Unused function removed */
static void update_account_status(const char *aor, reg_status_t status)
{
	if (!aor)
		return;

	account_status_t *acc = find_account(aor);
	if (!acc && g_call_state.account_count < MAX_ACCOUNTS) {
		acc = &g_call_state.accounts[g_call_state.account_count++];
		safe_strncpy(acc->aor, aor, sizeof(acc->aor));
	}

	if (acc) {
		acc->status = status;
		log_info("BaresipManager", "Account %s status: %d", aor, status);

		if (g_call_state.reg_callback) {
			g_call_state.reg_callback(aor, status);
		}
	}
}

// Call event handler
// Forward declarations
extern const struct mod_export exports_sdl_vidisp;
extern const struct mod_export exports_window;

static void message_handler(struct ua *ua, const struct pl *peer, const struct pl *ctype, struct mbuf *body, void *arg)
{
	(void)ua;
	(void)ctype;
	(void)arg;

	// Convert body to C-string
	size_t len = mbuf_get_left(body);
	if (len == 0)
		return;

	char *text = mem_zalloc(len + 1, NULL);
	if (!text)
		return;

	mbuf_read_mem(body, (uint8_t *)text, len);
	text[len] = '\0'; // Ensure null termination

	// Get Peer URI string
	char from_uri[256];
	if (peer && peer->l > 0) {
		snprintf(from_uri, sizeof(from_uri), "%.*s", (int)peer->l, peer->p);
	} else {
		snprintf(from_uri, sizeof(from_uri), "unknown");
	}

	log_info("BaresipManager", "RECEIVED MESSAGE from %s: %s", from_uri, text);

	// Save to DB (Incoming = 0)
	db_chat_add(from_uri, 0, text);

	if (g_message_callback) {
		g_message_callback(from_uri, text);
	}

	mem_deref(text);

	// Note: Baresip handles 200 OK automatically if handler returns (or earlier
	// in the chain)
}

static void call_event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	(void)arg;
	struct ua *ua = bevent_get_ua(event);
	struct call *call = bevent_get_call(event);
	const char *prm = bevent_get_text(event);

	// Define peer early
	const char *peer = call ? call_peeruri(call) : "unknown";

	// Log EVERY event for debugging
	printf("BaresipManager: *** Event received: %d (%s) ***\n", ev, bevent_str(ev));
	fflush(stdout);

	// Handle registration events
	switch (ev) {
	case BEVENT_REGISTERING: {
		// struct ua *ua = ua; // REMOVED: Shadowing caused ua to be uninitialized!
		if (ua) {
			const char *aor = account_aor(ua_account(ua));
			log_info("BaresipManager", ">>> REGISTERING: %s", aor);
			update_account_status(aor, REG_STATUS_REGISTERING);
		} else {
			log_warn("BaresipManager", ">>> REGISTERING: ua is NULL!");
		}
		return;
	}
	case BEVENT_REGISTER_OK: {
		// struct ua *ua = ua; // REMOVED: Shadowing caused ua to be uninitialized!
		if (ua) {
			const char *aor = account_aor(ua_account(ua));
			update_account_status(aor, REG_STATUS_REGISTERED);
		} else {
			log_warn("BaresipManager", ">>> REGISTER_OK: ua is NULL! (Cannot update status)");
		}
		return;
	}
	case BEVENT_REGISTER_FAIL: {
		// struct ua *ua = ua; // REMOVED: Shadowing caused ua to be uninitialized!
		if (ua) {
			const char *aor = account_aor(ua_account(ua));
			const char *error_text = prm; // Use prm instead of text

			// Extract response code if possible (e.g. "404 Not Found")
			int code = 0;
			if (error_text) {
				code = atoi(error_text);
			}

			if (code == 401 || code == 403) {
				log_warn("BaresipManager", ">>> REGISTER_FAIL: Auth Error %d", code);
				account_status_t *acc = find_account(aor);
				if (acc)
					acc->status = REG_STATUS_AUTH_FAILED;
				if (g_call_state.reg_callback) {
					g_call_state.reg_callback(aor, REG_STATUS_AUTH_FAILED);
				}
			} else {
				log_warn("BaresipManager", ">>> REGISTER_FAIL: %s (reason: %s) ✗", aor,
					 error_text ? error_text : "unknown");
				account_status_t *acc = find_account(aor);
				if (acc)
					acc->status = REG_STATUS_FAILED;
				if (g_call_state.reg_callback) {
					g_call_state.reg_callback(aor, REG_STATUS_FAILED);
				}
			}
		} else {
			log_warn("BaresipManager", ">>> REGISTER_FAIL: ua is NULL!");
		}
		return;
	}
	case BEVENT_SIPSESS_CONN:
		// If call is null, try to get it from UA
		if (!g_call_state.current_call) {
			struct ua *ua = ua;
			const struct sip_msg *msg = NULL;

			log_debug("BaresipManager", "SIPSESS_CONN: Event UA=%p, Msg=%p", (void *)ua, (void *)msg);

			if (!ua && msg) {
				ua = uag_find_msg(msg);
				log_debug("BaresipManager", "SIPSESS_CONN: UA resolved via uag_find_msg: %p", (void *)ua);

				// Fuzzy Match Fallback
				if (!ua) {
					log_debug("BaresipManager", "SIPSESS_CONN: uag_find_msg failed. "
								    "Attempting fuzzy match...");
					struct le *le;
					for (le = ((struct list *)uag_list())->head; le; le = le->next) {
						struct ua *candidate_ua = le->data;
						struct account *acc = ua_account(candidate_ua);
						struct uri *acc_uri = account_luri(acc);

						if (acc_uri && acc_uri->user.l > 0 && msg->uri.user.l >= acc_uri->user.l) {
							if (0 == memcmp(msg->uri.user.p, acc_uri->user.p, acc_uri->user.l)) {
								ua = candidate_ua;
								log_debug("BaresipManager",
									  "SIPSESS_CONN: Fuzzy match success! UA: %p", (void *)ua);
								// account_set_catchall(acc, true);
								break;
							}
						}
					}
				}
			}

			if (ua) {
				if (msg) {
					// ua_accept(ua, msg);
				}
				call = ua_call(ua);
			}

			if (!g_call_state.current_call) {
				// Global search fallback
				struct le *le;
				for (le = ((struct list *)uag_list())->head; le; le = le->next) {
					struct ua *u = le->data;
					struct call *c = ua_call(u);
					if (c) {
						call = c;
						break;
					}
				}
			}
		}

		// Revert: Allow SIPSESS_CONN to trigger INCOMING state even if call is
		// NULL. The 'call_resume' fix handles the UI persistence.
		if (g_call_state.state == CALL_STATE_IDLE || g_call_state.state == CALL_STATE_INCOMING) {
			log_debug("BaresipManager", ">>> SIPSESS_CONN (IDLE/INCOMING)");
			g_call_state.state = CALL_STATE_INCOMING;
			g_call_state.current_call = call;

			if (call) {
				safe_strncpy(g_call_state.peer_uri, call_peeruri(call), sizeof(g_call_state.peer_uri));
			} else {
				// Keep "unknown" or existing peer_uri
			}

			if (g_call_state.callback)
				g_call_state.callback(CALL_STATE_INCOMING, call ? call_peeruri(call) : "unknown", (void *)call);
		}
		break;

	default:
		break;
	}

	// Handle call events (rest of the function)
	// struct call *call = call;
	// const char *peer = call ? call_peeruri(call) : "unknown"; // Moved to top

	// Debug log for call events
	log_info("BaresipManager", "Call Event: %d from %s (Call obj: %p)", ev, peer, (void *)call);

	switch (ev) {
	case BEVENT_CALL_INCOMING:
		log_info("BaresipManager", ">>> INCOMING CALL from %s", peer);
		g_call_state.state = CALL_STATE_INCOMING;

		// logic to find call if NULL
		if (!g_call_state.current_call) {
			struct le *le;
			for (le = ((struct list *)uag_list())->head; le; le = le->next) {
				struct ua *u = le->data;
				struct call *c = ua_call(u);
				if (c) {
					call = c;
					log_debug("BaresipManager", "Resolved NULL call in INCOMING event via global scan: %p",
						  (void *)call);
					break;
				}
			}
		}

		if (call) {
			g_call_state.current_call = call;
			g_call_state.state = CALL_STATE_INCOMING; // FIX: Ensure global state matches
			safe_strncpy(g_call_state.peer_uri, peer, sizeof(g_call_state.peer_uri));
			add_or_update_call(call, CALL_STATE_INCOMING, peer);
		} else {
			if (!g_call_state.current_call) {
				log_warn("BaresipManager", "WARNING: INCOMING event with no call "
							   "object and none found via scan!");
			}
		}

		// NOTIFY LISTENERS (INCOMING)
		if (g_listener_mgr.count > 0) {
			log_debug("BaresipManager", "Notifying %d listeners (INCOMING)", g_listener_mgr.count);
			for (int i = 0; i < g_listener_mgr.count; i++) {
				if (g_listener_mgr.listeners[i])
					g_listener_mgr.listeners[i](CALL_STATE_INCOMING, peer, (void *)call);
			}
		} else {
			log_warn("BaresipManager", "No listeners registered for INCOMING event!");
		}
		break;

	case BEVENT_CALL_OUTGOING:
		if (call) {
			struct account *acc = call_account(call);
		}
		if (g_listener_mgr.count > 0) {
			for (int i = 0; i < g_listener_mgr.count; i++)
				if (g_listener_mgr.listeners[i])
					g_listener_mgr.listeners[i](CALL_STATE_OUTGOING, peer, (void *)call);
		}
		break;
	case BEVENT_CALL_RINGING:
		log_info("BaresipManager", ">>> CALL RINGING");
		g_call_state.state = CALL_STATE_RINGING; // was OUTGOING, better RINGING
		if (call)
			add_or_update_call(call, CALL_STATE_RINGING, peer);
		if (g_listener_mgr.count > 0) {
			for (int i = 0; i < g_listener_mgr.count; i++)
				if (g_listener_mgr.listeners[i])
					g_listener_mgr.listeners[i](CALL_STATE_RINGING, peer, (void *)call);
		}
		break;

	case BEVENT_CALL_PROGRESS:
		log_info("BaresipManager", ">>> CALL PROGRESS (Early Media/183)");
		g_call_state.state = CALL_STATE_EARLY;
		if (call)
			add_or_update_call(call, CALL_STATE_EARLY, peer);
		if (g_listener_mgr.count > 0) {
			for (int i = 0; i < g_listener_mgr.count; i++)
				if (g_listener_mgr.listeners[i])
					g_listener_mgr.listeners[i](CALL_STATE_EARLY, peer, (void *)call);
		}
		break;

	case BEVENT_CALL_ESTABLISHED:
		log_info("BaresipManager", ">>> CALL ESTABLISHED");
		g_call_state.state = CALL_STATE_ESTABLISHED;
		g_call_state.current_call = call;
		if (call)
			add_or_update_call(call, CALL_STATE_ESTABLISHED, peer);
		if (g_listener_mgr.count > 0) {
			for (int i = 0; i < g_listener_mgr.count; i++)
				if (g_listener_mgr.listeners[i])
					g_listener_mgr.listeners[i](CALL_STATE_ESTABLISHED, peer, (void *)call);
		}
		break;

	case BEVENT_CALL_LOCAL_SDP:
		log_info("BaresipManager", ">>> CALL LOCAL SDP");
		break;

	case BEVENT_CALL_CLOSED: {
		log_info("BaresipManager", ">>> CALL CLOSED");
		bool established = (g_call_state.state == CALL_STATE_ESTABLISHED);
		bool incoming = call ? !call_is_outgoing(call) : false;

		call_type_t type;
		if (incoming) {
			if (established) {
				type = CALL_TYPE_INCOMING;
			} else {
				type = CALL_TYPE_MISSED;
			}
		} else {
			type = CALL_TYPE_OUTGOING;
		}

		const char *acc_aor = "";
		if (call) {
			struct account *acc = call_account(call);
			if (acc)
				acc_aor = account_aor(acc);
		}

		const char *number = peer;
		if (!number || strlen(number) == 0)
			number = "unknown";

		log_warn("BaresipManager", "EVENT_CLOSED: Peer='%s', Incoming=%d, Type=%d", number, incoming, type);
		fflush(stdout);
		if (history_add(number, number, type, acc_aor) != 0) {
			log_error("BaresipManager", "HistoryAdd FAILED");
		} else {
			log_warn("BaresipManager", "HistoryAdd SUCCESS");
		}
		fflush(stdout);

		// Check if this was the current call BEFORE removing it
		bool was_current = (g_call_state.current_call == call);

		// Remove from active list
		if (call)
			remove_call(call);

		// If the closed call was the current one, try to switch to another
		// If the closed call was the current one, try to switch to another
		if (was_current) {
			g_call_state.current_call = NULL;
			// Search for another active call
			int others = 0;
			for (int i = 0; i < MAX_CALLS; i++) {
				if (g_call_state.active_calls[i].call) {
					g_call_state.current_call = g_call_state.active_calls[i].call;
					g_call_state.state = g_call_state.active_calls[i].state;
					safe_strncpy(g_call_state.peer_uri, g_call_state.active_calls[i].peer_uri,
						     sizeof(g_call_state.peer_uri));
					others++;
					break;
				}
			}
			if (others == 0) {
				g_call_state.state = CALL_STATE_TERMINATED;
				log_info("BaresipManager", ">>> State set to TERMINATED (No other calls)");
			} else {
				log_info("BaresipManager", ">>> Switched to other call (Count: %d)", others);
			}
		} else if (g_call_state.current_call == NULL) {
			// If we are not current but somehow have no current call, set TERMINATED
			g_call_state.state = CALL_STATE_TERMINATED;
			log_info("BaresipManager", ">>> State set to TERMINATED (No current call)");
		} else {
			// Background call ended
			log_info("BaresipManager", ">>> Background call ended");
		}

		// LISTENER NOTIFICATION
		if (g_call_state.state == CALL_STATE_TERMINATED) {
			if (g_listener_mgr.count > 0) {
				log_info("BaresipManager", ">>> Notifying listeners (TERMINATED)");
				for (int i = 0; i < g_listener_mgr.count; i++) {
					if (g_listener_mgr.listeners[i])
						g_listener_mgr.listeners[i](CALL_STATE_TERMINATED, peer, (void *)call);
				}
			}

			// FIX: Auto-reset to IDLE after notifying TERMINATED
			g_call_state.state = CALL_STATE_IDLE;
			if (g_listener_mgr.count > 0) {
				for (int i = 0; i < g_listener_mgr.count; i++) {
					if (g_listener_mgr.listeners[i])
						g_listener_mgr.listeners[i](CALL_STATE_IDLE, peer, (void *)call);
				}
			}
		} else if (g_listener_mgr.count > 0) {
			// Notify update (e.g. switched to other call)
			if (g_call_state.current_call) {
				for (int i = 0; i < g_listener_mgr.count; i++) {
					if (g_listener_mgr.listeners[i])
						g_listener_mgr.listeners[i](g_call_state.state, g_call_state.peer_uri,
									    (void *)g_call_state.current_call);
				}
			}
		}

	default:
		break;
	}
	}
}

// ============================================================================
// LVGL Video Display Module Implementation
// ============================================================================

// Global video objects (Set by Applet)
static lv_obj_t *g_remote_video_obj = NULL;
static lv_obj_t *g_local_video_obj = NULL;

struct vidisp_st {
	struct le le;
	uint64_t id; // Generation ID
	struct vidframe *frame;
	struct vidsz size;
	mtx_t *lock;
	bool new_frame;
	bool is_local;

	struct vidframe *frame_conv; // Conversion buffer

	// RGB565 Buffer for LVGL
	uint8_t *rgb_buf;
	size_t rgb_buf_size;
	lv_img_dsc_t img_dsc;
};

static struct list vidisp_list;
static mtx_t *vidisp_list_lock = NULL;

// Global Generation Tracking
static uint64_t g_vidisp_id_counter = 0; // Monotonic counter
static uint64_t g_remote_video_gen = 0; // Current ID for remote video
static uint64_t g_local_video_gen = 0; // Current ID for local video
static mtx_t *g_vidisp_id_lock = NULL; // Separate lock for ID tracking

// YUV420P to RGB565 Conversion
// YUV420P to RGB565 Conversion
#if LV_COLOR_DEPTH == 16
static void yuv420p_to_rgb565(uint8_t *dst, const struct vidframe *vf)
{
	int w = vf->size.w;
	int h = vf->size.h;
	const uint8_t *y_plane = vf->data[0];
	const uint8_t *u_plane = vf->data[1];
	const uint8_t *v_plane = vf->data[2];
	int y_stride = vf->linesize[0];
	int u_stride = vf->linesize[1];
	int v_stride = vf->linesize[2];
	uint16_t *d = (uint16_t *)dst;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int Y = y_plane[y * y_stride + x];
			int U = u_plane[(y / 2) * u_stride + (x / 2)];
			int V = v_plane[(y / 2) * v_stride + (x / 2)];

			int C = Y - 16;
			int D = U - 128;
			int E = V - 128;

			int R = (298 * C + 409 * E + 128) >> 8;
			int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
			int B = (298 * C + 516 * D + 128) >> 8;

			if (R < 0)
				R = 0;
			else if (R > 255)
				R = 255;
			if (G < 0)
				G = 0;
			else if (G > 255)
				G = 255;
			if (B < 0)
				B = 0;
			else if (B > 255)
				B = 255;

			// RGB565 Packing: R(5), G(6), B(5)
			// R: top 5 bits of 8-bit R
			// G: top 6 bits of 8-bit G
			// B: top 5 bits of 8-bit B
			uint16_t r5 = (R >> 3) & 0x1F;
			uint16_t g6 = (G >> 2) & 0x3F;
			uint16_t b5 = (B >> 3) & 0x1F;

			*d++ = (r5 << 11) | (g6 << 5) | b5;

			// Debug Center Pixel
			if (x == w / 2 && y == h / 2) {
				static int frame_cnt = 0;
				if (frame_cnt++ % 30 == 0) {
					printf("BaresipManager: Pixel Debug (Y=%d U=%d V=%d) -> (R=%d G=%d "
					       "B=%d) -> Hex=0x%04X\n",
					       Y, U, V, R, G, B, ((r5 << 11) | (g6 << 5) | b5));
				}
			}
		}
	}
}
#endif

// YUV420P to ARGB8888 Conversion (Fixed Point)
// YUV420P to ARGB8888 Conversion (Fixed Point)
#if LV_COLOR_DEPTH == 32
static void yuv420p_to_argb8888(uint8_t *dst, const struct vidframe *vf)
{
	int w = vf->size.w;
	int h = vf->size.h;
	const uint8_t *y_plane = vf->data[0];
	const uint8_t *u_plane = vf->data[1];
	const uint8_t *v_plane = vf->data[2];
	int y_stride = vf->linesize[0];
	int u_stride = vf->linesize[1];
	int v_stride = vf->linesize[2];
	uint32_t *d = (uint32_t *)dst;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int Y = y_plane[y * y_stride + x];
			int U = u_plane[(y / 2) * u_stride + (x / 2)];
			int V = v_plane[(y / 2) * v_stride + (x / 2)];

			int C = Y - 16;
			int D = U - 128;
			int E = V - 128;

			int R = (298 * C + 409 * E + 128) >> 8;
			int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
			int B = (298 * C + 516 * D + 128) >> 8;

			if (R < 0)
				R = 0;
			else if (R > 255)
				R = 255;
			if (G < 0)
				G = 0;
			else if (G > 255)
				G = 255;
			if (B < 0)
				B = 0;
			else if (B > 255)
				B = 255;

			// ARGB8888: A (0xFF), R, G, B
			uint32_t final_pixel = (0xFF000000) | (R << 16) | (G << 8) | B;

			// Diagnostic Red Header removed

			*d++ = final_pixel;

			// (Diagnostic code removed)
		}
	}
}
// Async callback to safely unbind video sources on the main thread
// Async callback to safely unbind video sources on the main thread
typedef struct {
	bool is_local;
	const void *old_src;
	uint64_t id;
} video_cleanup_ctx_t;

static void lv_video_cleanup_async_cb(void *user_data)
{
	if (!user_data)
		return;
	video_cleanup_ctx_t *ctx = (video_cleanup_ctx_t *)user_data;
	bool is_local = ctx->is_local;
	const void *old_src = ctx->old_src;

	lv_obj_t *target = is_local ? g_local_video_obj : g_remote_video_obj;
	if (target && lv_obj_is_valid(target)) {
		// Only unbind if it's still being used (avoid clobbering new calls)
		const void *current_src = lv_img_get_src(target);
		if (current_src == old_src) {
			lv_img_set_src(target, NULL);
			lv_obj_invalidate(target);
		}
	}
	free(ctx);
}
#endif

static void lvgl_vidisp_destructor(void *arg)
{
	struct vidisp_st *st = arg;
	log_info("BaresipManager", "Destroying Video Stream: %p (Local=%d)", st, st->is_local);

	if (vidisp_list_lock) {
		mtx_lock(vidisp_list_lock);
		list_unlink(&st->le);
		mtx_unlock(vidisp_list_lock);
	}

	// FAILSAFE: Queue cleanup item for main thread processing
	// Replaces lv_async_call which was causing freeezes
	// We allocate a context to pass the specific 'img_dsc' address we are
	// destroying.
	video_cleanup_item_t *item = mem_zalloc(sizeof(*item), NULL);
	if (item) {
		item->is_local = st->is_local;
		item->old_src = &st->img_dsc;
		item->id = st->id;
		list_append(&video_cleanup_list, &item->le, item);
	}

	// Partial mitigation for race condition:
	// Zero out the dimension/pointer in the descriptor so if draw happens
	// before async callback, it sees empty data (hopefully avoiding fs_seek
	// crash).
	st->img_dsc.header.w = 0;
	st->img_dsc.header.h = 0;
	st->img_dsc.data = NULL;

	if (st->frame)
		mem_deref(st->frame);
	if (st->frame_conv)
		mem_deref(st->frame_conv);
	if (st->lock)
		mem_deref(st->lock);
	if (st->rgb_buf)
		mem_deref(st->rgb_buf);
}

static int lvgl_vidisp_alloc(struct vidisp_st **stp, const struct vidisp *vd, struct vidisp_prm *prm, const char *dev,
			     vidisp_resize_h *resizeh, void *arg)
{
	(void)prm;
	(void)dev;
	(void)resizeh;
	(void)arg;

	struct vidisp_st *st;
	int err;

	// Debug Log Allocation
	printf("BaresipManager: lvgl_vidisp_alloc called. Dev='%s'\n", dev ? dev : "NULL");

	// Determine if local based on specific device name passed from selfview
	// module OR fallback to pointer check (though pointer check relies on vid2
	// which might not be used here)
	bool is_local = (dev && strcmp(dev, "selfview_display") == 0) || (vd == vid2);

	printf("BaresipManager: [DEBUG] lvgl_vidisp_alloc called. Dev='%s', "
	       "Title='%s', vd=%p, vid2=%p, is_local=%d\n",
	       dev ? dev : "NULL", "", /* title unavailable in alloc */
	       (void *)vd, (void *)vid2, is_local);

	if (is_local) {
		printf("BaresipManager: Allocating LOCAL video display (is_local=true)\n");
	} else {
		printf("BaresipManager: Allocating REMOTE video display (is_local=false)\n");
	}

	st = mem_zalloc(sizeof(*st), lvgl_vidisp_destructor);
	if (!st)
		return ENOMEM;

	err = mutex_alloc(&st->lock);
	if (err) {
		mem_deref(st);
		return err;
	}

	st->is_local = is_local;

	if (!g_vidisp_id_lock) {
		mutex_alloc(&g_vidisp_id_lock);
	}
	mtx_lock(g_vidisp_id_lock);
	st->id = ++g_vidisp_id_counter;
	mtx_unlock(g_vidisp_id_lock);

	log_info("BaresipManager", "Allocated Video Stream ID: %lu (Local=%d)", st->id, is_local);

	mtx_lock(vidisp_list_lock);
	list_append(&vidisp_list, &st->le, st);
	mtx_unlock(vidisp_list_lock);

	*stp = st;

	printf("BaresipManager: lvgl_vidisp_alloc Success (st=%p, Local=%d)\n", (void *)st, is_local);
	fflush(stdout);
	return 0;
}

// NV12 to RGB565 Conversion
// NV12 to RGB565 Conversion
#if LV_COLOR_DEPTH == 16
static void nv12_to_rgb565(uint8_t *dst, const struct vidframe *vf)
{
	int w = vf->size.w;
	int h = vf->size.h;
	const uint8_t *y_plane = vf->data[0];
	const uint8_t *uv_plane = vf->data[1]; // NV12 has interleaved UV
	int y_stride = vf->linesize[0];
	int uv_stride = vf->linesize[1];
	uint16_t *d = (uint16_t *)dst;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int Y = y_plane[y * y_stride + x];
			int uv_off = (y / 2) * uv_stride + (x & ~1);
			int U = uv_plane[uv_off];
			int V = uv_plane[uv_off + 1];

			int C = Y - 16;
			int D = U - 128;
			int E = V - 128;

			int R = (298 * C + 409 * E + 128) >> 8;
			int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
			int B = (298 * C + 516 * D + 128) >> 8;

			if (R < 0)
				R = 0;
			else if (R > 255)
				R = 255;
			if (G < 0)
				G = 0;
			else if (G > 255)
				G = 255;
			if (B < 0)
				B = 0;
			else if (B > 255)
				B = 255;

			uint16_t r5 = (R >> 3) & 0x1F;
			uint16_t g6 = (G >> 2) & 0x3F;
			uint16_t b5 = (B >> 3) & 0x1F;

			*d++ = (r5 << 11) | (g6 << 5) | b5;
		}
	}
}
#endif

// NV12 to ARGB8888 Conversion
// NV12 to ARGB8888 Conversion
#if LV_COLOR_DEPTH == 32
static void nv12_to_argb8888(uint8_t *dst, const struct vidframe *vf)
{
	int w = vf->size.w;
	int h = vf->size.h;
	const uint8_t *y_plane = vf->data[0];
	const uint8_t *uv_plane = vf->data[1];
	int y_stride = vf->linesize[0];
	int uv_stride = vf->linesize[1];
	uint32_t *d = (uint32_t *)dst;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int Y = y_plane[y * y_stride + x];
			int uv_off = (y / 2) * uv_stride + (x & ~1);
			int U = uv_plane[uv_off];
			int V = uv_plane[uv_off + 1];

			int C = Y - 16;
			int D = U - 128;
			int E = V - 128;

			int R = (298 * C + 409 * E + 128) >> 8;
			int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
			int B = (298 * C + 516 * D + 128) >> 8;

			if (R < 0)
				R = 0;
			else if (R > 255)
				R = 255;
			if (G < 0)
				G = 0;
			else if (G > 255)
				G = 255;
			if (B < 0)
				B = 0;
			else if (B > 255)
				B = 255;

			// Restore correct pixel assignment
			*d++ = (0xFF000000) | (R << 16) | (G << 8) | B;

			/*
      // FORCE TEST PATTERN (DIAGNOSTIC)
      // Top half RED, Bottom half GREEN
      if (y < h/2) {
          int R = 255; int G = 0; int B = 0;
          *d++ = (0xFF000000) | (R << 16) | (G << 8) | B;
      } else {
          int R = 0; int G = 255; int B = 0;
          *d++ = (0xFF000000) | (R << 16) | (G << 8) | B;
      }
      */

			//*d++ = (0xFF000000) | (R << 16) | (G << 8) | B;
		}
	}
}
#endif

// YUYV422 (YUY2) to RGB565 Conversion (for LV_COLOR_DEPTH=16)
#if LV_COLOR_DEPTH == 16
static void yuyv422_to_rgb565(uint8_t *dst, const struct vidframe *vf)
{
	int w = vf->size.w;
	int h = vf->size.h;
	const uint8_t *src = vf->data[0];
	int stride = vf->linesize[0];
	uint16_t *d = (uint16_t *)dst;

	for (int y = 0; y < h; y++) {
		const uint8_t *line_src = src + y * stride;
		for (int x = 0; x < w; x += 2) {
			// Read Macro-pixel YUYV
			int Y0 = line_src[x * 2 + 0];
			int U = line_src[x * 2 + 1];
			int Y1 = line_src[x * 2 + 2];
			int V = line_src[x * 2 + 3];

			// Debug: Check middle pixel values once per second
			if (x == w / 2 && y == h / 2) {
				static int64_t last_log = 0;
				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				if (ts.tv_sec - last_log > 1) {
					last_log = ts.tv_sec;
					printf("BaresipManager: [YUYV Debug] Y0=%d U=%d Y1=%d V=%d\n", Y0, U, Y1, V);
				}
			}

			// Pixel 0
			int C = Y0 - 16;
			int D = U - 128;
			int E = V - 128;
			int R = (298 * C + 409 * E + 128) >> 8;
			int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
			int B = (298 * C + 516 * D + 128) >> 8;
			R = (R < 0) ? 0 : ((R > 255) ? 255 : R);
			G = (G < 0) ? 0 : ((G > 255) ? 255 : G);
			B = (B < 0) ? 0 : ((B > 255) ? 255 : B);

			uint16_t r5 = (R >> 3) & 0x1F;
			uint16_t g6 = (G >> 2) & 0x3F;
			uint16_t b5 = (B >> 3) & 0x1F;
			*d++ = (r5 << 11) | (g6 << 5) | b5;

			// Pixel 1
			C = Y1 - 16;
			R = (298 * C + 409 * E + 128) >> 8;
			G = (298 * C - 100 * D - 208 * E + 128) >> 8;
			B = (298 * C + 516 * D + 128) >> 8;
			R = (R < 0) ? 0 : ((R > 255) ? 255 : R);
			G = (G < 0) ? 0 : ((G > 255) ? 255 : G);
			B = (B < 0) ? 0 : ((B > 255) ? 255 : B);

			r5 = (R >> 3) & 0x1F;
			g6 = (G >> 2) & 0x3F;
			b5 = (B >> 3) & 0x1F;
			*d++ = (r5 << 11) | (g6 << 5) | b5;
		}
	}
}

#endif

// YUYV422 (YUY2) to ARGB8888 Conversion
// Packet Format: [Y0 U0 Y1 V0] -> Pixel 0 (Y0,U0,V0), Pixel 1 (Y1,U0,V0)
#if LV_COLOR_DEPTH == 32
static void yuyv422_to_argb8888(uint8_t *dst, const struct vidframe *vf)
{
	int w = vf->size.w;
	int h = vf->size.h;
	const uint8_t *src = vf->data[0];
	int stride = vf->linesize[0];
	uint32_t *d = (uint32_t *)dst;

	for (int y = 0; y < h; y++) {
		const uint8_t *line_src = src + y * stride;
		for (int x = 0; x < w; x += 2) {
			// Read Macro-pixel YUYV
			int Y0 = line_src[x * 2 + 0];
			int U = line_src[x * 2 + 1];
			int Y1 = line_src[x * 2 + 2];
			int V = line_src[x * 2 + 3];

			// Pixel 0
			int C = Y0 - 16;
			int D = U - 128;
			int E = V - 128;
			int R = (298 * C + 409 * E + 128) >> 8;
			int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
			int B = (298 * C + 516 * D + 128) >> 8;
			R = (R < 0) ? 0 : ((R > 255) ? 255 : R);
			G = (G < 0) ? 0 : ((G > 255) ? 255 : G);
			B = (B < 0) ? 0 : ((B > 255) ? 255 : B);
			*d++ = (0xFF000000) | (R << 16) | (G << 8) | B;

			// Pixel 1
			C = Y1 - 16;
			R = (298 * C + 409 * E + 128) >> 8;
			G = (298 * C - 100 * D - 208 * E + 128) >> 8;
			B = (298 * C + 516 * D + 128) >> 8;
			R = (R < 0) ? 0 : ((R > 255) ? 255 : R);
			G = (G < 0) ? 0 : ((G > 255) ? 255 : G);
			B = (B < 0) ? 0 : ((B > 255) ? 255 : B);
			*d++ = (0xFF000000) | (R << 16) | (G << 8) | B;
		}
	}
}
#endif

static int lvgl_vidisp_update(struct vidisp_st *st, bool fullscreen, int orient, const struct vidrect *window)
{
	(void)fullscreen;
	(void)window;
	(void)orient;
	if (!st)
		return EINVAL;
	return 0;
}

static int lvgl_vidisp_disp(struct vidisp_st *st, const char *title, const struct vidframe *frame, uint64_t timestamp)
{
	(void)title;
	(void)timestamp;
	if (!st || !frame)
		return EINVAL;

	// Optimization: Instant Hangup Check
	if (g_call_state.state == CALL_STATE_IDLE || g_call_state.state == CALL_STATE_TERMINATED) {
		return 0;
	}

	// Debug: Check if disp is called
	static int disp_log_limit = 0;
	// Always log local frames to verify they exist
	if (st->is_local || disp_log_limit++ % 30 == 0) {
		if (st->is_local && disp_log_limit % 300 == 0) {
			// Keep a heartbeat for local video but less verbose
			printf("BaresipManager: Local Video Active (Format=%d)\n", frame->fmt);
		}
	}

	mtx_lock(st->lock);

	// Check size/format change
	if (!st->frame || !vidsz_cmp(&st->size, &frame->size) || st->frame->fmt != frame->fmt) {
		if (st->frame)
			mem_deref(st->frame);
		st->frame = NULL;
		st->size = frame->size;

		int err = vidframe_alloc(&st->frame, frame->fmt, &frame->size);
		if (err) {
			mtx_unlock(st->lock);
			return err;
		}

		// Re-allocate RGB buffer
		if (st->rgb_buf)
			mem_deref(st->rgb_buf);

#if LV_COLOR_DEPTH == 16
		// RGB565 = 2 bytes per pixel
		st->rgb_buf_size = st->size.w * st->size.h * 2;
		st->img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR; // RGB565
#else
		// ARGB8888 = 4 bytes per pixel
		st->rgb_buf_size = st->size.w * st->size.h * 4;
		st->img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA; // ARGB8888 + Alpha
#endif

		st->rgb_buf = mem_zalloc(st->rgb_buf_size, NULL);

		log_info("BaresipManager", "Video Resize: %dx%d (Buf: %zu bytes, Depth: %d)", st->size.w, st->size.h,
			 st->rgb_buf_size, LV_COLOR_DEPTH);

		// Initialize Image Descriptor
		st->img_dsc.header.always_zero = 0;
		st->img_dsc.header.w = st->size.w;
		st->img_dsc.header.h = st->size.h;
		st->img_dsc.data_size = st->rgb_buf_size;
		st->img_dsc.data = st->rgb_buf;
	}

	// Copy YUV Data
	vidframe_copy(st->frame, frame);

	// Convert to RGB immediately (Decode Thread)
	if (st->rgb_buf && MIN(frame->size.w, st->size.w) > 0) {
		if (frame->fmt == VID_FMT_YUV420P) {
#if LV_COLOR_DEPTH == 16
			yuv420p_to_rgb565(st->rgb_buf, frame);
#else
			yuv420p_to_argb8888(st->rgb_buf, frame);
#endif
			// Manual dispatch removed to rely on Universal Fallback
		} else if (frame->fmt == VID_FMT_NV12) {
#if LV_COLOR_DEPTH == 16
			nv12_to_rgb565(st->rgb_buf, frame);
#else
			nv12_to_argb8888(st->rgb_buf, frame);
#endif
		} else if (frame->fmt == VID_FMT_YUYV422) {
			// Native YUYV Support (Fixes Local Video Black Screen)
#if LV_COLOR_DEPTH == 32
			yuyv422_to_argb8888(st->rgb_buf, frame);
#else
			yuyv422_to_rgb565(st->rgb_buf, frame);
#endif
		} else {
			// UNIVERSAL FALLBACK: Use vidconv to convert to YUV420P
			if (!st->frame_conv || !vidsz_cmp(&st->frame_conv->size, &frame->size)) {
				if (st->frame_conv)
					mem_deref(st->frame_conv);
				st->frame_conv = NULL;
				int err = vidframe_alloc(&st->frame_conv, VID_FMT_YUV420P, &frame->size);
				if (err) {
					printf("BaresipManager: Converter Alloc Failed %d\n", err);
					mtx_unlock(st->lock);
					return err;
				}
			}

			// Convert Generic -> YUV420P
			vidconv(st->frame_conv, frame, NULL);

			// Render YUV420P -> RGB
#if LV_COLOR_DEPTH == 16
			yuv420p_to_rgb565(st->rgb_buf, st->frame_conv);
#else
			yuv420p_to_argb8888(st->rgb_buf, st->frame_conv);
#endif

			static int fallback_log = 0;
			if (fallback_log++ % 60 == 0)
				printf("BaresipManager: Using Universal Fallback for format %d\n", frame->fmt);
		}
		st->new_frame = true;
	}

	mtx_unlock(st->lock);
	return 0;
}

static void lvgl_vidisp_hide(struct vidisp_st *st)
{
	(void)st;
}

bool baresip_manager_is_video_call(void)
{
	return g_call_state.is_video_call;
}

// API to set LVGL Objects
void baresip_manager_set_video_objects(void *remote, void *local)
{
	g_remote_video_obj = (lv_obj_t *)remote;
	g_local_video_obj = (lv_obj_t *)local;
}

// Process Video - Called from Main Thread (LVGL Loop)
void baresip_manager_process_video(void)
{
	if (!vidisp_list_lock)
		return;

	// Revert: Block on list lock to ensure we don't miss new streams
	// If this blocks, it means alloc/free is happening, which should be fast.
	mtx_lock(vidisp_list_lock);

	// 1. Process Cleanup Queue (Main Thread) -> Move to temp list to avoid
	// holding lock while calling LVGL
	struct list temp_list;
	list_init(&temp_list);

	struct le *le_c = video_cleanup_list.head;
	while (le_c) {
		video_cleanup_item_t *item = le_c->data;
		le_c = le_c->next; // advance

		list_unlink(&item->le);
		list_append(&temp_list, &item->le, item);
	}

	mtx_unlock(vidisp_list_lock);

	// Now process safely without lock
	le_c = temp_list.head;
	while (le_c) {
		video_cleanup_item_t *item = le_c->data;
		le_c = le_c->next;

		lv_obj_t *target = item->is_local ? g_local_video_obj : g_remote_video_obj;
		if (target && lv_obj_is_valid(target)) {
			// Context check
			// ABA Fix: Check Generation ID
			uint64_t current_gen = item->is_local ? g_local_video_gen : g_remote_video_gen;

			if (current_gen != item->id) {
				log_info("BaresipManager", "Skipping cleanup for obsolete generation %lu (Current: %lu)", item->id,
					 current_gen);
			} else {
				const void *curr_src = lv_img_get_src(target);
				if (curr_src == item->old_src) {
					lv_img_set_src(target, NULL);
					lv_obj_invalidate(target);
					log_info("BaresipManager", "Cleaned up video generation %lu (Local=%d)", item->id,
						 item->is_local);
				}
			}
		}
		list_unlink(&item->le);
		mem_deref(item);
	}

	// Re-lock for frame processing
	mtx_lock(vidisp_list_lock);

	// Frame Update Loop - Refactored to Release Lock during LVGL calls
	// We process ONE item at a time, unlock, update UI, then restart the loop.
	// This is inefficient O(N^2) but N is max 2 (Local + Remote), so it's fine.
	// This prevents holding vidisp_list_lock while calling potentially slow LVGL
	// functions.

	bool activity = true;
	while (activity) {
		activity = false;
		struct le *le;

		for (le = vidisp_list.head; le; le = le->next) {
			struct vidisp_st *st = le->data;

			if (st->new_frame && st->rgb_buf && mtx_trylock(st->lock) == thrd_success) {
				// GUARD: If this is an Audio Call, Do NOT render local video
				// This prevents "ghost" video from appearing if the previous call
				// wasn't fully cleaned up or if a race condition started the stream
				// early.
				if (st->is_local && !baresip_manager_is_video_call()) {
					st->new_frame = false; // Discard frame
					mtx_unlock(st->lock);

					// Force clear existing image if any
					lv_obj_t *target = g_local_video_obj;
					if (target && lv_obj_is_valid(target)) {
						// Check if we need to clear (minimize locking churn)
						if (lv_img_get_src(target) != NULL) {
							mtx_unlock(vidisp_list_lock);
							lv_img_set_src(target, NULL);
							lv_obj_invalidate(target);
							mtx_lock(vidisp_list_lock);
							activity = true;
							break; // Restart loop
						}
					}
					continue;
				}

				// Found a frame to update!

				// LIFECYCLE SAFETY: Use ref counting to prevent object destruction
				// while we are using it outside the lock.
				// We must ref INSIDE the lock.
				mem_ref(st);

				// 1. Snapshot data we need
				lv_obj_t *target = st->is_local ? g_local_video_obj : g_remote_video_obj;
				const void *src = &st->img_dsc;

				// Clear flag so we don't process this again immediately
				st->new_frame = false;

				// Unlock frame lock
				mtx_unlock(st->lock);

				// 2. Validate Target
				if (target && lv_obj_is_valid(target)) {
					// 3. UNLOCK LIST LOCK before calling LVGL
					mtx_unlock(vidisp_list_lock);

					// 3.1 ABA Fix: Update Global Generation Counter
					// This tells the cleanup thread: "This ID is now active on this
					// object"
					if (g_vidisp_id_lock) {
						mtx_lock(g_vidisp_id_lock);
						if (st->is_local) {
							if (g_local_video_gen != st->id) {
								g_local_video_gen = st->id;
								log_info("BaresipManager",
									 "Setting Active Local Video Generation: %lu", st->id);
							}
						} else {
							if (g_remote_video_gen != st->id) {
								g_remote_video_gen = st->id;
								log_info("BaresipManager",
									 "Setting Active Remote Video Generation: %lu", st->id);
							}
						}
						mtx_unlock(g_vidisp_id_lock);
					}

					// 4. Perform UI Update (Safe, no locks held)
					lv_img_set_src(target, src);

					// FIX: Unhide local video container on first frame
					if (st->is_local) {
						lv_obj_clear_flag(target, LV_OBJ_FLAG_HIDDEN);
						lv_obj_t *parent = lv_obj_get_parent(target);
						if (parent)
							lv_obj_clear_flag(parent, LV_OBJ_FLAG_HIDDEN);
					}

					// Auto-Scale Logic
					lv_obj_t *parent = lv_obj_get_parent(target);
					if (parent) {
						lv_coord_t p_w = lv_obj_get_width(parent);
						lv_coord_t p_h = lv_obj_get_height(parent);
						const lv_img_dsc_t *dsc = (const lv_img_dsc_t *)src;
						int img_w = dsc->header.w;
						int img_h = dsc->header.h;
						if (p_w > 0 && p_h > 0 && img_w > 0 && img_h > 0) {
							int zoom_w = (p_w * 256) / img_w;
							int zoom_h = (p_h * 256) / img_h;
							int zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
							if (zoom > 256)
								zoom = 256; // Limit zoom
							lv_img_set_zoom(target, zoom);
						}
					}

					// 5. Release Reference (Unsafe Step for locking)
					// If this triggers destructor, destructor will try to lock
					// vidisp_list_lock. So we MUST NOT hold vidisp_list_lock here.
					mem_deref(st);

					// 6. RE-LOCK LIST LOCK and restart loop
					mtx_lock(vidisp_list_lock);
					activity = true; // Restart loop
					break; // Break inner for-loop, restart while logic
				} else {
					// Target invalid, release ref immediately
					// SAFE release: unlock, deref, relock
					mtx_unlock(vidisp_list_lock);
					mem_deref(st);
					mtx_lock(vidisp_list_lock);
				}
			}
		}
	}
	mtx_unlock(vidisp_list_lock);
}

// Removed duplicate/obsolete sdl_vidisp code and redefinitions
struct video_rect {
	int x;
	int y;
	int w;
	int h;
};
static struct video_rect g_video_rect = { 0, 0, 0, 0 };
static struct video_rect g_local_video_rect = { 0, 0, 0, 0 };

// Video Display Module Pointers - Moved to Global Scope

// vidfmt_to_sdl removed

void baresip_manager_set_video_rect(int x, int y, int w, int h)
{
	g_video_rect.x = x;
	g_video_rect.y = y;
	g_video_rect.w = w;
	g_video_rect.h = h;
}

void baresip_manager_set_local_video_rect(int x, int y, int w, int h)
{
	g_local_video_rect.x = x;
	g_local_video_rect.y = y;
	g_local_video_rect.w = w;
	g_local_video_rect.h = h;
}

// Removed hanging sdl_vid_render logic

static void create_default_config(const char *config_path)
{
	FILE *f = fopen(config_path, "w");
	if (f) {
		fprintf(f, "# Minimal Baresip Config\n"
			   "poll_method\t\tpoll\n"
			   "sip_listen\t\t0.0.0.0:5060\n"
			   "sip_transports\t\tudp,tcp\n"
			   "audio_path\t\t/usr/share/baresip\n"
			   "# Modules\n"
			   "module_path\t\t/usr/lib/baresip/modules\n"
			   "# Audio\n"
#ifdef __APPLE__
			   "audio_player\t\taudiounit,nil\n"
			   "audio_source\t\taudiounit,nil\n"
			   "audio_alert\t\taudiounit,nil\n"
#else
			   "audio_player\t\tsdl,nil\n"
			   "audio_source\t\tsdl,nil\n"
			   "audio_alert\t\tsdl,nil\n"
#endif
#ifdef __APPLE__
			   "video_source\t\tfakevideo\n"
#else
			   "video_source\t\tdummy,nil\n" // Use dummy or v4l2 if available
#endif
			   "video_display\t\tsdl_vidisp,nil\n"
			   "# Connect\n"
			   "sip_verify_server\tyes\n"
			   "answer_mode\t\tmanual\n"
			   "# Modules\n"
			   "module_load\t\tstdio.so\n"
			   "module_load\t\tg711.so\n"
			   "module_load\t\tuuid.so\n"
			   "module_load\t\taccount.so\n"
			   "# Network\n"
		// MacOS specific modules
#ifdef __APPLE__
			   "module_load\t\taudiounit.so\n"
			   "module_load\t\tavcapture.so\n"
			   "module_load\t\tcoreaudio.so\n"
			   "module_load\t\tselfview.so\n"
#endif
			   "module_load\t\tfakevideo.so\n"
			   "module_load\t\topus.so\n"
			   "# Network Modules\n"
			   "module_load\t\tudp.so\n"
			   "module_load\t\ttcp.so\n"
			   "module_load\t\tice.so\n"
			   "module_load\t\tstun.so\n"
			   "module_load\t\tturn.so\n"
			   "module_load\t\toutbound.so\n"
			   "# Selfview\n"
			   "video_selfview\t\tsdl_vidisp\n"
			   "# Keepalive\n"
			   "sip_keepalive_interval\t15\n");
		fclose(f);
		log_info("BaresipManager", "Wrote minimal config to %s", config_path);
	}
}

static void patch_config_file(const char *config_path, const app_config_t *app_conf)
{
	char temp_path[1024];
	snprintf(temp_path, sizeof(temp_path), "%s.tmp", config_path);

	FILE *f_in = fopen(config_path, "r");
	FILE *f_out = fopen(temp_path, "w");

	if (!f_in) {
		log_warn("BaresipManager", "Config file not found for patching, skipping.");
		if (f_out)
			fclose(f_out);
		return;
	}
	if (!f_out) {
		log_error("BaresipManager", "Failed to open temp config file for writing");
		fclose(f_in);
		return;
	}

	char line[512];
	bool selfview_found = false;
	bool keepalive_found = false;
	bool source_found = false;

	while (fgets(line, sizeof(line), f_in)) {
		// Check for video_selfview (Loose match to handle whitespace)
		if (strstr(line, "video_selfview")) {
			fprintf(f_out, "video_selfview\t\tsdl_vidisp,selfview_display\n");
			selfview_found = true;
			log_info("BaresipManager",
				 "Patched video_selfview to sdl_vidisp,selfview_display (Matched "
				 "line: %s)",
				 line);
		}
		// Check for video_source
		else if (strstr(line, "video_source")) {
			fprintf(f_out, "video_source\t\tv4l2,/dev/video0\n");
			source_found = true;
			log_info("BaresipManager", "Patched video_source to v4l2,/dev/video0");
		}
		// Correct SIP Listen Port if it was set to 0.0.0.0:0 (Random)
		else if (strstr(line, "sip_listen") && strstr(line, "0.0.0.0:0")) {
			fprintf(f_out, "sip_listen\t\t0.0.0.0:5060\n");
			log_info("BaresipManager", "Patched sip_listen to 0.0.0.0:5060");
		}
		// Check for keepalive
		else if (strstr(line, "sip_keepalive_interval")) {
			fprintf(f_out, "%s", line);
			keepalive_found = true;
		} else {
			fprintf(f_out, "%s", line);
		}
	}

	if (!selfview_found) {
		fprintf(f_out, "\n# Selfview\nvideo_selfview\t\tsdl_vidisp,selfview_display\n");
		log_info("BaresipManager", "Appended video_selfview config");
	}

	if (!source_found) {
		fprintf(f_out, "\n# Video Source\nvideo_source\t\tv4l2,/dev/video0\n");
		log_info("BaresipManager", "Appended video_source config (v4l2)");
	}

	if (!keepalive_found) {
		fprintf(f_out, "\n# Keepalive\nsip_keepalive_interval\t15\n");
		log_info("BaresipManager", "Appended sip_keepalive_interval config");
	}

	// Ensure modules are loaded
	fprintf(f_out, "module_load\t\tstun.so\n");
	fprintf(f_out, "module_load\t\tturn.so\n");
	fprintf(f_out, "module_load\t\tice.so\n");
	fprintf(f_out, "module_load\t\tnatpmp.so\n");

	// FORCE LOAD V4L2 and SDL (if dynamic)
	// Check if we need to load v4l2 explicitly
	if (!strstr(line, "v4l2.so")) {
		fprintf(f_out, "module_load\t\tv4l2.so\n");
	}
	// Force load selfview if needed (though it's our static code?)
	// If we rely on baresip internal 'selfview.so', we need to load it?
	// No, selfview is a module in deps/baresip/modules/selfview.
	// If compiled as shared, we MUST load it.
	fprintf(f_out, "module_load\t\tselfview.so\n");

	fclose(f_in);
	fclose(f_out);

	if (rename(temp_path, config_path) != 0) {
		log_error("BaresipManager", "Failed to replace config file after patching");
		remove(temp_path);
	} else {
		log_info("BaresipManager", "Config file patched successfully");
	}
}

/* Unused functions removed */

// Orphan code removed

int baresip_manager_init(void)
{
	static bool initialized = false;
	if (initialized)
		return 0;
	initialized = true;

	// Initialize libre (CORE REQUIREMENT)
	int err = libre_init();
	if (err) {
		log_error("BaresipManager", "Failed to initialize libre: %d", err);
		return err;
	}

	// Initialize Database First
	db_init();

	// Initialize History/Database Subsystem
	history_manager_init();
	printf("BaresipManager: History Init Done\n");
	fflush(stdout);

	// create_default_accounts(); // Removed as per request

	// Mutex initialized via PTHREAD_MUTEX_INITIALIZER

	struct config *cfg;

	// Set up signal handlers
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	// --- Apply Application Settings Overrides ---
	// Allocate on heap to avoid stack smashing
	app_config_t *app_conf = calloc(1, sizeof(app_config_t));
	if (!app_conf) {
		log_error("BaresipManager", "Failed to allocate app_config");
		libre_close();
		return ENOMEM;
	}

	if (config_load_app_settings(app_conf) != 0) {
		log_warn("BaresipManager", "Failed to load app settings (using defaults)");
		// calloc already zeroed it
	}
	memset(app_conf, 0, sizeof(app_config_t)); // Redundant but safe? No, calloc
		// is safer. Removed memset.

	// Create baresip configuration
	// Ensure .baresip directory exists
	char home_dir[256];
	const char *home = getenv("HOME");
	if (home) {
		snprintf(home_dir, sizeof(home_dir), "%s/.baresip", home);
		mkdir(home_dir, 0755);

		// Create accounts file if not exists
		char accounts_path[512];
		snprintf(accounts_path, sizeof(accounts_path), "%s/accounts", home_dir);
		FILE *f = fopen(accounts_path, "a");
		if (f)
			fclose(f);

		// Config path logic
		char config_path[512];
		snprintf(config_path, sizeof(config_path), "%s/config", home_dir);

		FILE *f_check = fopen(config_path, "r");
		long file_size = 0;
		if (f_check) {
			fseek(f_check, 0, SEEK_END);
			file_size = ftell(f_check);
			fclose(f_check);
		}

		if (file_size == 0) {
			create_default_config(config_path);
		}
		// 2. Patch config file (Robustly ensure selfview=sdl_vidisp and
		// keepalive)
		patch_config_file(config_path, app_conf);

		log_info("BaresipManager", "Config dir: %s", home_dir);
	}

	printf("BaresipManager: Configuring...\n");
	fflush(stdout);

	// Configure baresip from config file
	int cfg_err = conf_configure();
	if (cfg_err) {
		printf("BaresipManager: conf_configure failed: %d\n", cfg_err);
		fflush(stdout);
		log_warn("BaresipManager", "conf_configure failed: %d (Using defaults)", cfg_err);
	}

	cfg = conf_config();
	if (!cfg) {
		printf("BaresipManager: Failed to get config\n");
		fflush(stdout);
		log_error("BaresipManager", "Failed to get config");
		free(app_conf);
		libre_close();
		return EINVAL;
	}
	printf("BaresipManager: Config Loaded\n");
	fflush(stdout);

	// app_conf is already loaded above
	if (1) {
		log_info("BaresipManager", "Applying App Settings Overrides...");

		// 1. Listen Address
		if (strlen(app_conf->listen_address) > 0) {
			log_info("BaresipManager", "Override Listen Address: %s", app_conf->listen_address);
			safe_strncpy(cfg->sip.local, app_conf->listen_address, sizeof(cfg->sip.local));
		} else {
			// Enforce 5060 if config has 0 (random) or empty
			if (strstr(cfg->sip.local, ":0") || strlen(cfg->sip.local) == 0) {
				log_info("BaresipManager", "Enforcing Default Listen Port: 0.0.0.0:5060");
				safe_strncpy(cfg->sip.local, "0.0.0.0:5060", sizeof(cfg->sip.local));
			}
		}

		// 2. DNS Servers
		if (strlen(app_conf->dns_servers) > 0) {
			log_info("BaresipManager", "Override DNS Servers: %s", app_conf->dns_servers);
			cfg->net.nsc = 0; // Reset existing
			char *dup = strdup(app_conf->dns_servers);
			if (dup) {
				char *tok = strtok(dup, ",");
				while (tok && cfg->net.nsc < NET_MAX_NS) {
					// Trim leading whitespace
					while (*tok == ' ')
						tok++;

					char dns_addr[64];
					safe_strncpy(dns_addr, tok, sizeof(dns_addr));

					// Validate and append port 53 if missing
					struct sa temp_sa;
					if (sa_decode(&temp_sa, dns_addr, strlen(dns_addr)) != 0) {
						// Try appending :53
						char with_port[64];
						snprintf(with_port, sizeof(with_port), "%s:53", dns_addr);
						if (sa_decode(&temp_sa, with_port, strlen(with_port)) == 0) {
							safe_strncpy(dns_addr, with_port, sizeof(dns_addr));
						}
					}

					if (cfg->net.nsc < NET_MAX_NS) {
						snprintf(cfg->net.nsv[cfg->net.nsc].addr, 64, "%s", dns_addr);
						cfg->net.nsv[cfg->net.nsc].addr[63] = '\0';
						cfg->net.nsc++;
					}
					tok = strtok(NULL, ",");
				}
				free(dup);
			}
		}

		// 3. Video Frame Size
		switch (app_conf->video_frame_size) {
		case 0:
			cfg->video.width = 1920;
			cfg->video.height = 1080;
			break;
		case 1:
			cfg->video.width = 1280;
			cfg->video.height = 720;
			break;
		case 2:
			cfg->video.width = 640;
			cfg->video.height = 480;
			break;
		case 3:
			cfg->video.width = 320;
			cfg->video.height = 240;
			break;
		default:
			// Default to 720p if unknown
			cfg->video.width = 1280;
			cfg->video.height = 720;
			break;
		}
		log_info("BaresipManager", "Override Video Size: %dx%d", cfg->video.width, cfg->video.height);
	}

	// Free app_conf as it's no longer needed
	free(app_conf);
	// --------------------------------------------

#ifdef __APPLE__
	// macOS config (omitted for brevity, not building for macOS)
#elif defined(__linux__)
	// FORCE audio driver to alsa on Linux
	log_info("BaresipManager", "Forcing audio driver to 'alsa' for Linux...");
	snprintf(cfg->audio.play_mod, sizeof(cfg->audio.play_mod), "alsa");
	snprintf(cfg->audio.src_mod, sizeof(cfg->audio.src_mod), "alsa");
	snprintf(cfg->audio.alert_mod, sizeof(cfg->audio.alert_mod), "alsa");

	// Force video source to v4l2 on Linux
	if (access("/dev/video0", F_OK) == 0) {
		log_info("BaresipManager", "Found /dev/video0 - Using v4l2");
		snprintf(cfg->video.src_mod, sizeof(cfg->video.src_mod), "v4l2");
		snprintf(cfg->video.src_dev, sizeof(cfg->video.src_dev), "/dev/video0");
	} else {
		log_warn("BaresipManager", "/dev/video0 NOT FOUND - Fallback to 'fakevideo'");
		snprintf(cfg->video.src_mod, sizeof(cfg->video.src_mod), "fakevideo");
		if (strlen(cfg->video.src_dev) == 0 && cfg->video.src_mod[0] != '\0') {
			// If src_mod is set but src_dev is empty, might need defaults?
			// For now, ensure it's empty string explicitly if needed
			cfg->video.src_dev[0] = '\0';
		}
	}

	// Set default device to "default"
	snprintf(cfg->audio.play_dev, sizeof(cfg->audio.play_dev), "default");
	snprintf(cfg->audio.src_dev, sizeof(cfg->audio.src_dev), "default");
	snprintf(cfg->audio.alert_dev, sizeof(cfg->audio.alert_dev), "default");

	// Audio Device Tuning for Linux/ALSA
	cfg->audio.adaptive = true;
	cfg->audio.buffer.min = 40;
	cfg->audio.buffer.max = 200;
#endif

	// Ensure we use our custom display module
	re_snprintf(cfg->video.disp_mod, sizeof(cfg->video.disp_mod), "sdl_vidisp");

	cfg->video.enc_fmt = VID_FMT_YUV420P;
	cfg->video.width = 640;
	cfg->video.height = 480;
	cfg->video.fps = 30;
	cfg->video.bitrate = 1000000;

	// FIX: Enable auto-accept (allocation) of incoming calls
	// Without this, SIPSESS_CONN fires but the call object is never created!
	cfg->call.accept = true;

	// FIX: Allow significantly more calls in Core than in UI to handle "zombie"
	// calls that are resolving (BYE/200 OK) in the background.
	// UI Limit: 8 (MAX_CALLS), Core Limit: 32
	cfg->call.max_calls = 32;

	// Enable SIP Trace
	// cfg->sip.trace = true; // Error: No such member

	// Initialize Baresip core
	err = baresip_init(cfg);
	if (err) {
		log_error("BaresipManager", "Failed to initialize baresip: %d", err);
		libre_close();
		return err;
	}

	// Force Configuration Programmatically (Fix for config file parsing issues)
	// MUST be done BEFORE module loading so modules like selfview see the config!
	cfg = conf_config();
	if (cfg) {
		log_info("BaresipManager", "Forcing video configuration via buffer...");
		char force_conf[512];
		const char *vid_src = "fakevideo";
		if (access("/dev/video0", F_OK) == 0) {
			vid_src = "v4l2,/dev/video0";
		}
#ifdef __linux__
		snprintf(force_conf, sizeof(force_conf),
			 "video_selfview window\n"
			 "video_source %s\n",
			 vid_src);
#else
		snprintf(force_conf, sizeof(force_conf), "video_selfview window\n");
#endif
		conf_configure_buf((const uint8_t *)force_conf, strlen(force_conf));
	}

	struct mod *m = NULL;

	// Audio Codecs (Linked statically in QEMU build)
	mod_add(&m, &exports_opus);
	mod_add(&m, &exports_g711);

#ifdef __linux__
	// Audio Driver (ALSA)
	err = mod_add(&m, &exports_alsa);
	if (err)
		log_warn("BaresipManager", "Failed to add alsa module: %d", err);

	// Video Source (V4L2)
	err = mod_add(&m, &exports_v4l2);
	if (err)
		log_warn("BaresipManager", "Failed to add v4l2 module: %d", err);

	// Video Codecs (FFmpeg/Libav)
	err = mod_add(&m, &exports_avcodec);
	if (err)
		log_warn("BaresipManager", "Failed to add avcodec module: %d", err);

	// Selfview (Local Camera Mirror)
	err = mod_add(&m, &exports_selfview);
	if (err)
		log_warn("BaresipManager", "Failed to add selfview module: %d", err);
#endif

	// Video Display & Sources (Local exports)
	err = mod_add(&m, &exports_sdl_vidisp);
	if (err)
		log_warn("BaresipManager", "Failed to add sdl_vidisp: %d", err);

	// Window pseudo-module (if needed for resize events)
	err = mod_add(&m, &exports_window);
	if (err)
		log_warn("BaresipManager", "Failed to add window module: %d", err);

	// Video Conversion (Critical for selfview format mismatch)
	// Replaced vidconv (missing) with swscale
	err = mod_add(&m, &exports_swscale);
	if (err)
		log_warn("BaresipManager", "Failed to add swscale module: %d", err);

	// NAT modules (optional)
	// STUN/TURN/ICE disabled due to linker errors (static modules not found)
	// mod_add(&m, &exports_stun);
	// mod_add(&m, &exports_turn);
	// mod_add(&m, &exports_ice); // Linker error: undefined reference

	log_info("BaresipManager", "App Init: Static Module Loading Completed.");

	// Initialize User Agents
	err = ua_init("baresip-lvgl", true, true, true);
	if (err) {
		log_error("BaresipManager", "Failed to initialize UA: %d", err);
		baresip_close();
		libre_close();
		return err;
	}

	// Manually enable transports
	struct sa laddr_any;
	sa_init(&laddr_any, AF_INET);

	log_info("BaresipManager", "Manually adding UDP transport...");
	err = sip_transp_add(uag_sip(), SIP_TRANSP_UDP, &laddr_any);
	if (err)
		log_warn("BaresipManager", "Failed to add UDP transport: %d", err);

	log_info("BaresipManager", "Manually adding TCP transport...");
	err = sip_transp_add(uag_sip(), SIP_TRANSP_TCP, &laddr_any);
	if (err)
		log_warn("BaresipManager", "Failed to add TCP transport: %d", err);

	// Generate UUID if missing
	if (cfg && !str_isset(cfg->sip.uuid)) {
		log_info("BaresipManager", "Generating missing UUID...");
		snprintf(cfg->sip.uuid, sizeof(cfg->sip.uuid), "%08x-%04x-%04x-%04x-%08x%04x", rand(), rand() & 0xFFFF,
			 rand() & 0xFFFF, rand() & 0xFFFF, rand(), rand() & 0xFFFF);
	}

	printf("DEBUG: Pre-net_debug\n");
	fflush(stdout);

	// Debug: Print network interfaces
	log_debug("BaresipManager", "--- Network Interface Debug ---");
	net_debug(NULL, NULL);
	log_debug("BaresipManager", "------------------------------");

	printf("DEBUG: Post-net_debug\n");
	fflush(stdout);

	// Register event handler
	// Register event handler
	bevent_register(call_event_handler, NULL);

	// Initialize Messaging Subsystem
	err = message_init(&g_message);
	if (err) {
		log_error("BaresipManager", "Failed to init messaging: %d", err);
	} else {
		// Listen for Messages
		err = message_listen(g_message, (message_recv_h *)message_handler, NULL);
		if (err) {
			log_warn("BaresipManager", "Message listen FAILED: %d", err);
		}
	}

	printf("DEBUG: Post-uag_event_register\n");
	fflush(stdout);

	if (!vidisp_list_lock) {
		mutex_alloc(&vidisp_list_lock);
	}

	printf("DEBUG: Post-mutex_alloc\n");
	fflush(stdout);

	// Initialize Video Display Pointers (Fix for Black Local Video)
	// Moved to end of init to ensure modules are loaded
	vid = vidisp_find(baresip_vidispl(), "sdl_vidisp");
	vid2 = vidisp_find(baresip_vidispl(),
			   "window"); // selfview module name (registered as 'window'
	// in baresip_manager.c)

	// Double check selfview module
	if (!vid2) {
		log_warn("BaresipManager", "Could not find 'window' vidisp module via "
					   "find(). Trying manual lookup...");
		// Fallback: Check if 'window' is registered in vidisp list manually if
		// needed
	}

	if (!vid2) {
		log_warn("BaresipManager", "Could not find 'window' vidisp module (Selfview)");
	} else {
		log_info("BaresipManager", "Found 'window' vidisp module: %p", vid2);
	}
	if (!vid) {
		log_warn("BaresipManager", "Could not find 'sdl_vidisp' vidisp module (Remote)");
	} else {
		log_info("BaresipManager", "Found 'sdl_vidisp' vidisp module: %p", vid);
	}

	printf("BaresipManager: [DEBUG] Init Pointers: vid=%p, vid2=%p\n", (void *)vid, (void *)vid2);
	fflush(stdout);

	log_info("BaresipManager", "Initialization complete");
	log_info("BaresipManager", "Starting Call Watchdog...");
	tmr_start(&watchdog_tmr, 1000, check_call_watchdog, NULL);

	// Sync Accounts
	log_info("BaresipManager", "Syncing existing accounts...");
	struct le *le;
	for (le = ((struct list *)uag_list())->head; le; le = le->next) {
		struct ua *ua = le->data;
		struct account *acc = ua_account(ua);
		const char *aor = account_aor(acc);

		// Check if already registered
		reg_status_t status = ua_isregistered(ua) ? REG_STATUS_REGISTERED : REG_STATUS_NONE;
		update_account_status(aor, status);
		log_info("BaresipManager", "Synced Account: %s Status=%d", aor, status);
	}

	return 0;
}

static int sdl_vidisp_init(void)
{
	int err = 0;
	if (!vid) {
		err = vidisp_register(&vid2, baresip_vidispl(), "window", (vidisp_alloc_h *)lvgl_vidisp_alloc,
				      (vidisp_update_h *)lvgl_vidisp_update, (vidisp_disp_h *)lvgl_vidisp_disp,
				      (vidisp_hide_h *)lvgl_vidisp_hide);
		if (err) {
			log_error("BaresipManager", "Failed to register sdl_vidisp_self: %d", err);
			return err;
		}

		err = vidisp_register(&vid, baresip_vidispl(), "sdl_vidisp", (vidisp_alloc_h *)lvgl_vidisp_alloc,
				      (vidisp_update_h *)lvgl_vidisp_update, (vidisp_disp_h *)lvgl_vidisp_disp,
				      (vidisp_hide_h *)lvgl_vidisp_hide);
		if (err) {
			log_error("BaresipManager", "Failed to register sdl_vidisp: %d", err);
			return err;
		}
	}
	return err;
}

static int sdl_vidisp_close(void)
{
	vid = mem_deref(vid);
	return 0;
}

const struct mod_export exports_sdl_vidisp = {
	"sdl_vidisp",
	"vidisp",
	sdl_vidisp_init,
	sdl_vidisp_close,
};

static int window_init(void)
{
	if (vid2)
		return 0;
	int err = vidisp_register(&vid2, baresip_vidispl(), "window", lvgl_vidisp_alloc, lvgl_vidisp_update, lvgl_vidisp_disp,
				  lvgl_vidisp_hide);
	return err;
}

static int window_close(void)
{
	vid2 = mem_deref(vid2);
	return 0;
}

const struct mod_export exports_window = {
	"window",
	"vidisp",
	window_init,
	window_close,
};

void baresip_manager_set_callback(call_event_cb cb)
{
	g_call_state.callback = cb;
}

void baresip_manager_set_reg_callback(reg_event_cb cb)
{
	g_call_state.reg_callback = cb;
}

void baresip_manager_set_message_callback(message_event_cb cb)
{
	g_message_callback = cb;
}

reg_status_t baresip_manager_get_account_status(const char *aor)
{
	if (!aor)
		return REG_STATUS_NONE;
	account_status_t *acc = find_account(aor);
	return acc ? acc->status : REG_STATUS_NONE;
}

// Watchdog Handler
static void check_call_watchdog(void *arg)
{
	(void)arg;
	tmr_start(&watchdog_tmr, 1000, check_call_watchdog, NULL); // Reschedule

	// START GARBAGE COLLECTOR
	// Iterate active_calls and ensure they still exist in Core
	for (int i = 0; i < MAX_CALLS; i++) {
		if (g_call_state.active_calls[i].call) {
			struct call *c = g_call_state.active_calls[i].call;
			bool core_found = false;

			// Scan all UAs
			struct le *le_ua;
			for (le_ua = ((struct list *)uag_list())->head; le_ua && !core_found; le_ua = le_ua->next) {
				struct ua *u = le_ua->data;
				struct list *calls_list = ua_calls(u);
				struct le *le_call;
				for (le_call = list_head(calls_list); le_call; le_call = le_call->next) {
					if (le_call->data == c) {
						core_found = true;
						break;
					}
				}
			}

			if (!core_found) {
				log_warn("BaresipManager", "GC: Removing Zombie Call %p from slot %d", c, i);
				remove_call(c);
				// Check if this was current call? remove_call handles it.
			}
		}
	}
	// END GARBAGE COLLECTOR

	if (!g_call_state.current_call)
		return;

	bool found = false;
	struct le *le;
	// FIX: Iterate ALL calls for each UA, not just the primary 'ua_call()'.
	struct le *le_ua;
	for (le_ua = ((struct list *)uag_list())->head; le_ua && !found; le_ua = le_ua->next) {
		struct ua *u = le_ua->data;
		struct list *calls_list = ua_calls(u);
		struct le *le_call;
		for (le_call = list_head(calls_list); le_call; le_call = le_call->next) {
			struct call *c = le_call->data;
			if (c == g_call_state.current_call) {
				found = true;
				break;
			}
		}
	}

	if (!found) {
		log_warn("BaresipManager", "WATCHDOG: Call %p vanished without EVENT_CLOSED!", (void *)g_call_state.current_call);

		// Manually trigger close logic
		// Use stored peer URI since call object is invalid
		const char *peer = g_call_state.peer_uri;
		if (!peer || strlen(peer) == 0)
			peer = "unknown";

		call_type_t type = CALL_TYPE_OUTGOING;
		if (g_call_state.state == CALL_STATE_INCOMING) {
			type = CALL_TYPE_INCOMING;
			if (g_call_state.state != CALL_STATE_ESTABLISHED)
				type = CALL_TYPE_MISSED;
		}

		log_warn("BaresipManager", "WATCHDOG: Adding History: %s (Type=%d)", peer, type);
		fflush(stdout);
		history_add(peer, peer, type, "");

		// Update State
		struct call *dead_call = g_call_state.current_call;

		// Use remove_call to properly clean up and switch
		remove_call(dead_call);

		if (g_call_state.callback) {
			g_call_state.callback(CALL_STATE_TERMINATED, peer, NULL);
		}

		// Force UI Back if needed
		applet_t *current = applet_manager_get_current();
		if (current && strcmp(current->name, "Call") == 0) {
			log_warn("BaresipManager", "WATCHDOG: Force closing Call Applet");
			// Only back if we truly went to IDLE (remove_call might have switched)
			if (g_call_state.state == CALL_STATE_IDLE)
				applet_manager_back();
		}
	}
}

// Helper to detect all active local IPs (IPv4) - Redundant if net_debug used?
// Retaining per original code.

// Helper to auto-hold all other active calls
static void internal_hold_active_calls(struct call *exclude)
{
	for (int i = 0; i < MAX_CALLS; i++) {
		struct call *c = g_call_state.active_calls[i].call;
		// Only hold ESTABLISHED calls.
		// We might also want to hold EARLY/RINGING? No, usually you can't hold
		// those easily or it implies separate behavior. Stick to ESTABLISHED for
		// now.
		if (c && c != exclude && g_call_state.active_calls[i].state == CALL_STATE_ESTABLISHED) {
			if (!call_is_onhold(c)) {
				log_info("BaresipManager", "Auto-holding call %p (Slot %d)", c, i);
				call_hold(c, true);
				// Force update internal state immediately
				add_or_update_call(c, CALL_STATE_ESTABLISHED, NULL);
			}
		}
	}
}

static int internal_connect(const char *uri, const char *account_aor, bool video)
{
	log_info("BaresipManager", "internal_connect START: URI='%s'", uri ? uri : "NULL");
	char full_uri[256];
	struct ua *ua = NULL;
	int err;

	if (!uri)
		return -1;

	// Use current call if already active (for adding video?)
	// For now we support 1 active call switch.

	// Check valid accounts
	struct le *le_debug;
	for (le_debug = ((struct list *)uag_list())->head; le_debug; le_debug = le_debug->next) {
		struct ua *u = le_debug->data;
		(void)u;

		log_info("BaresipManager", "  - UA Available: %p", (void *)u);
	}

	// Select User Agent
	if (account_aor) {
		ua = uag_find_aor(account_aor);
	}

	if (!ua) {
		struct le *le = list_head(((struct list *)uag_list()));
		if (le)
			ua = le->data;
	}

	if (!ua) {
		log_warn("BaresipManager", "No user agent available - Cannot connect");
		return -1;
	}

	// Normalize URI
	if (strstr(uri, "sip:") == uri) {
		safe_strncpy(full_uri, uri, sizeof(full_uri));
	} else if (strchr(uri, '@')) {
		snprintf(full_uri, sizeof(full_uri), "sip:%s", uri);
	} else {
		// Just a number? Use account domain
		struct account *acc = ua_account(ua);
		const struct uri *luri = account_luri(acc);
		if (luri && luri->host.p) {
			char domain[128];
			pl_strcpy(&luri->host, domain, sizeof(domain));
			snprintf(full_uri, sizeof(full_uri), "sip:%s@%s", uri, domain);
		} else {
			snprintf(full_uri, sizeof(full_uri), "sip:%s", uri);
		}
	}
	full_uri[sizeof(full_uri) - 1] = '\0';

	// Make call
	// Auto-hold other calls
	internal_hold_active_calls(NULL);

	struct call *call = NULL;
	err = ua_connect(ua, &call, NULL, full_uri, video ? VIDMODE_ON : VIDMODE_OFF);
	if (err) {
		log_error("BaresipManager", "Failed to make call: %d", err);
		return err;
	}

	if (call) {
		g_call_state.current_call = call;
		add_or_update_call(call, CALL_STATE_OUTGOING, full_uri);
	}

	snprintf(g_call_state.peer_uri, sizeof(g_call_state.peer_uri), "%s", full_uri);
	g_call_state.state = CALL_STATE_OUTGOING;
	g_call_state.is_video_call = video;

	return 0;
}

int baresip_manager_connect(const char *uri, const char *account_aor, bool video)
{
	// FIX: QUEUE the connect command to ensure it executes AFTER any pending
	// hangup
	// NUCLEAR VIDEO FLUSH: Forcefully remove OLD video states
	// This prevents the "Flash of Old Face" on new calls
	// NUCLEAR VIDEO FLUSH: Forcefully remove OLD video states
	// This prevents the "Flash of Old Face" on new calls
	// FIX: Unlink ONLY. Do not deref (Baresip owns memory). Do not hold lock
	// while derefing (Deadlock).
	if (vidisp_list_lock) {
		mtx_lock(vidisp_list_lock);
		// Clear UI pointers first
		if (g_local_video_obj)
			lv_img_set_src(g_local_video_obj, NULL);
		if (g_remote_video_obj)
			lv_img_set_src(g_remote_video_obj, NULL);

		// Detach all video states from the display list
		struct le *le = vidisp_list.head;
		while (le) {
			struct vidisp_st *st = le->data;
			le = le->next; // advance before unlinking

			if (st) {
				st->new_frame = false;
				list_unlink(&st->le); // Detach. Baresip will free memory later.
			}
		}
		mtx_unlock(vidisp_list_lock);
		log_info("BaresipManager", "Nuclear Video Flush Complete (Detached)");
	}

	cmd_t cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.type = CMD_DIAL;
	safe_strncpy(cmd.data.dial.uri, uri, sizeof(cmd.data.dial.uri));
	if (account_aor)
		safe_strncpy(cmd.data.dial.account_aor, account_aor, sizeof(cmd.data.dial.account_aor));
	else
		cmd.data.dial.account_aor[0] = '\0';
	cmd.data.dial.video = video;

	log_info("BaresipManager", "Enqueueing DIAL command (URI=%s)", uri);
	if (!cmd_enqueue(&cmd)) {
		log_error("BaresipManager", "Failed to enqueue dial command!");
		return -1;
	}

	// OPTIMISTIC UI: Launch Call Applet immediately
	// Request Active View to override "IDLE" check in call_resume
	log_info("BaresipManager", "Optimistic UI: Launching Call Applet (Active View) immediately");
	call_applet_request_active_view();
	applet_manager_launch("Call");

	return 0;
}

int baresip_manager_call_with_account(const char *uri, const char *account_aor)
{
	return baresip_manager_connect(uri, account_aor, false);
}

int baresip_manager_call(const char *uri)
{
	return baresip_manager_connect(uri, NULL, false);
}

int baresip_manager_videocall_with_account(const char *uri, const char *account_aor)
{
	return baresip_manager_connect(uri, account_aor, true);
}

int baresip_manager_videocall(const char *uri)
{
	// NUCLEAR OPTION: Force clear any lingering video images from previous calls
	// This prevents the "flash of old face" when starting a new call.
	if (vidisp_list_lock) {
		mtx_lock(vidisp_list_lock);
		if (g_local_video_obj && lv_obj_is_valid(g_local_video_obj)) {
			lv_img_set_src(g_local_video_obj, NULL);
			lv_obj_invalidate(g_local_video_obj);
		}
		if (g_remote_video_obj && lv_obj_is_valid(g_remote_video_obj)) {
			lv_img_set_src(g_remote_video_obj, NULL);
			lv_obj_invalidate(g_remote_video_obj);
		}

		// FIX: Clear 'new_frame' flags to prevent immediate display of
		// stale (black) buffers. Force wait for fresh data.
		struct le *le;
		for (le = vidisp_list.head; le; le = le->next) {
			struct vidisp_st *st = le->data;
			if (st)
				st->new_frame = false;
		}

		mtx_unlock(vidisp_list_lock);
	}

	return baresip_manager_connect(uri, NULL, true);
}

int baresip_manager_answer_call(bool video)
{
	struct call *c = g_call_state.current_call;

	// FAILSAFE: If no current call, scan core for any incoming call
	if (!c) {
		struct le *le;
		for (le = ((struct list *)uag_list())->head; le; le = le->next) {
			struct ua *ua = le->data;
			struct call *candidate = ua_call(ua);
			if (candidate && call_state(candidate) == CALL_STATE_INCOMING) {
				c = candidate;
				g_call_state.current_call = c; // Auto-fix
				log_info("BaresipManager", "Answer: Auto-resolved missing call object %p", (void *)c);
				break;
			}
		}
	}

	if (!c) {
		log_warn("BaresipManager", "Answer: No call found to answer.");
		return -1;
	}

	// Auto-hold other calls before answering
	internal_hold_active_calls(c);

	log_info("BaresipManager", "Answer: Answering call %p with video=%d", c, video);
	int err = call_answer(c, 200, video ? VIDMODE_ON : VIDMODE_OFF);
	if (err) {
		log_warn("BaresipManager", "Answer: call_answer failed with error %d", err);
	} else {
		log_info("BaresipManager", "Answer: call_answer succeeded");
	}
	return err;
}

int baresip_manager_reject_call(void *call_ptr)
{
	struct call *c = (struct call *)call_ptr;

	// Check for Sentinel (Ghost Call) or NULL
	if (c == (void *)0xDEADBEEF || c == NULL) {
		log_warn("BaresipManager", "Reject: Received Ghost/Null pointer %p. Scanning...", call_ptr);
		c = NULL; // Reset to NULL to avoid using DEADBEEF

		// FAILSAFE: Scan core for any incoming or active call to hang up
		struct le *le;
		for (le = ((struct list *)uag_list())->head; le; le = le->next) {
			struct ua *ua = le->data;
			struct call *candidate = ua_call(ua);
			if (candidate) {
				// Prefer incoming, but take what we can get if we are desperate
				enum call_state st = call_state(candidate);
				if (st == CALL_STATE_INCOMING || st == CALL_STATE_RINGING || st == CALL_STATE_EARLY ||
				    st == CALL_STATE_ESTABLISHED) {
					c = candidate;
					log_info("BaresipManager",
						 "Reject/Hangup: Auto-resolved missing call object %p "
						 "(State %d)",
						 (void *)c, st);
					break;
				}
			}
		}
	}

	if (!c) {
		log_warn("BaresipManager", "Reject: Failed to find valid call object.");
		return -1;
	}

	log_info("BaresipManager", "Rejecting call object %p", (void *)c);
	// Mark as TERMINATED immediately to allow slot recycling
	for (int i = 0; i < MAX_CALLS; i++) {
		if (g_call_state.active_calls[i].call == c) {
			g_call_state.active_calls[i].state = CALL_STATE_TERMINATED;
			log_info("BaresipManager", "Force-set call %p in slot %d to TERMINATED (Reject)", c, i);
			break;
		}
	}

	// Using 486 (Busy Here) explicitly to ensure standard rejection
	call_hangup(c, 486, "Busy Here");

	// DEEP CLEAN: Wipe video buffers to black
	if (vidisp_list_lock) {
		mtx_lock(vidisp_list_lock);
		struct le *le;
		for (le = vidisp_list.head; le; le = le->next) {
			struct vidisp_st *st = le->data;
			if (st->lock)
				mtx_lock(st->lock);
			if (st->rgb_buf && st->rgb_buf_size > 0) {
				memset(st->rgb_buf, 0, st->rgb_buf_size);
			}
			if (st->lock)
				mtx_unlock(st->lock);

			if (st->lock)
				mtx_unlock(st->lock);

			// NOTE: Do NOT call lv_img_set_src here (Background Thread) -> Causes
			// Freeze! The buffer is zeroed, so it will show black if displayed. UI
			// cleanup is handled by main thread video loop or next call start.
		}
		mtx_unlock(vidisp_list_lock);
	}

	return 0;
}

static int internal_hangup(void)
{
	struct call *call = g_call_state.current_call;

	// FAILSAFE: If no current call, scan core for any active call to hang up
	if (!call) {
		struct le *le;
		for (le = ((struct list *)uag_list())->head; le; le = le->next) {
			struct ua *ua = le->data;
			struct call *candidate = ua_call(ua);
			if (candidate) {
				enum call_state st = call_state(candidate);
				// Prioritize Incoming/Ringing/Established
				if (st != CALL_STATE_IDLE && st != CALL_STATE_TERMINATED && st != CALL_STATE_UNKNOWN) {
					call = candidate;
					g_call_state.current_call = call; // Auto-fix
					log_info("BaresipManager", "Hangup: Auto-resolved missing call object %p (State %d)",
						 (void *)call, st);
					break;
				}
			}
		}
	}

	if (!call) {
		log_warn("BaresipManager", "Hangup: No call to hang up.");
		return -1;
	}

	log_info("BaresipManager", "Hangup: Hanging up call %p", call);

	// Mark as TERMINATED immediately to allow slot recycling
	for (int i = 0; i < MAX_CALLS; i++) {
		if (g_call_state.active_calls[i].call == call) {
			g_call_state.active_calls[i].state = CALL_STATE_TERMINATED;
			log_info("BaresipManager", "Force-set call %p in slot %d to TERMINATED (Hangup)", call, i);
			break;
		}
	}

	// Use 486 Busy Here for explicit rejection
	// 486 Busy Here for explicit rejection
	call_hangup(call, 486, "Busy Here");

	// Removed synchronous DEEP CLEAN loop to prevent lock contention and freezes.
	// The Generation Counter fix now handles cleanup safely and asynchronously.

	// Check if there are other active calls to switch to
	struct call *next_call = NULL;
	int others = 0;

	struct le *le;
	for (le = ((struct list *)uag_list())->head; le; le = le->next) {
		struct ua *u = le->data;
		struct list *calls = ua_calls(u);
		struct le *lec;
		for (lec = calls->head; lec; lec = lec->next) {
			struct call *c = lec->data;
			// Ignore the call we just hung up (pointer match) and any terminated
			// calls
			if (c && c != call && call_state(c) != CALL_STATE_TERMINATED) {
				if (!next_call)
					next_call = c;
				others++;
			}
		}
	}

	if (others > 0 && next_call) {
		log_info("BaresipManager", "Hangup: Switching to next active call %p (Total others: %d)", next_call, others);
		g_call_state.current_call = next_call;
		g_call_state.state = call_state(next_call);

		if (g_call_state.current_call) {
			safe_strncpy(g_call_state.peer_uri, call_peeruri(g_call_state.current_call), sizeof(g_call_state.peer_uri));
		}

		// Notify listeners of the switch immediately to update UI
		if (g_listener_mgr.count > 0) {
			for (int i = 0; i < g_listener_mgr.count; i++) {
				if (g_listener_mgr.listeners[i])
					g_listener_mgr.listeners[i](g_call_state.state, g_call_state.peer_uri,
								    (void *)g_call_state.current_call);
			}
		}
	} else {
		log_info("BaresipManager", "Hangup: No other calls, forcing IDLE");
		g_call_state.state = CALL_STATE_IDLE;
		g_call_state.current_call = NULL;
		g_call_state.is_video_call = false; // Reset video state on hangup

		// Force notify IDLE to ensure UI closes
		if (g_listener_mgr.count > 0) {
			for (int i = 0; i < g_listener_mgr.count; i++) {
				if (g_listener_mgr.listeners[i])
					g_listener_mgr.listeners[i](CALL_STATE_IDLE, NULL, NULL);
			}
		}
	}

	return 0;
}

int baresip_manager_hangup(void)
{
	// UI FIX: Clear "Last Picture" immediately!
	if (vidisp_list_lock) {
		mtx_lock(vidisp_list_lock);
		if (g_local_video_obj && lv_obj_is_valid(g_local_video_obj)) {
			lv_img_set_src(g_local_video_obj, NULL);
			lv_obj_invalidate(g_local_video_obj);
		}
		if (g_remote_video_obj && lv_obj_is_valid(g_remote_video_obj)) {
			lv_img_set_src(g_remote_video_obj, NULL);
			lv_obj_invalidate(g_remote_video_obj);
		}
		mtx_unlock(vidisp_list_lock);
		// FORCE REDRAW NOW to show the cleared screen
		lv_refr_now(NULL);

		// REMOVED unsafe unlock: pthread_mutex_unlock(&g_lvgl_lock);
		// We are in UI thread (usually), applet_manager_back relies on lock being
		// held.
	}

	// OPTIMISTIC UI: Force close call applet immediately
	// This makes the UI feel instant even if the driver takes 3s to close
	applet_t *current = applet_manager_get_current();
	if (current && strcmp(current->name, "Call") == 0) {
		log_info("BaresipManager", "Optimistic UI: Closing Call Applet immediately");
		applet_manager_back();
	}

	// FIX: Revert to QUEUEING to ensure serialization with DIAL commands.
	// We cleared the UI above so the user sees immediate feedback.
	// The actual heavy lifting (v4l2_close) will happen on the thread.
	cmd_t cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.type = CMD_HANGUP;

	log_info("BaresipManager", "Enqueueing HANGUP command");
	if (!cmd_enqueue(&cmd)) {
		log_error("BaresipManager", "Failed to enqueue hangup command!");
		return -1;
	}
	return 0;
}

enum call_state baresip_manager_get_state(void)
{
	return g_call_state.state;
}

bool baresip_manager_is_current_call_held(void)
{
	if (g_call_state.current_call) {
		return call_is_onhold(g_call_state.current_call);
	}
	return false;
}

const char *baresip_manager_get_peer(void)
{
	return g_call_state.peer_uri[0] ? g_call_state.peer_uri : NULL;
}

void baresip_manager_mute(bool mute)
{
	g_call_state.muted = mute;

	if (g_call_state.current_call) {
		struct audio *audio = call_audio(g_call_state.current_call);
		if (audio) {
			audio_mute(audio, mute);
		}
	}

	log_info("BaresipManager", "Microphone %s", mute ? "muted" : "unmuted");
}

int baresip_manager_account_register(const char *aor)
{
	struct ua *ua = uag_find_aor(aor);
	if (!ua) {
		log_warn("BaresipManager", "Account not found for register: %s", aor ? aor : "NULL");
		return -1;
	}
	return ua_register(ua);
}

int baresip_manager_account_register_simple(const char *user, const char *domain)
{
	if (!user || !domain)
		return -1;

	struct list *l = (struct list *)uag_list();
	struct le *le;
	for (le = l->head; le; le = le->next) {
		struct ua *ua = le->data;
		struct account *acc = ua_account(ua);
		const char *aor = account_aor(acc);

		// Check if AOR contains user and domain
		if (strstr(aor, user) && strstr(aor, domain)) {
			log_info("BaresipManager", "Found UA for simple register: %s", aor);
			return ua_register(ua);
		}
	}
	log_warn("BaresipManager", "No UA found for simple register: %s @ %s", user, domain);
	return -1;
}

// Timer callback for command processing
// Timer callback for command processing
// Timer callback for command processing
static struct tmr g_loop_tmr;
static void cmd_check_cb(void *arg)
{
	(void)arg;

	// Process ALL pending commands in queue
	while (1) {
		cmd_t cmd;
		bool has_cmd = false;

		pthread_mutex_lock(&g_cmd_mutex);
		if (g_cmd_queue.count > 0) {
			cmd = g_cmd_queue.items[g_cmd_queue.head];
			g_cmd_queue.head = (g_cmd_queue.head + 1) % CMD_QUEUE_SIZE;
			g_cmd_queue.count--;
			has_cmd = true;
		}
		pthread_mutex_unlock(&g_cmd_mutex);

		if (!has_cmd)
			break;

		// Execute Command
		switch (cmd.type) {
		case CMD_ADD_ACCOUNT:
			log_info("BaresipManager", "Processing CMD_ADD_ACCOUNT");
			internal_add_account(&cmd.data.acc);
			break;
		case CMD_SEND_MESSAGE:
			log_info("BaresipManager", "Processing CMD_SEND_MESSAGE");
			internal_send_message(cmd.data.msg.peer, cmd.data.msg.text);
			break;
		case CMD_HANGUP:
			log_info("BaresipManager", "Processing CMD_HANGUP");
			internal_hangup();
			break;
		case CMD_DIAL:
			log_info("BaresipManager", "Processing CMD_DIAL");
			internal_connect(cmd.data.dial.uri,
					 strlen(cmd.data.dial.account_aor) > 0 ? cmd.data.dial.account_aor : NULL,
					 cmd.data.dial.video);
			break;

		default:
			break;
		}
	}

	// Reschedule
	tmr_start(&g_loop_tmr, 20, cmd_check_cb, NULL);
}

// UI Timer
static struct tmr g_ui_tmr;
static void (*g_ui_cb)(void) = NULL;
static int g_ui_interval = 5;

static void ui_timer_cb(void *arg)
{
	(void)arg;
	if (g_ui_cb) {
		g_ui_cb();
	}
	tmr_start(&g_ui_tmr, g_ui_interval, ui_timer_cb, NULL);
}

void baresip_manager_loop(void (*ui_cb)(void), int interval_ms)
{
	int err;

	// Initialize manager
	// Removed redundant init call: err = baresip_manager_init();

	printf("BaresipManager: Loop Starting... Interval=%dms\n", interval_ms);
	fflush(stdout);

	// Start SIP User Agent
	printf("BaresipManager: Starting UA...\n");
	fflush(stdout);
	// log_info("BaresipManager", "Starting UA...");

	printf("BaresipManager: Starting Main Loop...\n");
	fflush(stdout);
	// log_info("BaresipManager", "Starting Main Loop...");
	tmr_init(&g_loop_tmr);

	// Start Command Check Timer
	tmr_start(&g_loop_tmr, 50, cmd_check_cb, NULL);

	// Start UI Timer
	if (ui_cb && interval_ms > 0) {
		g_ui_cb = ui_cb;
		g_ui_interval = interval_ms;
		tmr_init(&g_ui_tmr);
		tmr_start(&g_ui_tmr, interval_ms, ui_timer_cb, NULL);
		printf("BaresipManager: UI Timer started (Interval: %dms)\n", interval_ms);
	}

	// Run main loop (Blocks until re_cancel or error)
	err = re_main(signal_handler);

	if (err) {
		printf("BaresipManager: re_main exited with error: %d\n", err);
	} else {
		printf("BaresipManager: re_main exited normally\n");
	}

	tmr_cancel(&g_ui_tmr);
	tmr_cancel(&g_loop_tmr);

	baresip_close();
	libre_close();
}

// Inserting missing functions here

bool baresip_manager_is_muted(void)
{
	return g_call_state.muted;
}

// Thread-safe wrapper
int baresip_manager_add_account(const voip_account_t *acc)
{
	if (!acc)
		return -1;

	cmd_t cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.type = CMD_ADD_ACCOUNT;
	cmd.data.acc = *acc;

	// We cannot easily check if queue is full and return error synchronously
	// that means "try again". But for now, we log error.
	if (!cmd_enqueue(&cmd)) {
		log_error("BaresipManager", "Command Queue Full! Failed to enqueue account add.");
		return -1;
	}

	return 0; // Success (Pending)
}

// Internal function (runs in Baresip Thread)
static int internal_add_account(const voip_account_t *acc)
{
	char aor[2048];
	int err;

	if (!acc)
		return EINVAL;

	/* Format: "Display Name"
   * <sip:user:password@domain;transport=tcp>;regint=3600
   */
	/* Simplified for now: <sip:user:password@domain>;transport=tcp */

	char transport_param[32];
	if (strlen(acc->transport) > 0) {
		snprintf(transport_param, sizeof(transport_param), ";transport=%s", acc->transport);
	} else {
		strcpy(transport_param, ";transport=udp");
	}

	// Handle Custom Port
	char server_with_port[150];
	if (acc->port > 0 && acc->port != 5060) {
		snprintf(server_with_port, sizeof(server_with_port), "%s:%d", acc->server, acc->port);
	} else {
		snprintf(server_with_port, sizeof(server_with_port), "%s", acc->server);
	}

	// Prepare Media NAT Parameter
	char mnat_param[256] = ""; // Increased size

	if (acc->use_ice) {
		strcat(mnat_param, ";mnat=ice");
	}

	if (strlen(acc->stun_server) > 0) {
		// Check if it's TURN or STUN
		if (strncmp(acc->stun_server, "turn:", 5) == 0) {
			char buf[128];
			snprintf(buf, sizeof(buf), ";outbound=\"%s\"",
				 acc->stun_server); // Quote for safety
			strcat(mnat_param, buf);
		} else {
			// Assume STUN
			// Baresip AOR parameter for STUN server override is not standard
			// but we can try adding it as a custom parameter or rely on global.
			// However, user asked to use it.
			// Let's try adding it as ;stunserver=<uri>
			char buf[128];
			snprintf(buf, sizeof(buf), ";stunserver=\"%s\"", acc->stun_server);
			strcat(mnat_param, buf);
		}
	}

	if (strlen(acc->outbound_proxy) > 0) {
		// With Outbound Proxy
		// Format:
		// <sip:user@domain;transport=tcp>;auth_pass=pass;auth_user=user;outbound=sip:proxy;regint=600

		char proxy_buf[256];
		if (strstr(acc->outbound_proxy, "sip:") == acc->outbound_proxy) {
			snprintf(proxy_buf, sizeof(proxy_buf), "%s", acc->outbound_proxy);
		} else {
			snprintf(proxy_buf, sizeof(proxy_buf), "sip:%s", acc->outbound_proxy);
		}

		const char *a_user = (strlen(acc->auth_user) > 0) ? acc->auth_user : acc->username;

		snprintf(aor, sizeof(aor), "<sip:%s@%s%s>;auth_pass=%s;auth_user=%s;outbound=%s;regint=%d%s", acc->username,
			 server_with_port, transport_param, acc->password, a_user, proxy_buf, acc->reg_interval, mnat_param);
	} else {
		// Direct Registration
		snprintf(aor, sizeof(aor), "<sip:%s:%s@%s%s>;regint=%d%s", acc->username, acc->password, server_with_port,
			 transport_param, acc->reg_interval, mnat_param);
	}

	// Add display name if present
	if (strlen(acc->display_name) > 0) {
		char temp[4096];
		snprintf(temp, sizeof(temp), "\"%s\" %s", acc->display_name, aor);
		strcpy(aor, temp);
	}

	printf("TEST_MATCH_MGR: Adding account: %s\n", aor);

	struct ua *ua = NULL;
	err = ua_alloc(&ua, aor);
	if (err) {
		log_error("BaresipManager", "Failed to allocate UA: %d", err);
		return err;
	}

	// Auto-register
	// Auto-register
	if (ua) {
		log_info("BaresipManager", "UA Allocated. Enabled: %d", acc->enabled);
		if (acc->enabled) {
			log_info("BaresipManager", "Triggering Registration for %s", aor);
			err = ua_register(ua);
			if (err) {
				log_warn("BaresipManager", "Failed to register UA: %d", err);
			} else {
				log_info("BaresipManager", "Registration triggered successfully");
			}
		}

		// Update our internal tracking list
		const char *final_aor = account_aor(ua_account(ua));
		update_account_status(final_aor, REG_STATUS_NONE);
	}

	return 0;
}

// --- Active Calls and Call Control ---

int baresip_manager_get_active_calls(call_info_t *calls, int max_count)
{
	int count = 0;

	// No fallback logic needed: active_calls array is the source of truth.

	// Iterate our managed active_calls array
	for (int i = 0; i < MAX_CALLS && count < max_count; i++) {
		if (g_call_state.active_calls[i].call != NULL) {
			calls[count].id = (void *)g_call_state.active_calls[i].call;
			safe_strncpy(calls[count].peer_uri, g_call_state.active_calls[i].peer_uri, sizeof(calls[count].peer_uri));

			calls[count].state = g_call_state.active_calls[i].state;

			// Check hold status from cache (Non-blocking)
			calls[count].is_held = g_call_state.active_calls[i].is_held;

			calls[count].is_current = (g_call_state.active_calls[i].call == g_call_state.current_call);
			count++;
		}
	}
	return count;
}

int baresip_manager_send_dtmf(char key)
{
	if (!g_call_state.current_call)
		return -1;
	return call_send_digit(g_call_state.current_call, key);
}

int baresip_manager_transfer(const char *target)
{
	if (!g_call_state.current_call) {
		log_warn("BaresipManager", "Transfer: No active call");
		return -1;
	}
	log_info("BaresipManager", "Transferring active call to %s", target);
	return call_transfer(g_call_state.current_call, target);
}

// Ensure correct thread safe call (usually called from main thread)
int baresip_manager_hold_call(void *call_id)
{
	struct call *call = (struct call *)call_id;
	if (!call)
		call = g_call_state.current_call;

	if (!call) {
		log_warn("BaresipManager", "No call to hold");
		return -1;
	}
	log_info("BaresipManager", "Holding call %p", call);

	// Update cache
	for (int i = 0; i < MAX_CALLS; i++) {
		if (g_call_state.active_calls[i].call == call) {
			g_call_state.active_calls[i].is_held = true;
			break;
		}
	}

	return call_hold(call, true);
}

int baresip_manager_resume_call(void *call_id)
{
	struct call *call = (struct call *)call_id;
	if (!call)
		call = g_call_state.current_call;

	if (!call) {
		log_warn("BaresipManager", "No call to resume");
		return -1;
	}

	// Hold other calls first
	for (int i = 0; i < MAX_CALLS; i++) {
		struct call *c = g_call_state.active_calls[i].call;
		if (c && c != call && !call_is_onhold(c)) {
			log_info("BaresipManager", "Holding existing call %p before resuming %p", (void *)c, (void *)call);
			call_hold(c, true);
		}
	}

	log_info("BaresipManager", "Resuming call %p", (void *)call);

	// Update cache
	for (int i = 0; i < MAX_CALLS; i++) {
		if (g_call_state.active_calls[i].call == call) {
			g_call_state.active_calls[i].is_held = false;
			break;
		}
	}

	return call_hold(call, false);
}

// Switch to specific call
int baresip_manager_switch_to(void *call_id)
{
	if (!call_id)
		return -1;
	struct call *call = (struct call *)call_id;

	log_info("BaresipManager", "Switching current call to %p", call);
	g_call_state.current_call = call;
	g_call_state.state = call_state(call);
	safe_strncpy(g_call_state.peer_uri, call_peeruri(call), sizeof(g_call_state.peer_uri));

	// Notify listeners so UI updates immediately
	if (g_listener_mgr.count > 0) {
		for (int i = 0; i < g_listener_mgr.count; i++) {
			if (g_listener_mgr.listeners[i])
				g_listener_mgr.listeners[i](g_call_state.state, g_call_state.peer_uri, (void *)call);
		}
	}
	return 0;
}

void baresip_manager_set_log_level(log_level_t level)
{
	enum log_level b_level;
	int dbg_level;

	// Map App Log Level -> Baresip Log Level
	switch (level) {
	// case LOG_LEVEL_TRACE: // Assumes LOG_LEVEL_* are defined in logger.h
	//   b_level = LEVEL_DEBUG;
	//   dbg_level = DBG_DEBUG; // 7
	//   break;
	case LOG_LEVEL_DEBUG:
		b_level = LEVEL_DEBUG;
		dbg_level = DBG_DEBUG; // 7
		break;
	case LOG_LEVEL_INFO:
		b_level = LEVEL_INFO;
		dbg_level = DBG_INFO; // 6
		break;
	case LOG_LEVEL_WARN:
		b_level = LEVEL_WARN;
		dbg_level = DBG_WARNING; // 4
		break;
	case LOG_LEVEL_ERROR:
		// case LOG_LEVEL_FATAL:
		b_level = LEVEL_ERROR;
		dbg_level = DBG_ERR; // 3
		break;
	default:
		b_level = LEVEL_INFO;
		dbg_level = DBG_INFO;
		break;
	}

	// log_info usage note: ensure it is inside function.
	// log_info("BaresipManager", "Setting Baresip Log Level to %d", b_level);

	// Set Baresip Log Level
	log_level_set(b_level);

	// Set re library Debug Level
	dbg_init(dbg_level, DBG_ALL);
}

void baresip_manager_destroy(void)
{
	ua_stop_all(false);
	ua_close();
	baresip_close();
	libre_close();
}

void baresip_manager_get_peer_display_name(struct call *call, const char *peer_uri, char *out_buf, size_t size)
{
	if (!out_buf || size == 0)
		return;
	out_buf[0] = '\0';

	if (!peer_uri && call)
		peer_uri = call_peeruri(call);
	if (!peer_uri)
		return;

	// 1. Try Contact DB
	char contact_name[256];
	// Need to parse URI to get number first?
	// db_contact_find expects "sip:user@domain" or just "user"?
	// Typically contacts stored as "sip:..." in DB?
	// Let's try full URI first.
	if (db_contact_find(peer_uri, contact_name, sizeof(contact_name)) == 0) {
		log_info("BaresipManager", "Display Name: Found in DB: '%s'", contact_name);
		safe_strncpy(out_buf, contact_name, size);
		return;
	}

	// Try cleaning URI (users often store just number)
	char user[64];
	struct pl pl_uri;
	struct uri uri;
	pl_set_str(&pl_uri, peer_uri);
	if (uri_decode(&uri, &pl_uri) == 0) {
		pl_strcpy(&uri.user, user, sizeof(user));
		if (db_contact_find(user, contact_name, sizeof(contact_name)) == 0) {
			log_info("BaresipManager", "Display Name: Found in DB (User): '%s'", contact_name);
			safe_strncpy(out_buf, contact_name, size);
			return;
		}
	} else {
		// Fallback for decode fail
		snprintf(user, sizeof(user), "%s", peer_uri);
	}

	// 2. Try Display Name
	if (call) {
		const char *dn = call_peername(call);
		if (dn && strlen(dn) > 0) {
			log_info("BaresipManager", "Display Name: Using SIP Header: '%s'", dn);
			safe_strncpy(out_buf, dn, size);
			return;
		}
	}

	// 3. Fallback to formatted URI
	// Format: user@domain or just user
	if (strlen(user) > 0) {
		safe_strncpy(out_buf, user, size);
		log_info("BaresipManager", "Display Name: Fallback to User: '%s'", user);
	} else {
		// Just copy URI
		safe_strncpy(out_buf, peer_uri, size);
		log_info("BaresipManager", "Display Name: Fallback to connected URI: '%s'", peer_uri);
	}
}

void baresip_manager_get_current_call_display_name(char *out_buf, size_t size)
{
	if (g_call_state.current_call) {
		baresip_manager_get_peer_display_name(g_call_state.current_call, g_call_state.peer_uri, out_buf, size);
	} else {
		if (out_buf && size > 0)
			out_buf[0] = '\0';
	}
}

// Internal function (runs on Baresip Thread)
static int internal_send_message(const char *peer_uri, const char *text)
{
	log_info("BaresipManager", "internal_send_message: START peer='%s'", peer_uri);

	struct ua *ua = NULL;
	struct list *l = (struct list *)uag_list();
	if (l && l->head) {
		ua = l->head->data;
	}

	if (!ua) {
		log_warn("BaresipManager", "No UA available to send message");
		return -1;
	}
	log_info("BaresipManager", "internal_send_message: UA found: %p", ua);

	// Format URI
	char final_uri[512];
	if (strchr(peer_uri, '@')) {
		log_info("BaresipManager", "internal_send_message: Using full URI");
		const char *p = peer_uri;
		if (strncmp(p, "sip:", 4) != 0 && strncmp(p, "sips:", 5) != 0) {
			snprintf(final_uri, sizeof(final_uri), "sip:%s", peer_uri);
		} else {
			safe_strncpy(final_uri, peer_uri, sizeof(final_uri));
		}
	} else {
		log_info("BaresipManager", "internal_send_message: Appending domain");
		// Append domain from UA
		struct account *acc = ua_account(ua);
		if (!acc) {
			log_error("BaresipManager", "internal_send_message: UA has no account!");
			return -1;
		}
		const char *aor = account_aor(acc);
		log_info("BaresipManager", "internal_send_message: AOR='%s'", aor);

		char domain[128];
		domain[0] = '\0';

		const char *at = strchr(aor, '@');
		if (at) {
			const char *semi = strchr(at, ';');
			size_t len = semi ? (size_t)(semi - (at + 1)) : strlen(at + 1);
			if (len >= sizeof(domain))
				len = sizeof(domain) - 1;
			snprintf(domain, sizeof(domain), "%.*s", (int)len, at + 1);
			domain[len] = '\0';
		}

		if (strlen(domain) > 0) {
			snprintf(final_uri, sizeof(final_uri), "sip:%s@%s", peer_uri, domain);
		} else {
			snprintf(final_uri, sizeof(final_uri), "sip:%s", peer_uri);
		}
	}

	log_info("BaresipManager", "internal_send_message: Final URI='%s'", final_uri);
	log_info("BaresipManager", "Sending MESSAGE... text len=%zu", strlen(text));

	printf("DEBUG_STEP: internal_send_message: Calling message_send...\n");
	fflush(stdout);
	// message_send takes (ua, peer, msg, resp_handler, arg)
	int err = message_send(ua, final_uri, text, NULL, NULL);
	printf("DEBUG_STEP: internal_send_message: message_send returned: %d\n", err);
	fflush(stdout);

	if (err) {
		log_error("BaresipManager", "Failed to send message: %d", err);
		return err;
	}

	log_info("BaresipManager", "internal_send_message: Message sent successfully. Saving to DB...");
	printf("DEBUG_STEP: internal_send_message: Calling db_chat_add...\n");
	fflush(stdout);
	db_chat_add(final_uri, 1, text);
	printf("DEBUG_STEP: internal_send_message: db_chat_add returned.\n");
	fflush(stdout);

	log_info("BaresipManager", "internal_send_message: Saved to DB. DONE.");
	return 0;
}

// Public API (runs on Main Thread)
int baresip_manager_send_message(const char *peer_uri, const char *text)
{
	if (!peer_uri) {
		log_error("BaresipManager", "send_message: peer_uri is NULL");
		return -1;
	}
	if (!text) {
		log_error("BaresipManager", "send_message: text is NULL");
		return -1;
	}
	log_info("BaresipManager", "enqueue message to='%s' text='%s'", peer_uri, text);

	cmd_t cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.type = CMD_SEND_MESSAGE;
	safe_strncpy(cmd.data.msg.peer, peer_uri, sizeof(cmd.data.msg.peer));
	safe_strncpy(cmd.data.msg.text, text, sizeof(cmd.data.msg.text));

	if (!cmd_enqueue(&cmd)) {
		log_error("BaresipManager", "Command Queue Full! Failed to enqueue message.");
		return -1;
	}
	return 0;
}

// Expose Local Video DSC for thread-safe UI binding
void *baresip_manager_get_local_video_dsc(void)
{
	struct le *le;
	if (!vidisp_list_lock)
		return NULL;

	// Optimism Guard: If call is ended, do not return video objects as they are
	// likely being freed.
	if (g_call_state.state == CALL_STATE_IDLE || g_call_state.state == CALL_STATE_TERMINATED)
		return NULL;

	mtx_lock(vidisp_list_lock);
	for (le = vidisp_list.head; le; le = le->next) {
		struct vidisp_st *st = le->data;
		if (st->is_local) {
			mtx_unlock(vidisp_list_lock);
			return (void *)&st->img_dsc;
		}
	}
	mtx_unlock(vidisp_list_lock);
	return NULL;
}
