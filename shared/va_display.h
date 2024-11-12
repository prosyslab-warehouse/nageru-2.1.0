#ifndef _VA_DISPLAY_H
#define _VA_DISPLAY _H1

#include <va/va.h>
#include <X11/Xlib.h>

#include <memory>
#include <string>
#include <vector>

struct VADisplayWithCleanup {
	~VADisplayWithCleanup();

	VADisplay va_dpy;
	Display *x11_display = nullptr;
	bool can_use_zerocopy = true;  // For H.264 encoding in Nageru.
	int drm_fd = -1;
};

struct ConfigRequest {
        std::string name;  // For error texts only.
        uint32_t rt_format, fourcc;

        // Output.
        VAConfigID *config_id;
        VAImageFormat *image_format;
};
std::unique_ptr<VADisplayWithCleanup> try_open_va(
        const std::string &va_display, const std::vector<VAProfile> &desired_profiles, VAEntrypoint entrypoint,
        const std::vector<ConfigRequest> &desired_configs, VAProfile *chosen_profile, std::string *error);  // Can return nullptr on failure.

#endif  // !defined(_VA_DISPLAY_H)
