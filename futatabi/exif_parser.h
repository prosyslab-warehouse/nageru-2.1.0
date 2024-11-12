#ifndef _EXIF_PARSER_H
#define _EXIF_PARSER_H

#include <movit/effect.h>
#include <string>

class Frame;

// Try to parse the WhitePoint tag in the given Exif data.
// If the string is empty, or the tag is corrupted, or if it was
// just more complicated than our makeshift parser could deal with,
// returns (1.0, 1.0, 1.0), giving a regular D65 white point.
movit::RGBTriplet get_neutral_color(const std::string &exif);

#endif  // !defined(_EXIF_PARSER_H)

