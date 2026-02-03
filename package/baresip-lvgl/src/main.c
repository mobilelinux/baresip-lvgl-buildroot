#include "applet_manager.h"
#include "baresip_manager.h"
#include "config_manager.h" // Added for config_manager_init and config_load_app_settings
#include "history_manager.h"
#include "logger.h"
#include "lv_drivers/sdl/sdl.h"
#include "lvgl.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h> // Added for memset
#include <sys/time.h>
#include <unistd.h>
#ifdef USE_FBDEV
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"
#include "lv_drivers/indev/evdev_kb.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#endif
#include <libudev.h>
#include <pthread.h>
#include <stdlib.h>

// Forward declarations for applet registration functions
extern void home_applet_register(void);
extern void settings_applet_register(void);
extern void calculator_applet_register(void);
extern void call_applet_register(void);
extern void contacts_applet_register(void);
extern void call_log_applet_register(void);
extern void chat_applet_register(void);
extern void about_applet_register(void);

// Get current time in milliseconds
static uint32_t get_tick_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

// Global for loop callback
static uint32_t last_tick = 0;
#ifndef USE_FBDEV
extern volatile bool sdl_quit_qry;
#endif

// Global UI Lock and Thread
pthread_mutex_t g_lvgl_lock = PTHREAD_MUTEX_INITIALIZER;
// Global Baresip Thread
static pthread_t g_baresip_thread;
static volatile bool g_baresip_thread_running = false;
static volatile bool g_baresip_ready = false;
static volatile bool app_quit_qry = false;

// Baresip Loop Callback (Background Thread) - Only process video frames
static void baresip_loop_cb(void)
{
#ifndef USE_FBDEV
	if (sdl_quit_qry) {
		re_cancel();
		return;
	}
#endif

	// Process Video Frames
	pthread_mutex_lock(&g_lvgl_lock);
	baresip_manager_process_video();
	pthread_mutex_unlock(&g_lvgl_lock);
}

static void *baresip_thread_func(void *arg)
{
	(void)arg;
	log_info("Main", "Baresip Thread Started");

	// Initialize Baresip Manager HERE (Background Thread)
	// This ensures libre/networking is initialized on the same thread as the
	// event loop
	if (baresip_manager_init() != 0) {
		log_error("Main", "Failed to initialize Baresip Manager in background thread");
		g_baresip_thread_running = false;
		return NULL;
	}

	// Signal Main Thread that we are ready
	g_baresip_ready = true;

	// Start Baresip main loop with UI callback (Video processing only)
	last_tick = get_tick_ms();
	baresip_manager_loop(baresip_loop_cb, 5); // 5ms interval for video updates

	log_info("Main", "Baresip Thread Stopped");
	return NULL;
}

// Baresip Loop Callback (Main Thread) - Only process video frames
static void ui_loop_cb(void)
{
#ifndef USE_FBDEV
	if (sdl_quit_qry) {
		re_cancel();
		return;
	}
#endif

	// Process Video Frames (Locked internally or we lock here?)
	// We will lock around process_video inside baresip_manager.c
	// OR lock here? Let's lock here to be safe and consistent.
	// Actually process_video logic is complex with locks inside.
	// Better to wrap the call here.

	pthread_mutex_lock(&g_lvgl_lock);
	baresip_manager_process_video();
	pthread_mutex_unlock(&g_lvgl_lock);
}

#ifdef USE_FBDEV
// Helper to find input device by property
static char *find_input_device_by_prop(const char *prop_name)
{
	struct udev *udev = udev_new();
	if (!udev)
		return NULL;

	struct udev_enumerate *enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(enumerate, "input");
	udev_enumerate_add_match_property(enumerate, prop_name, "1");
	udev_enumerate_scan_devices(enumerate);

	struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
	struct udev_list_entry *entry;
	char *dev_node = NULL;

	udev_list_entry_foreach(entry, devices)
	{
		const char *path = udev_list_entry_get_name(entry);
		struct udev_device *dev = udev_device_new_from_syspath(udev, path);
		if (dev) {
			const char *node = udev_device_get_devnode(dev);
			if (node) {
				// Check if it has "event" in name to avoid mouse0 etc if we want event
				// interface
				if (strstr(node, "event")) {
					dev_node = strdup(node);
					udev_device_unref(dev);
					break;
				}
			}
			udev_device_unref(dev);
		}
	}

	udev_enumerate_unref(enumerate);
	udev_unref(udev);
	return dev_node;
}
#endif

