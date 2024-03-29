m4_define([weston_major_version], [1])
m4_define([weston_minor_version], [0])
m4_define([weston_micro_version], [90])
m4_define([weston_version],
          [weston_major_version.weston_minor_version.weston_micro_version])

AC_PREREQ([2.64])
AC_INIT([weston],
        [weston_version],
        [https://bugs.freedesktop.org/enter_bug.cgi?product=Wayland&component=weston&version=weston_version],
        [weston],
        [http://wayland.freedesktop.org/])

AC_SUBST([WESTON_VERSION_MAJOR], [weston_major_version])
AC_SUBST([WESTON_VERSION_MINOR], [weston_minor_version])
AC_SUBST([WESTON_VERSION_MICRO], [weston_micro_version])
AC_SUBST([WESTON_VERSION], [weston_version])

AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([1.11 parallel-tests foreign no-dist-gzip dist-xz color-tests])

AM_SILENT_RULES([yes])

# Check for programs
AC_PROG_CC
AC_PROG_SED

# Initialize libtool
LT_PREREQ([2.2])
LT_INIT([disable-static])

AC_ARG_VAR([WESTON_NATIVE_BACKEND],
           [Set the native backend to use, if Weston is not running under Wayland nor X11. @<:@default=drm-backend.so@:>@])

PKG_PROG_PKG_CONFIG()

AC_CHECK_FUNC([dlopen], [],
              AC_CHECK_LIB([dl], [dlopen], DLOPEN_LIBS="-ldl"))
AC_SUBST(DLOPEN_LIBS)

AC_CHECK_HEADERS([execinfo.h])

AC_CHECK_FUNCS([mkostemp strchrnul])

COMPOSITOR_MODULES="wayland-server xkbcommon pixman-1"

AC_ARG_ENABLE(egl, [  --disable-egl],,
              enable_egl=yes)
AM_CONDITIONAL(ENABLE_EGL, test x$enable_egl = xyes)
if test x$enable_egl = xyes; then
	AC_DEFINE([ENABLE_EGL], [1], [Build Weston with EGL support])
	COMPOSITOR_MODULES="$COMPOSITOR_MODULES egl >= 7.10 glesv2"
fi

PKG_CHECK_MODULES(COMPOSITOR, [$COMPOSITOR_MODULES])

AC_ARG_ENABLE(setuid-install, [  --enable-setuid-install],,
	      enable_setuid_install=yes)
AM_CONDITIONAL(ENABLE_SETUID_INSTALL, test x$enable_setuid_install = xyes)


AC_ARG_ENABLE(xwayland, [  --enable-xwayland],,
	      enable_xwayland=yes)
AC_ARG_ENABLE(xwayland-test, [  --enable-xwayland-test],,
              enable_xwayland_test=yes)
AM_CONDITIONAL(ENABLE_XWAYLAND, test x$enable_xwayland = xyes)
AM_CONDITIONAL(ENABLE_XWAYLAND_TEST, test x$enable_xwayland = xyes -a x$enable_xwayland_test = xyes)
if test x$enable_xwayland = xyes; then
  PKG_CHECK_MODULES([XWAYLAND], xcb xcb-xfixes xcursor cairo-xcb)
  AC_DEFINE([BUILD_XWAYLAND], [1], [Build the X server launcher])

  AC_ARG_WITH(xserver-path, AS_HELP_STRING([--with-xserver-path=PATH],
              [Path to X server]), [XSERVER_PATH="$withval"],
              [XSERVER_PATH="$bindir/Xorg"])
  AC_SUBST([XSERVER_PATH])
  if test x$enable_xwayland_test = xyes; then
    PKG_CHECK_MODULES([XWAYLAND_TEST], xcb xcb-dri2 libdrm)
  fi
fi


AC_ARG_ENABLE(x11-compositor, [  --enable-x11-compositor],,
	      enable_x11_compositor=yes)
AM_CONDITIONAL(ENABLE_X11_COMPOSITOR, test x$enable_x11_compositor = xyes)
if test x$enable_x11_compositor = xyes; then
  PKG_CHECK_MODULES([XCB], xcb)
  xcb_save_LIBS=$LIBS
  xcb_save_CFLAGS=$CFLAGS
  CFLAGS=$XCB_CFLAGS
  LIBS=$XCB_LIBS
  AC_CHECK_FUNCS([xcb_poll_for_queued_event])
  LIBS=$xcb_save_LIBS
  CFLAGS=$xcb_save_CFLAGS

  X11_COMPOSITOR_MODULES="x11 x11-xcb xcb-shm"

  PKG_CHECK_MODULES(X11_COMPOSITOR_XKB, [xcb-xkb],
		    [have_xcb_xkb="yes"], [have_xcb_xkb="no"])
  if test "x$have_xcb_xkb" = xyes; then
	# Most versions of XCB have totally broken XKB bindings, where the
	# events don't work.  Make sure we can actually use them.
	xcb_xkb_save_CFLAGS=$CFLAGS
	CFLAGS=$X11_COMPOSITOR_XKB_CFLAGS
	AC_CHECK_MEMBER([struct xcb_xkb_state_notify_event_t.xkbType],
			[], [have_xcb_xkb=no], [[#include <xcb/xkb.h>]])
	CFLAGS=$xcb_xkb_save_CFLAGS
  fi
  if test "x$have_xcb_xkb" = xyes; then
	X11_COMPOSITOR_MODULES="$X11_COMPOSITOR_MODULES xcb-xkb"
	AC_DEFINE([HAVE_XCB_XKB], [1], [libxcb supports XKB protocol])
  fi

  PKG_CHECK_MODULES(X11_COMPOSITOR, [$X11_COMPOSITOR_MODULES])
  AC_DEFINE([BUILD_X11_COMPOSITOR], [1], [Build the X11 compositor])
fi


AC_ARG_ENABLE(drm-compositor, [  --enable-drm-compositor],,
	      enable_drm_compositor=yes)
AM_CONDITIONAL(ENABLE_DRM_COMPOSITOR, test x$enable_drm_compositor = xyes -a x$enable_egl = xyes)
if test x$enable_drm_compositor = xyes -a x$enable_egl = xyes; then
  AC_DEFINE([BUILD_DRM_COMPOSITOR], [1], [Build the DRM compositor])
  PKG_CHECK_MODULES(DRM_COMPOSITOR, [libudev >= 136 libdrm >= 2.4.30 gbm mtdev >= 1.1.0])
fi


AC_ARG_ENABLE(wayland-compositor, [  --enable-wayland-compositor],,
	      enable_wayland_compositor=yes)
AM_CONDITIONAL(ENABLE_WAYLAND_COMPOSITOR,
	       test x$enable_wayland_compositor = xyes -a x$enable_egl = xyes)
if test x$enable_wayland_compositor = xyes -a x$enable_egl = xyes; then
  AC_DEFINE([BUILD_WAYLAND_COMPOSITOR], [1],
	    [Build the Wayland (nested) compositor])
  PKG_CHECK_MODULES(WAYLAND_COMPOSITOR, [wayland-client wayland-egl])
fi


AC_ARG_ENABLE(headless-compositor, [  --enable-headless-compositor],,
	      enable_headless_compositor=yes)
AM_CONDITIONAL(ENABLE_HEADLESS_COMPOSITOR,
	       test x$enable_headless_compositor = xyes)


AC_ARG_ENABLE(rpi-compositor,
	      AS_HELP_STRING([--disable-rpi-compositor],
	                     [do not build the Raspberry Pi backend]),,
	      enable_rpi_compositor=yes)
AM_CONDITIONAL(ENABLE_RPI_COMPOSITOR, test "x$enable_rpi_compositor" = "xyes" -a "x$enable_egl" = "xyes")
have_bcm_host="no"
if test x$enable_rpi_compositor = xyes -a x$enable_egl = xyes; then
  AC_DEFINE([BUILD_RPI_COMPOSITOR], [1], [Build the compositor for Raspberry Pi])
  PKG_CHECK_MODULES(RPI_COMPOSITOR, [libudev >= 136 mtdev >= 1.1.0])
  PKG_CHECK_MODULES(RPI_BCM_HOST, [bcm_host],
                    [have_bcm_host="yes"
                     AC_DEFINE([HAVE_BCM_HOST], [1], [have Raspberry Pi BCM headers])],
                    [AC_MSG_WARN([Raspberry Pi BCM host libraries not found, will use stubs instead.])])
fi
AM_CONDITIONAL(INSTALL_RPI_COMPOSITOR, test "x$have_bcm_host" = "xyes")


AC_ARG_ENABLE([fbdev-compositor], [  --enable-fbdev-compositor],,
              enable_fbdev_compositor=yes)
AM_CONDITIONAL([ENABLE_FBDEV_COMPOSITOR],
               [test x$enable_fbdev_compositor = xyes])
AS_IF([test x$enable_fbdev_compositor = xyes], [
  AC_DEFINE([BUILD_FBDEV_COMPOSITOR], [1], [Build the fbdev compositor])
  PKG_CHECK_MODULES([FBDEV_COMPOSITOR], [libudev >= 136 mtdev >= 1.1.0])
])

AC_ARG_ENABLE([rdp-compositor], [  --enable-rdp-compositor],,
              enable_rdp_compositor=no)
AM_CONDITIONAL([ENABLE_RDP_COMPOSITOR],
               [test x$enable_rdp_compositor = xyes])
if test x$enable_rdp_compositor = xyes; then
  PKG_CHECK_MODULES(RDP_COMPOSITOR, [freerdp >= 1.1.0])
fi

AC_ARG_WITH(cairo-glesv2,
            AS_HELP_STRING([--with-cairo-glesv2],
                           [Use GLESv2 cairo instead of full GL]))
if test "x$with_cairo_glesv2" = "xyes"; then
  cairo_modules="cairo-glesv2"
  AC_DEFINE([USE_CAIRO_GLESV2], [1], [Use the GLESv2 GL cairo backend])
else
  cairo_modules="cairo-gl"
fi

PKG_CHECK_MODULES(PIXMAN, [pixman-1])
PKG_CHECK_MODULES(PNG, [libpng])
PKG_CHECK_MODULES(WEBP, [libwebp], [have_webp=yes], [have_webp=no])
AS_IF([test "x$have_webp" = "xyes"],
      [AC_DEFINE([HAVE_WEBP], [1], [Have webp])])

AC_CHECK_LIB([jpeg], [jpeg_CreateDecompress], have_jpeglib=yes)
if test x$have_jpeglib = xyes; then
  JPEG_LIBS="-ljpeg"
else
  AC_ERROR([libjpeg not found])
fi
AC_SUBST(JPEG_LIBS)

PKG_CHECK_MODULES(CAIRO, [cairo])

AC_ARG_ENABLE(simple-clients,
              AS_HELP_STRING([--disable-simple-clients],
                             [do not build the simple wl_shm clients]),,
              enable_simple_clients=yes)
AM_CONDITIONAL(BUILD_SIMPLE_CLIENTS, test "x$enable_simple_clients" = "xyes")
if test x$enable_simple_clients = xyes; then
  PKG_CHECK_MODULES(SIMPLE_CLIENT, [wayland-client])
fi

AC_ARG_ENABLE(simple-egl-clients,
              AS_HELP_STRING([--disable-simple-egl-clients],
                             [do not build the simple EGL clients]),,
              enable_simple_egl_clients=yes)
AM_CONDITIONAL(BUILD_SIMPLE_EGL_CLIENTS, test "x$enable_simple_egl_clients" = "xyes" -a "x$enable_egl" = "xyes")
if test x$enable_simple_egl_clients = xyes -a x$enable_egl = xyes; then
  PKG_CHECK_MODULES(SIMPLE_EGL_CLIENT,
                    [egl >= 7.10 glesv2 wayland-client wayland-egl wayland-cursor])
fi

AC_ARG_ENABLE(clients, [  --enable-clients],, enable_clients=yes)
AM_CONDITIONAL(BUILD_CLIENTS, test x$enable_clients = xyes)
if test x$enable_clients = xyes; then
  AC_DEFINE([BUILD_CLIENTS], [1], [Build the Wayland clients])

  PKG_CHECK_MODULES(CLIENT, [wayland-client cairo >= 1.10.0 xkbcommon wayland-cursor])
  PKG_CHECK_MODULES(WESTON_INFO, [wayland-client])

  PKG_CHECK_MODULES(POPPLER, [poppler-glib glib-2.0 gobject-2.0 gio-2.0 ],
			     [have_poppler=yes], [have_poppler=no])
  PKG_CHECK_MODULES(CAIRO_EGL, [wayland-egl egl >= 7.10 cairo-egl >= 1.11.3 $cairo_modules],
		    [have_cairo_egl=yes], [have_cairo_egl=no])
  AS_IF([test "x$have_cairo_egl" = "xyes"],
        [AC_DEFINE([HAVE_CAIRO_EGL], [1], [Have cairo-egl])],
        [AC_MSG_WARN([clients will use cairo image, cairo-egl not used because $CAIRO_EGL_PKG_ERRORS])])
  PKG_CHECK_MODULES(PANGO, [pangocairo], [have_pango=yes], [have_pango=no])
fi

AC_ARG_ENABLE(weston-launch, [  --enable-weston-launch],, enable_weston_launch=yes)
AM_CONDITIONAL(BUILD_WESTON_LAUNCH, test x$enable_weston_launch == xyes)
if test x$enable_weston_launch == xyes; then
  PKG_CHECK_MODULES(WESTON_LAUNCH, [libdrm])
  PKG_CHECK_MODULES(SYSTEMD_LOGIN, [libsystemd-login],
		    [have_systemd_login=yes], [have_systemd_login=no])
  AS_IF([test "x$have_systemd_login" = "xyes"],
	[AC_DEFINE([HAVE_SYSTEMD_LOGIN], [1], [Have systemd-login])])

  AC_CHECK_LIB([pam], [pam_open_session], [have_pam=yes], [have_pam=no])
  if test x$have_pam == xno; then
    AC_ERROR([weston-launch requires pam])
  fi
  WESTON_LAUNCH_LIBS="$WESTON_LAUNCH_LIBS -lpam"
fi

if test x$enable_egl = xyes; then
  PKG_CHECK_MODULES(GLU, [glu], [have_glu=yes], [have_glu=no])
fi
AM_CONDITIONAL(HAVE_GLU, test "x$have_glu" = "xyes")


AM_CONDITIONAL(HAVE_POPPLER, test "x$have_poppler" = "xyes")

AM_CONDITIONAL(HAVE_PANGO, test "x$have_pango" = "xyes")

AM_CONDITIONAL(BUILD_FULL_GL_CLIENTS,
	       test x$cairo_modules = "xcairo-gl" -a "x$have_cairo_egl" = "xyes" -a "x$enable_egl" = "xyes")

AM_CONDITIONAL(ENABLE_DESKTOP_SHELL, true)

AC_ARG_ENABLE(tablet-shell,
              AS_HELP_STRING([--disable-tablet-shell],
                             [do not build tablet-shell server plugin and client]),,
	      enable_tablet_shell=yes)
AM_CONDITIONAL(ENABLE_TABLET_SHELL,
	       test "x$enable_tablet_shell" = "xyes")


AC_ARG_ENABLE(wcap-tools, [  --disable-wcap-tools],, enable_wcap_tools=yes)
AM_CONDITIONAL(BUILD_WCAP_TOOLS, test x$enable_wcap_tools = xyes)
if test x$enable_wcap_tools = xyes; then
  AC_DEFINE([BUILD_WCAP_TOOLS], [1], [Build the wcap tools])
  PKG_CHECK_MODULES(WCAP, [cairo])
  WCAP_LIBS="$WCAP_LIBS -lm"
fi

AC_CHECK_PROG(RSVG_CONVERT, rsvg-convert, rsvg-convert)
AM_CONDITIONAL(HAVE_RSVG_CONVERT, test -n "$RSVG_CONVERT")

PKG_CHECK_MODULES(SETBACKLIGHT, [libudev libdrm], enable_setbacklight=yes, enable_setbacklight=no)
AM_CONDITIONAL(BUILD_SETBACKLIGHT, test "x$enable_setbacklight" = "xyes")

if test "x$GCC" = "xyes"; then
	GCC_CFLAGS="-Wall -Wextra -Wno-unused-parameter \
		-Wno-missing-field-initializers -g -fvisibility=hidden \
		-Wstrict-prototypes -Wmissing-prototypes"
fi
AC_SUBST(GCC_CFLAGS)

AC_ARG_ENABLE(libunwind, AS_HELP_STRING([  --disable-libunwind],
                                        [Disable libunwind usage for backtraces]),,
              enable_libunwind=yes)
if test "x$enable_libunwind" = xyes; then
    PKG_CHECK_MODULES(LIBUNWIND, libunwind)
    AC_DEFINE(HAVE_LIBUNWIND, 1, [Have libunwind support])
fi
AM_CONDITIONAL(HAVE_LIBUNWIND, [test "x$enable_libunwind" = xyes])

if test "x$WESTON_NATIVE_BACKEND" = "x"; then
	WESTON_NATIVE_BACKEND="drm-backend.so"
fi
AC_MSG_NOTICE([Weston's native backend: $WESTON_NATIVE_BACKEND])
AC_DEFINE_UNQUOTED([WESTON_NATIVE_BACKEND], ["$WESTON_NATIVE_BACKEND"],
		   [The default backend to load, if not wayland nor x11.])

WAYLAND_SCANNER_RULES(['$(top_srcdir)/protocol'])

AC_CONFIG_FILES([Makefile
		 shared/Makefile
		 src/Makefile
		 src/xwayland/Makefile
		 src/version.h
		 src/weston.pc
		 clients/Makefile
		 wcap/Makefile
		 data/Makefile
		 protocol/Makefile
		 man/Makefile
		 tests/Makefile])
AC_OUTPUT
