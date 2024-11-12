#ifndef _VAAPI_JPEG_DECODER_H
#define _VAAPI_JPEG_DECODER_H 1

#include <X11/Xlib.h>
#include <memory>
#include <string>
#include <va/va.h>

struct Frame;

std::string get_usable_va_display();

void init_jpeg_vaapi();
std::shared_ptr<Frame> decode_jpeg_vaapi(const std::string &jpeg);

extern bool vaapi_jpeg_decoding_usable;

#endif  // !defined(_VAAPI_JPEG_DECODER_H)
