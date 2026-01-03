################################################################################
#
# baresip-lvgl
#
################################################################################

BARESIP_LVGL_VERSION = 8bb9abf7d45be5cbe3c96c7a4320cc9dde72dae5
BARESIP_LVGL_SITE = https://github.com/mobilelinux/baresip-lvgl.git
BARESIP_LVGL_SITE_METHOD = git
BARESIP_LVGL_LICENSE = Apache-2.0
BARESIP_LVGL_LICENSE_FILES = LICENSE
BARESIP_LVGL_DEPENDENCIES = openssl zlib opus sqlite

# GIT_SUBMODULES disabled because .gitmodules is missing/broken in the repo.
# We must manually fetch dependencies (re, rem, baresip) into deps/
define BARESIP_LVGL_PREPARE_SOURCE
	rm -rf $(@D)/deps/baresip $(@D)/deps/re $(@D)/deps/rem $(@D)/lvgl $(@D)/lv_drivers
	git clone --depth 1 --branch v2.12.0 https://github.com/baresip/baresip.git $(@D)/deps/baresip
	git clone --depth 1 --branch v2.12.0 https://github.com/baresip/re.git $(@D)/deps/re
	git clone --depth 1 --branch v2.12.0 https://github.com/baresip/rem.git $(@D)/deps/rem
	git clone --depth 1 --branch v8.3.11 https://github.com/lvgl/lvgl.git $(@D)/lvgl
	git clone --depth 1 --branch v8.3.0 https://github.com/lvgl/lv_drivers.git $(@D)/lv_drivers
	
	# Configure lv_drivers for FBDEV
	cp package/baresip-lvgl/lv_drv_conf.h $(@D)/lv_drivers/lv_drv_conf.h
	cp package/baresip-lvgl/lv_drv_conf.h $(@D)/lvgl/lv_drv_conf.h
	cp package/baresip-lvgl/lv_drv_conf.h $(@D)/lv_drv_conf.h
	cp package/baresip-lvgl/lv_conf.h $(@D)/lv_conf.h
	cp package/baresip-lvgl/lv_conf.h $(@D)/lvgl/lv_conf.h
	
	# Replace main.c with FBDEV version
	cp package/baresip-lvgl/main_fbdev.c $(@D)/src/main.c
	
	# Replace applets with modified versions
	cp package/baresip-lvgl/src/applets/call_applet.c $(@D)/src/applets/call_applet.c
	cp package/baresip-lvgl/settings_applet.c $(@D)/src/applets/settings_applet.c
	
	# Copy fake SDL header to include dir where it's found reliably
	cp package/baresip-lvgl/fake_sdl/SDL.h $(@D)/include/SDL.h
	
	# Use provided CMakeLists.txt (rewritten for in-tree support)
	cp package/baresip-lvgl/CMakeLists.txt $(@D)/CMakeLists.txt

	# Patch dependencies to bypass find_package (overridden/satisfied by CMakeLists.txt variables)
	sed -i 's/find_package(RE/# find_package(RE/g' $(@D)/deps/rem/CMakeLists.txt
	sed -i 's/find_package(re/# find_package(re/g' $(@D)/deps/rem/CMakeLists.txt
	
	sed -i 's/find_package(RE/# find_package(RE/g' $(@D)/deps/baresip/CMakeLists.txt
	sed -i 's/find_package(re/# find_package(re/g' $(@D)/deps/baresip/CMakeLists.txt
	sed -i 's/find_package(REM/# find_package(REM/g' $(@D)/deps/baresip/CMakeLists.txt
	sed -i 's/find_package(REM/# find_package(REM/g' $(@D)/deps/baresip/CMakeLists.txt
	sed -i 's/find_package(rem/# find_package(rem/g' $(@D)/deps/baresip/CMakeLists.txt
	
	# Force disable FFMPEG to prevent avcodec crash
	sed -i 's/find_package(FFMPEG/# find_package(FFMPEG/g' $(@D)/deps/baresip/CMakeLists.txt
	sed -i 's/find_package(FFmpeg/# find_package(FFmpeg/g' $(@D)/deps/baresip/CMakeLists.txt
	
	# Force disable FFMPEG and SDL2 variables
	sed -i '1i set(FFMPEG_FOUND FALSE CACHE BOOL "Force Disable" FORCE)' $(@D)/deps/baresip/CMakeLists.txt
	sed -i '1i set(FFmpeg_FOUND FALSE CACHE BOOL "Force Disable" FORCE)' $(@D)/deps/baresip/CMakeLists.txt
	sed -i '1i set(SDL2_FOUND FALSE CACHE BOOL "Force Disable" FORCE)' $(@D)/deps/baresip/CMakeLists.txt
	sed -i '1i set(ALSA_FOUND FALSE CACHE BOOL "Force Disable" FORCE)' $(@D)/deps/baresip/CMakeLists.txt
	
	# Remove unsafe paths (cross-compilation fix)
	sed -i '/\/usr\/local\/include/d' $(@D)/deps/baresip/CMakeLists.txt
	sed -i '/link_directories(\/usr\/local\/lib)/d' $(@D)/deps/baresip/CMakeLists.txt
	
	# Fix implicit declaration in conf.c
	sed -i '1i #include <unistd.h>' $(@D)/deps/baresip/src/conf.c

	# Patch baresip_manager.c: Replace with custom version
	cp package/baresip-lvgl/baresip_manager.c $(@D)/src/manager/baresip_manager.c
	
	# Patch config_manager.c: Replace with custom version (Fixes comments parsing)
	cp package/baresip-lvgl/config_manager.c $(@D)/src/manager/config_manager.c

	# Patch history_manager.c: Replace with custom version
	cp package/baresip-lvgl/history_manager.c $(@D)/src/manager/history_manager.c
	cp package/baresip-lvgl/call_log_applet.c $(@D)/src/applets/call_log_applet.c

	# Patch database_manager.c: Replace with custom version
	cp package/baresip-lvgl/database_manager.c $(@D)/src/manager/database_manager.c

	# Fix baresip.h missing includes (critical for v2.12.0)
	sed -i '1i #include <stdint.h>\n#include <stddef.h>\n#include <re.h>' $(@D)/deps/baresip/include/baresip.h

	# Remove avcodec module to prevent build (causes crash on QEMU)
	rm -rf $(@D)/deps/baresip/modules/avcodec
	# Patch modules.cmake to prevent adding avcodec to MODULES list
	sed -i '/if(FFMPEG_FOUND)/,/endif()/d' $(@D)/deps/baresip/cmake/modules.cmake
	
	# Force disable tests in baresip/CMakeLists.txt (Fixes mod_table link error)
	sed -i 's/add_subdirectory(test)/# add_subdirectory(test)/g' $(@D)/deps/baresip/CMakeLists.txt
	
	# Replace evdev.c with custom auto-detection version
	cp package/baresip-lvgl/evdev.c $(@D)/lv_drivers/indev/evdev.c
endef
BARESIP_LVGL_POST_EXTRACT_HOOKS += BARESIP_LVGL_PREPARE_SOURCE

# CMake package
BARESIP_LVGL_INSTALL_STAGING = YES

define BARESIP_LVGL_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 package/baresip-lvgl/S99baresip-lvgl \
		$(TARGET_DIR)/etc/init.d/S99baresip-lvgl
endef

$(eval $(cmake-package))
