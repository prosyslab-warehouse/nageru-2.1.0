#include "va_display.h"
#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_x11.h>

#include <string>
#include <vector>

using namespace std;

VADisplayWithCleanup::~VADisplayWithCleanup()
{
	if (va_dpy != nullptr) {
		vaTerminate(va_dpy);
	}
	if (x11_display != nullptr) {
		XCloseDisplay(x11_display);
	}
	if (drm_fd != -1) {
		close(drm_fd);
	}
}

unique_ptr<VADisplayWithCleanup> va_open_display(const string &va_display)
{
	if (va_display.empty() || va_display[0] != '/') {  // An X display.
		Display *x11_display = XOpenDisplay(va_display.empty() ? nullptr : va_display.c_str());
		if (x11_display == nullptr) {
			fprintf(stderr, "error: can't connect to X server!\n");
			return nullptr;
		}

		unique_ptr<VADisplayWithCleanup> ret(new VADisplayWithCleanup);
		ret->x11_display = x11_display;
		ret->can_use_zerocopy = true;
		ret->va_dpy = vaGetDisplay(x11_display);
		if (ret->va_dpy == nullptr) {
			return nullptr;
		}
		return ret;
	} else {  // A DRM node on the filesystem (e.g. /dev/dri/renderD128).
		int drm_fd = open(va_display.c_str(), O_RDWR);
		if (drm_fd == -1) {
			perror(va_display.c_str());
			return NULL;
		}
		unique_ptr<VADisplayWithCleanup> ret(new VADisplayWithCleanup);
		ret->drm_fd = drm_fd;
		ret->can_use_zerocopy = false;
		ret->va_dpy = vaGetDisplayDRM(drm_fd);
		if (ret->va_dpy == nullptr) {
			return nullptr;
		}
		return ret;
	}
}

unique_ptr<VADisplayWithCleanup> try_open_va(
	const string &va_display, const vector<VAProfile> &desired_profiles, VAEntrypoint entrypoint,
	const vector<ConfigRequest> &desired_configs, VAProfile *chosen_profile, string *error)
{
	unique_ptr<VADisplayWithCleanup> va_dpy = va_open_display(va_display);
	if (va_dpy == nullptr) {
		if (error) *error = "Opening VA display failed";
		return nullptr;
	}
	int major_ver, minor_ver;
	VAStatus va_status = vaInitialize(va_dpy->va_dpy, &major_ver, &minor_ver);
	if (va_status != VA_STATUS_SUCCESS) {
		char buf[256];
		snprintf(buf, sizeof(buf), "vaInitialize() failed with status %d\n", va_status);
		if (error != nullptr) *error = buf;
		return nullptr;
	}

	int num_entrypoints = vaMaxNumEntrypoints(va_dpy->va_dpy);
	unique_ptr<VAEntrypoint[]> entrypoints(new VAEntrypoint[num_entrypoints]);
	if (entrypoints == nullptr) {
		if (error != nullptr) *error = "Failed to allocate memory for VA entry points";
		return nullptr;
	}

	// Try the profiles from highest to lowest until we find one that can be used.
	VAProfile found_profile = VAProfileNone;
	for (VAProfile profile : desired_profiles) {
		vaQueryConfigEntrypoints(va_dpy->va_dpy, profile, entrypoints.get(), &num_entrypoints);
		for (int slice_entrypoint = 0; slice_entrypoint < num_entrypoints; slice_entrypoint++) {
			if (entrypoints[slice_entrypoint] != entrypoint) {
				continue;
			}

			// We found a usable encoder/decoder, so return it.
			if (chosen_profile != nullptr) {
				*chosen_profile = profile;
			}
			found_profile = profile;
			break;
		}
		if (found_profile != VAProfileNone) {
			break;
		}
	}
	if (!found_profile) {
		if (error != nullptr) *error = "Can't find entry points for suitable codec profile";
		return nullptr;
	}

	int num_formats = vaMaxNumImageFormats(va_dpy->va_dpy);
	assert(num_formats > 0);

	unique_ptr<VAImageFormat[]> formats(new VAImageFormat[num_formats]);
	va_status = vaQueryImageFormats(va_dpy->va_dpy, formats.get(), &num_formats);
	if (va_status != VA_STATUS_SUCCESS) {
		char buf[256];
		snprintf(buf, sizeof(buf), "vaQueryImageFormats() failed with status %d\n", va_status);
		if (error != nullptr) *error = buf;
		return nullptr;
	}

	for (const ConfigRequest &request : desired_configs) {
		// Create the config.
		VAConfigAttrib attr = { VAConfigAttribRTFormat, request.rt_format };
		va_status = vaCreateConfig(va_dpy->va_dpy, found_profile, entrypoint,
			&attr, 1, request.config_id);
		if (va_status == VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT) {
			if (error != nullptr) *error = "No " + request.name + " hardware support";
			return nullptr;
		} else if (va_status != VA_STATUS_SUCCESS) {
			char buf[256];
			snprintf(buf, sizeof(buf), "vaCreateConfig() for %s failed with status %d\n", request.name.c_str(), va_status);
			if (error != nullptr) *error = buf;
			return nullptr;
		}

		// Find out which image format we're going to be using.
		bool format_found = false;
		for (int i = 0; i < num_formats; ++i) {
			if (formats[i].fourcc == request.fourcc) {
				memcpy(request.image_format, &formats[i], sizeof(VAImageFormat));
				format_found = true;
				break;
			}
		}
		if (!format_found) {
			if (error != nullptr) *error = "Format for " + request.name + " not found";
			return nullptr;
		}
	}

	return va_dpy;
}
