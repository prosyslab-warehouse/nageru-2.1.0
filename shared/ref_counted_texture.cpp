#include "ref_counted_texture.h"

#include <epoxy/gl.h>
#include <movit/util.h>

RefCountedTexture create_texture_2d(GLuint width, GLuint height, GLenum internal_format, GLenum format, GLenum type, const GLvoid *pixels)
{
	GLuint tex;
	glCreateTextures(GL_TEXTURE_2D, 1, &tex);
	check_error();
	glTextureStorage2D(tex, 1, internal_format, width, height);
	check_error();
	glTextureSubImage2D(tex, 0, 0, 0, width, height, format, type, pixels);
	check_error();
	glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	check_error();
	glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	return RefCountedTexture(new GLuint(tex), TextureDeleter());
}