// Initialize LVGL display
static int init_display(void)
{
	lv_init();

#ifdef USE_FBDEV

	// FBDEV Initialization
	fbdev_init();

	// DEBUG: Query VINFO to diagnose color depth/format
	int fbfd_dbg = open("/dev/fb0", O_RDWR);
	if (fbfd_dbg != -1) {
		struct fb_var_screeninfo vinfo;
		struct fb_fix_screeninfo finfo;
		ioctl(fbfd_dbg, FBIOGET_FSCREENINFO, &finfo);
		if (ioctl(fbfd_dbg, FBIOGET_VSCREENINFO, &vinfo) == 0) {
			log_info("Main", ">>> FBDEV DETECTED: %dx%d, %dbpp, STRIDE: %d <<<", vinfo.xres, vinfo.yres,
				 vinfo.bits_per_pixel, finfo.line_length);
			log_info("Main", ">>> FBDEV RGB: R(%d/%d) G(%d/%d) B(%d/%d) TRANS(%d/%d) <<<", vinfo.red.offset,
				 vinfo.red.length, vinfo.green.offset, vinfo.green.length, vinfo.blue.offset, vinfo.blue.length,
				 vinfo.transp.offset, vinfo.transp.length);
		} else {
			log_error("Main", ">>> FBDEV IOCTL FAILED <<<");
		}
		close(fbfd_dbg);
	}

	// Create display buffer (Double buffering or partial)
	static lv_disp_draw_buf_t disp_buf;
	// Use small buffer for FBDEV relative to resolution? Or full?
	// Existing code used 100 lines for SDL. Let's stick to it.
	const size_t buf_size = 800 * 480 * sizeof(lv_color_t) / 10; // Approx 1/10th screen?
// Hardcoded for now or use macro if available. SDL_HOR_RES might be SDL
// specific. Re-use SDL_HOR_RES if defined, or define defaults.
#ifndef SDL_HOR_RES
#define SDL_HOR_RES 800
#endif
#ifndef SDL_VER_RES
#define SDL_VER_RES 480
#endif

	lv_color_t *buf1 = malloc(SDL_HOR_RES * 100 * sizeof(lv_color_t));
	lv_color_t *buf2 = NULL; // Single buffer for FBDEV usually enough unless double buffered

	if (!buf1) {
		log_error("Main", "Failed to allocate display buffer");
		return -1;
	}

	lv_disp_draw_buf_init(&disp_buf, buf1, buf2, SDL_HOR_RES * 100);

	// Initialize and register display driver
	static lv_disp_drv_t disp_drv;
	lv_disp_drv_init(&disp_drv);
	disp_drv.draw_buf = &disp_buf;
	disp_drv.flush_cb = fbdev_flush;
	disp_drv.hor_res = SDL_HOR_RES;
	disp_drv.ver_res = SDL_VER_RES;
	// disp_drv.screen_transp = 1; // Not supported on standard FBDEV usually

	lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
	if (!disp) {
		log_error("Main", "Failed to register display driver");
		return -1;
	}

	// Initialize EVDEV input (Mouse/Touch)
	char *mouse_dev = find_input_device_by_prop("ID_INPUT_TOUCHSCREEN");
	if (!mouse_dev)
		mouse_dev = find_input_device_by_prop("ID_INPUT_MOUSE");

	if (mouse_dev) {
		log_info("Main", "Auto-detected Pointer Device: %s", mouse_dev);
		evdev_init();
		evdev_set_file(mouse_dev);
		free(mouse_dev);
	} else {
		log_warn("Main", "No Pointer Device detected, fallback to auto-detect");
		evdev_init();
	}

	static lv_indev_drv_t indev_drv;
	lv_indev_drv_init(&indev_drv);
	indev_drv.type = LV_INDEV_TYPE_POINTER;
	indev_drv.read_cb = evdev_read;
	lv_indev_drv_register(&indev_drv);

	// Initialize EVDEV Keypad
	char *kb_dev = find_input_device_by_prop("ID_INPUT_KEYBOARD");
	if (kb_dev) {
		log_info("Main", "Auto-detected Keyboard Device: %s", kb_dev);
		evdev_kb_init();
		evdev_kb_set_file(kb_dev);
		free(kb_dev);
	} else {
		log_warn("Main", "No Keyboard Device detected, fallback to auto-detect");
		evdev_kb_init();
	}

	static lv_indev_drv_t indev_drv_kb;
	lv_indev_drv_init(&indev_drv_kb);
	indev_drv_kb.type = LV_INDEV_TYPE_KEYPAD;
	indev_drv_kb.read_cb = evdev_kb_read; // Use KB specific read
	// indev_drv_kb.user_data = ...;
	lv_indev_t *kb_indev = lv_indev_drv_register(&indev_drv_kb);

	// Create a group for keyboard input and assign it to the device
	lv_group_t *g = lv_group_create();
	lv_group_set_default(g);
	lv_indev_set_group(kb_indev, g);

#else
	// SDL Initialization
	sdl_init();

	// Create display buffer
	static lv_disp_draw_buf_t disp_buf;
	const size_t buf_size = SDL_HOR_RES * 100 * sizeof(lv_color_t);

	lv_color_t *buf1 = malloc(buf_size);
	lv_color_t *buf2 = malloc(buf_size);

	if (!buf1 || !buf2) {
		log_error("Main", "Failed to allocate display buffers");
		return -1;
	}

	lv_disp_draw_buf_init(&disp_buf, buf1, buf2, SDL_HOR_RES * 100);

	// Initialize and register display driver
	static lv_disp_drv_t disp_drv;
	lv_disp_drv_init(&disp_drv);
	disp_drv.draw_buf = &disp_buf;
	disp_drv.flush_cb = sdl_display_flush;
	disp_drv.hor_res = SDL_HOR_RES;
	disp_drv.ver_res = SDL_VER_RES;
	disp_drv.screen_transp = 1;

	lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
	if (!disp) {
		log_error("Main", "Failed to register display driver");
		return -1;
	}

	// Initialize mouse input device
	static lv_indev_drv_t indev_drv_mouse;
	lv_indev_drv_init(&indev_drv_mouse);
	indev_drv_mouse.type = LV_INDEV_TYPE_POINTER;
	indev_drv_mouse.read_cb = sdl_mouse_read;
	lv_indev_drv_register(&indev_drv_mouse);
#endif

#ifndef USE_FBDEV
	// Initialize keyboard input device
	static lv_indev_drv_t indev_drv_kb;
	lv_indev_drv_init(&indev_drv_kb);
	indev_drv_kb.type = LV_INDEV_TYPE_KEYPAD;
	indev_drv_kb.read_cb = sdl_keyboard_read;
	lv_indev_t *kb_indev = lv_indev_drv_register(&indev_drv_kb);

	// Create a group for keyboard input and assign it to the device
	lv_group_t *g = lv_group_create();
	lv_group_set_default(g);
	lv_indev_set_group(kb_indev, g);

	// Initialize mousewheel input device
	static lv_indev_drv_t indev_drv_wheel;
	lv_indev_drv_init(&indev_drv_wheel);
	indev_drv_wheel.type = LV_INDEV_TYPE_ENCODER;
	indev_drv_wheel.read_cb = sdl_mousewheel_read;
	lv_indev_drv_register(&indev_drv_wheel);
#endif

	log_info("Main", "LVGL display initialized with SDL2 (%dx%d)", SDL_HOR_RES, SDL_VER_RES);
	return 0;
}

