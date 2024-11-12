#include <QGL>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurface>
#include <QSurfaceFormat>
#include <stdio.h>
#include <string>

QGLWidget *global_share_widget = nullptr;

using namespace std;

QSurface *create_surface()
{
	QSurfaceFormat fmt;
	fmt.setDepthBufferSize(0);
	fmt.setStencilBufferSize(0);
	fmt.setProfile(QSurfaceFormat::CoreProfile);
	fmt.setMajorVersion(4);
	fmt.setMinorVersion(5);
	fmt.setSwapInterval(0);
	QOffscreenSurface *surface = new QOffscreenSurface;
	surface->setFormat(fmt);
	surface->create();
	if (!surface->isValid()) {
		fprintf(stderr, "ERROR: surface not valid!\n");
		abort();
	}
	return surface;
}

QSurface *create_surface(const QSurfaceFormat &format)
{
	QOffscreenSurface *surface = new QOffscreenSurface;
	surface->setFormat(format);
	surface->create();
	if (!surface->isValid()) {
		fprintf(stderr, "ERROR: surface not valid!\n");
		abort();
	}
	return surface;
}

QSurface *create_surface_with_same_format(const QSurface *surface)
{
	return create_surface(surface->format());
}

QOpenGLContext *create_context(const QSurface *surface)
{
	QOpenGLContext *context = new QOpenGLContext;
	context->setShareContext(global_share_widget->context()->contextHandle());

	// Qt has a bug (QTBUG-76299) where, when using EGL, the surface ignores
	// the requested OpenGL context version and just becomes 2.0. Mesa honors
	// this and gives us a 3.0 compatibility context, but then has a bug related
	// to its shader cache (Mesa bug #110872) that causes spurious linker failures
	// when we need to re-link a Movit shader in the same context. However,
	// the surface itself doesn't use the OpenGL version in its format for anything,
	// so we can just override it and get a proper context.
	QSurfaceFormat fmt = surface->format();
	fmt.setProfile(QSurfaceFormat::CoreProfile);
	fmt.setMajorVersion(4);
	fmt.setMinorVersion(5);
	context->setFormat(fmt);

	context->create();
	return context;
}

bool make_current(QOpenGLContext *context, QSurface *surface)
{
	return context->makeCurrent(surface);
}

void delete_context(QOpenGLContext *context)
{
	delete context;
}
