#include "exif_parser.h"

#include <movit/colorspace_conversion_effect.h>
#include <stdint.h>
#include <Eigen/Core>
#include <Eigen/LU>

using namespace Eigen;
using namespace movit;
using namespace std;

uint32_t read32be(const uint8_t *data)
{
	return (uint32_t(data[0]) << 24) |
		(uint32_t(data[1]) << 16) |
		(uint32_t(data[2]) <<  8) |
		 uint32_t(data[3]);
}

uint16_t read16be(const uint8_t *data)
{
	return (uint16_t(data[0]) << 8) | uint16_t(data[1]);
}

RGBTriplet get_neutral_color(const string &exif)
{
	if (exif.empty()) {
		return {1.0f, 1.0f, 1.0f};
	}

	const uint8_t *data = reinterpret_cast<const uint8_t *>(exif.data());

	// Very rudimentary Exif parser (and probably integer-overflowable);
	// we really only care about what Nageru sends us (MJPEGEncoder::init_jpeg_422()),
	// but it would be nice to have a little bit of future-proofing, just in case.
	if (exif.size() < 14 || memcmp(data, "Exif\0\0MM\0\x2a", 10) != 0) {
		fprintf(stderr, "WARNING: Truncated or malformed Exif header, ignoring.\n");
		return {1.0f, 1.0f, 1.0f};
	}

	// We only care about the first IFD.
	uint32_t ifd_offset = read32be(data + 10);
	ifd_offset += 6;  // Relative to the MM.

	if (ifd_offset < 14 || ifd_offset >= exif.size()) {
		fprintf(stderr, "WARNING: Truncated or malformed Exif IFD, ignoring.\n");
		return {1.0f, 1.0f, 1.0f};
	}

	// Skip over number of tags (16 bits); if the white point is not the first one,
	// we're bailing anyway.
	if (ifd_offset + 2 > exif.size() || ifd_offset + 2 < ifd_offset) {
		fprintf(stderr, "WARNING: Exif IFD has no rom for number of tags, ignoring.\n");
		return {1.0f, 1.0f, 1.0f};
	}

	if (ifd_offset + 4 > exif.size() || ifd_offset + 4 < ifd_offset) {
		fprintf(stderr, "WARNING: Exif IFD has no rom for tag, ignoring.\n");
		return {1.0f, 1.0f, 1.0f};
	}
	uint16_t tag = read16be(data + ifd_offset + 2);
	if (tag != 0x13e) {  // WhitePoint.
		fprintf(stderr, "WARNING: Unexpected first Exif tag, ignoring.\n");
		return {1.0f, 1.0f, 1.0f};
	}

	if (ifd_offset + 14 > exif.size() || ifd_offset + 14 < ifd_offset) {
		fprintf(stderr, "WARNING: WhitePoint Exif tag was truncated, ignoring.\n");
		return {1.0f, 1.0f, 1.0f};
	}

	// Just assume we're rational type and two values...
	uint32_t white_point_offset = read32be(data + ifd_offset + 10);
	white_point_offset += 6;  // Relative to the MM.

	if (white_point_offset >= exif.size()) {
		fprintf(stderr, "WARNING: WhitePoint Exif tag was out of bounds, ignoring.\n");
		return {1.0f, 1.0f, 1.0f};
	}
	if (white_point_offset + 16 > exif.size()) {
		fprintf(stderr, "WARNING: WhitePoint Exif tag was truncated, ignoring.\n");
		return {1.0f, 1.0f, 1.0f};
	}

	uint32_t x_nom = read32be(data + white_point_offset);
	uint32_t x_den = read32be(data + white_point_offset + 4);
	uint32_t y_nom = read32be(data + white_point_offset + 8);
	uint32_t y_den = read32be(data + white_point_offset + 12);

	double x = double(x_nom) / x_den;
	double y = double(y_nom) / y_den;
	double z = 1.0 - x - y;

	Matrix3d rgb_to_xyz_matrix = movit::ColorspaceConversionEffect::get_xyz_matrix(COLORSPACE_sRGB);
	Vector3d rgb = rgb_to_xyz_matrix.inverse() * Vector3d(x, y, z);

	return RGBTriplet(rgb[0], rgb[1], rgb[2]);
}