int main(void)
{
	log_info("Main", "=== LVGL Applet Manager with SDL2 ===");

	// Initialize LVGL display
	if (init_display() != 0) {
		log_error("Main", "Failed to initialize display");
		return 1;
	}

	// Initialize config manager
	config_manager_init();

	// Load config to set log level early
	app_config_t config;
	memset(&config, 0, sizeof(app_config_t));
	if (config_load_app_settings(&config) == 0) {
		logger_init(config.log_level);
		baresip_manager_set_log_level(config.log_level);
	} else {
		logger_init(LOG_LEVEL_INFO);
		baresip_manager_set_log_level(LOG_LEVEL_INFO);
	}

	// Baresip Manager Init MOVED to background thread
	// to ensure thread affinity with re_main loop

	// Initialize applet manager
	if (applet_manager_init() != 0) {
		log_error("Main", "Failed to initialize applet manager");
		goto cleanup;
	}

	// Register all applets
	log_info("Main", "Registering applets...");
	home_applet_register();
	settings_applet_register();
	calculator_applet_register();
	call_applet_register();
	contacts_applet_register();
	call_log_applet_register();
	chat_applet_register();
	about_applet_register();

	// Start Baresip Thread EARLY (Before Applets)
	// We need Baresip initialized before "Call" applet tries to use it
	log_info("Main", "Starting Baresip Thread...");
	g_baresip_thread_running = true;
	if (pthread_create(&g_baresip_thread, NULL, baresip_thread_func, NULL) != 0) {
		log_error("Main", "Failed to create Baresip Thread");
		return 1;
	}

	// Wait for Baresip to be ready
	log_info("Main", "Waiting for Baresip Initialization...");
	while (!g_baresip_ready && g_baresip_thread_running) {
		usleep(10000); // 10ms wait
	}
	if (!g_baresip_thread_running) {
		log_error("Main", "Baresip Thread failed to start");
		return 1;
	}
	log_info("Main", "Baresip Initialized!");

	// Force initialization of Call applet to start background SIP services
	log_info("Main", "Initializing background services...");
	int count = 0;
	applet_t **applets = applet_manager_get_all(&count);
	if (applets) {
		for (int i = 0; i < count; i++) {
			if (applets[i] && applets[i]->name && strcmp(applets[i]->name, "Call") == 0) {
				if (applets[i]->callbacks.init) {
					// Manually create screen to satisfy applet_manager requirements
					if (!applets[i]->screen) {
						applets[i]->screen = lv_obj_create(NULL);
					}

					if (applets[i]->callbacks.init(applets[i]) != 0) {
						log_error("Main", "Failed to initialize Call applet background services");
						// Cleanup on failure
						if (applets[i]->screen) {
							lv_obj_del(applets[i]->screen);
							applets[i]->screen = NULL;
						}
					} else {
						// Mark as PAUSED so applet_manager knows it's initialized but
						// background
						applets[i]->state = APPLET_STATE_PAUSED;
						log_info("Main", "Call applet background services initialized");
					}
				}
				break;
			}
		}
	}

	// Launch home screen
	log_info("Main", "Launching home screen...");
	if (applet_manager_launch("Home") != 0) {
		log_error("Main", "Failed to launch home screen");
		return 1;
	}

	log_info("Main", "=== Applet Manager Running ===");
	log_info("Main", "Use mouse to interact with the UI");
	log_info("Main", "Press ESC or close window to exit");

	// Baresip Thread already started above

	// Run UI Loop on Main Thread (Required for SDL)
	log_info("Main", "Starting UI Loop on Main Thread");
#ifndef USE_FBDEV
	while (!sdl_quit_qry) {
#else
	while (!app_quit_qry) {
#endif
		pthread_mutex_lock(&g_lvgl_lock);

		// Update LVGL tick
		uint32_t current_tick = get_tick_ms();
		if (last_tick == 0)
			last_tick = current_tick;

		uint32_t elapsed = current_tick - last_tick;
		if (elapsed > 0) {
			lv_tick_inc(elapsed);
			last_tick = current_tick;
		}

		// Handle LVGL tasks (Input + Rendering)
		lv_timer_handler();

		pthread_mutex_unlock(&g_lvgl_lock);

		// Sleep to prevent CPU hogging (~5ms interval)
		usleep(5000);
	}

	// Stop Baresip Thread
	log_info("Main", "Stopping Baresip Thread...");
	baresip_manager_hangup(); // Try to cleanup if needed (optional)
	re_cancel(); // Signal Baresip loop to exit
	g_baresip_thread_running = false;
	pthread_join(g_baresip_thread, NULL);

cleanup:
	log_info("Main", "=== Shutting down ===");

	// Cleanup
	applet_manager_destroy();

	log_info("Main", "Applet Manager exited successfully!");
	return 0;
}
