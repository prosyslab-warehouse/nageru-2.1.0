#ifndef _REF_COUNTED_TEXTURE_H
#define _REF_COUNTED_TEXTURE_H 1

// A wrapper around an OpenGL texture that is automatically deleted.

#include <epoxy/gl.h>
#include <memory>

struct TextureDeleter {
	void operator() (GLuint *tex)
	{
		glDeleteTextures(1, tex);
		delete tex;
	}
};

typedef std::unique_ptr<GLuint, TextureDeleter> UniqueTexture;
typedef std::shared_ptr<GLuint> RefCountedTexture;

// TODO: consider mipmaps.
RefCountedTexture create_texture_2d(GLuint width, GLuint height, GLenum internal_format, GLenum format, GLenum type, const GLvoid *pixels);

#endif  // !defined(_REF_COUNTED_TEXTURE)
