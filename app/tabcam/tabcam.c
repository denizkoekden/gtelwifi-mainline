// SPDX-License-Identifier: GPL-2.0
/*
 * tabcam: a small camera for the Galaxy Tab E 9.6 (gtelwifi) under Phosh.
 *
 * GNOME Snapshot renders the preview on the CPU here (GTK 4 needs GLES 3,
 * the Mali-400 has GLES 2) and keeps the four Cortex-A7 busy at 11 fps.
 * tabcam takes the frames from PipeWire (NV12, through libcamera) with an
 * appsink and draws them with SDL3's GLES 2 renderer, which converts NV12
 * in a shader: full sensor rate at a few percent CPU.
 *
 * Controls: shutter, camera switch and the last photo (opens it) on the
 * main bar; rotation lock (Phosh's setting), self-timer and a thirds grid
 * on the tool bar; the volume keys also release the shutter. Photos are
 * JPEGs in ~/Pictures, upright by the accelerometer even with the rotation
 * locked.
 *
 * Orientation: the preview follows the display rotation that Phosh applies
 * (read from its org.gnome.Mutter.DisplayConfig, SDL's display orientation
 * does not follow it); photos and the control icons follow the
 * accelerometer (iio-sensor-proxy).
 *
 *   tabcam [front]
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <gio/gio.h>
#include <gst/app/app.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ICON_DIR	"/usr/share/icons/Adwaita/symbolic/"
#define FONT_FILE	"/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf"
#define LOCK_SCHEMA	"org.gnome.settings-daemon.peripherals.touchscreen"
#define LOCK_KEY	"orientation-lock"

/*
 * PipeWire nodes of the two cameras and their mounting rotation, the DT
 * "rotation" property, which counts counter-clockwise as in libcamera: the
 * picture turned clockwise by 360 - rotation - display rotation is upright.
 * The mirror flag gives the front camera a mirror-image preview.
 */
static const struct cam {
	const char *node;
	int rotation;
	bool mirror;
	const char *name;
} cams[] = {
	{ "libcamera_input._base_soc_i2c_70500000_camera_56", 270, false,
	  "Rückkamera" },
	{ "libcamera_input._base_soc_i2c_70500000_camera_20", 90, true,
	  "Frontkamera" },
};

enum icon { IC_SWITCH, IC_LOCKED, IC_UNLOCKED, IC_GRID, IC_TIMER, IC_IMAGE,
	    IC_N };

static const char * const icon_files[IC_N] = {
	"actions/camera-switch-symbolic.svg",
	"status/rotation-locked-symbolic.svg",
	"status/rotation-allowed-symbolic.svg",
	"actions/view-grid-symbolic.svg",
	"status/alarm-symbolic.svg",
	"mimetypes/image-x-generic-symbolic.svg",
};

static const SDL_FColor accent = { 0.98f, 0.82f, 0.25f, 1 };

struct text {			/* a cached rendered string */
	char str[64];
	SDL_Texture *tex;
	float w, h;
};

struct app {
	SDL_Window *win;
	SDL_Renderer *ren;
	int w, h;			/* window size the layout was made for */

	SDL_Texture *frame;		/* camera picture, NV12 */
	int frame_w, frame_h;
	unsigned int frame_cam;		/* camera the picture came from */
	SDL_Texture *icons[IC_N];
	SDL_Texture *disc;		/* white anti-aliased disc */
	SDL_Texture *thumb;		/* last photo */
	char last_photo[320];
	TTF_Font *font, *big;
	struct text t_toast, t_count, t_timer;

	GstElement *pipe;
	GstAppSink *sink;
	GstSample *last;		/* newest frame, for the shutter */
	unsigned int cam;
	unsigned int frames;		/* since the camera started */
	bool debug;

	GDBusConnection *session, *system;
	GSettings *lock;
	int display, device;		/* clockwise rotations, degrees */
	Uint64 polled;

	int timer;			/* self-timer seconds, 0 = off */
	bool grid;
	Uint64 countdown_end;		/* 0 = no countdown */
	Uint64 flash_until, toast_until;
	char toast[64];

	/* button centres and radii, window coordinates */
	SDL_FPoint p_shutter, p_switch, p_gallery, p_lock, p_timer, p_grid;
	float r_shutter, r_button;
	SDL_FRect bar_main, bar_tool;
};

static volatile sig_atomic_t quit;

static void on_signal(int sig)
{
	quit = 1;
}

/* ---- settings ---------------------------------------------------------- */

static void state_dir(char *buf, size_t n)
{
	const char *home = getenv("HOME");

	snprintf(buf, n, "%s/.config/tabcam", home ? home : "/tmp");
}

static void load_state(struct app *a)
{
	char path[256], line[64];
	FILE *f;

	state_dir(path, sizeof(path) - 8);
	strcat(path, "/state");
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		int v;

		if (sscanf(line, "camera=%d", &v) == 1)
			a->cam = v ? 1 : 0;
		else if (sscanf(line, "timer=%d", &v) == 1)
			a->timer = (v == 3 || v == 10) ? v : 0;
		else if (sscanf(line, "grid=%d", &v) == 1)
			a->grid = v;
	}
	fclose(f);
}

static void save_state(struct app *a)
{
	char path[256];
	FILE *f;

	state_dir(path, sizeof(path) - 8);
	g_mkdir_with_parents(path, 0755);
	strcat(path, "/state");
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "camera=%u\ntimer=%d\ngrid=%d\n", a->cam, a->timer, a->grid);
	fclose(f);
}

static void show_toast(struct app *a, const char *msg)
{
	snprintf(a->toast, sizeof(a->toast), "%s", msg);
	a->toast_until = SDL_GetTicks() + 1500;
}

/* ---- orientation ------------------------------------------------------- */

/*
 * Display: the transform of the first logical monitor in DisplayConfig's
 * GetCurrentState, 0..3 for normal, left-up, bottom-up, right-up on the
 * gtelwifi. Device: iio-sensor-proxy's accelerometer orientation, on the
 * same scale. Polled every 300 ms.
 */
static void poll_orientation(struct app *a)
{
	static const char * const names[] = {
		"normal", "left-up", "bottom-up", "right-up"
	};
	GVariant *r, *lms, *lm, *v;
	const char *o;
	guint32 t;
	int i;

	if (SDL_GetTicks() - a->polled < 300)
		return;
	a->polled = SDL_GetTicks();

	r = !a->session ? NULL :
	    g_dbus_connection_call_sync(a->session,
			"org.gnome.Mutter.DisplayConfig",
			"/org/gnome/Mutter/DisplayConfig",
			"org.gnome.Mutter.DisplayConfig", "GetCurrentState",
			NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 200, NULL, NULL);
	if (r) {
		lms = g_variant_get_child_value(r, 2);
		if (g_variant_n_children(lms)) {
			lm = g_variant_get_child_value(lms, 0);
			g_variant_get_child(lm, 3, "u", &t);
			a->display = (t & 3) * 90;
			g_variant_unref(lm);
		}
		g_variant_unref(lms);
		g_variant_unref(r);
	}

	r = !a->system ? NULL :
	    g_dbus_connection_call_sync(a->system,
			"net.hadess.SensorProxy", "/net/hadess/SensorProxy",
			"org.freedesktop.DBus.Properties", "Get",
			g_variant_new("(ss)", "net.hadess.SensorProxy",
				      "AccelerometerOrientation"),
			G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 200,
			NULL, NULL);
	if (r) {
		g_variant_get(r, "(v)", &v);
		o = g_variant_get_string(v, NULL);
		for (i = 0; i < 4; i++)
			if (!strcmp(o, names[i]))
				a->device = i * 90;
		g_variant_unref(v);
		g_variant_unref(r);
	} else {
		a->device = a->display;
	}
}

/* clockwise rotation of the camera picture on the display */
static int preview_rotation(struct app *a, unsigned int cam)
{
	return (720 - cams[cam].rotation - a->display) % 360;
}

/* clockwise rotation that makes a saved photo upright */
static int photo_rotation(struct app *a)
{
	return (720 - cams[a->cam].rotation - a->device) % 360;
}

/* rotation of icons and text, so that they stay upright when locked */
static float ui_angle(struct app *a)
{
	return (float)((a->display - a->device + 360) % 360);
}

/* ---- camera ------------------------------------------------------------ */

static bool start_camera(struct app *a)
{
	char desc[512];
	GError *err = NULL;

	snprintf(desc, sizeof(desc),
		 "pipewiresrc target-object=%s "
		 "stream-properties=\"props,media.type=Video,media.category=Capture,media.role=Camera\" "
		 "! video/x-raw,format=NV12 "
		 "! appsink name=sink max-buffers=2 drop=true sync=false",
		 cams[a->cam].node);
	a->pipe = gst_parse_launch(desc, &err);
	if (!a->pipe) {
		fprintf(stderr, "tabcam: %s\n", err->message);
		g_error_free(err);
		return false;
	}
	a->sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(a->pipe), "sink"));
	gst_element_set_state(a->pipe, GST_STATE_PLAYING);
	a->frames = 0;
	if (a->debug)
		fprintf(stderr, "tabcam: start %s\n", cams[a->cam].node);
	return true;
}

static void stop_camera(struct app *a)
{
	if (!a->pipe)
		return;
	gst_element_set_state(a->pipe, GST_STATE_NULL);
	gst_object_unref(a->sink);
	gst_object_unref(a->pipe);
	a->pipe = NULL;
	a->sink = NULL;
	if (a->last) {
		gst_sample_unref(a->last);
		a->last = NULL;
	}
}

static void check_bus(struct app *a)
{
	GstBus *bus;
	GstMessage *m;

	if (!a->pipe)
		return;
	bus = gst_element_get_bus(a->pipe);
	while ((m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR))) {
		GError *err;

		gst_message_parse_error(m, &err, NULL);
		fprintf(stderr, "tabcam: %s\n", err->message);
		g_error_free(err);
		gst_message_unref(m);
	}
	gst_object_unref(bus);
}

static void upload(struct app *a, GstSample *s)
{
	GstVideoInfo info;
	GstVideoFrame f;

	if (!gst_video_info_from_caps(&info, gst_sample_get_caps(s)))
		return;
	if (!a->frame || a->frame_w != info.width || a->frame_h != info.height) {
		SDL_DestroyTexture(a->frame);
		a->frame = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_NV12,
					     SDL_TEXTUREACCESS_STREAMING,
					     info.width, info.height);
		a->frame_w = info.width;
		a->frame_h = info.height;
	}
	if (!gst_video_frame_map(&f, &info, gst_sample_get_buffer(s),
				 GST_MAP_READ))
		return;
	a->frame_cam = a->cam;
	SDL_UpdateNVTexture(a->frame, NULL,
			    GST_VIDEO_FRAME_PLANE_DATA(&f, 0),
			    GST_VIDEO_FRAME_PLANE_STRIDE(&f, 0),
			    GST_VIDEO_FRAME_PLANE_DATA(&f, 1),
			    GST_VIDEO_FRAME_PLANE_STRIDE(&f, 1));
	gst_video_frame_unmap(&f);
}

static void switch_camera(struct app *a)
{
	stop_camera(a);
	a->cam ^= 1;
	start_camera(a);
	show_toast(a, cams[a->cam].name);
	save_state(a);
}

/* ---- photos ------------------------------------------------------------ */

static void load_thumb(struct app *a, const char *path)
{
	SDL_Surface *s = IMG_Load(path);

	if (!s)
		return;
	SDL_DestroyTexture(a->thumb);
	a->thumb = SDL_CreateTextureFromSurface(a->ren, s);
	SDL_DestroySurface(s);
	snprintf(a->last_photo, sizeof(a->last_photo), "%s", path);
}

/* the newest tabcam photo from an earlier run, for the gallery button */
static void find_last_photo(struct app *a)
{
	const char *home = getenv("HOME"), *name;
	char dir[256], path[320], *best = NULL;
	GDir *d;

	if (!home)
		return;
	snprintf(dir, sizeof(dir), "%s/Pictures", home);
	d = g_dir_open(dir, 0, NULL);
	if (!d)
		return;
	/* the names carry the time, so the largest is the newest */
	while ((name = g_dir_read_name(d)))
		if (g_str_has_prefix(name, "tabcam-") &&
		    g_str_has_suffix(name, ".jpg") &&
		    (!best || strcmp(name, best) > 0)) {
			g_free(best);
			best = g_strdup(name);
		}
	g_dir_close(d);
	if (best) {
		snprintf(path, sizeof(path), "%s/%s", dir, best);
		load_thumb(a, path);
		g_free(best);
	}
}

/* the newest frame as JPEG, rotated upright, not mirrored */
static void take_photo(struct app *a)
{
	static const char * const methods[] = {
		"identity", "clockwise", "rotate-180", "counterclockwise"
	};
	const char *home = getenv("HOME");
	char dir[256], path[320], desc[512], stamp[32];
	GstElement *p, *src;
	GstBus *bus;
	GstMessage *m;
	time_t t = time(NULL);
	bool ok = false;

	if (!a->last || !home)
		return;
	snprintf(dir, sizeof(dir), "%s/Pictures", home);
	g_mkdir_with_parents(dir, 0755);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
	snprintf(path, sizeof(path), "%s/tabcam-%s.jpg", dir, stamp);
	snprintf(desc, sizeof(desc),
		 "appsrc name=src ! videoconvert ! videoflip method=%s "
		 "! jpegenc quality=92 ! filesink location=\"%s\"",
		 methods[photo_rotation(a) / 90], path);
	p = gst_parse_launch(desc, NULL);
	if (!p)
		return;
	src = gst_bin_get_by_name(GST_BIN(p), "src");
	g_object_set(src, "caps", gst_sample_get_caps(a->last), NULL);
	gst_element_set_state(p, GST_STATE_PLAYING);
	gst_app_src_push_buffer(GST_APP_SRC(src),
				gst_buffer_ref(gst_sample_get_buffer(a->last)));
	gst_app_src_end_of_stream(GST_APP_SRC(src));
	bus = gst_element_get_bus(p);
	m = gst_bus_timed_pop_filtered(bus, 3 * GST_SECOND,
				       GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
	if (m) {
		ok = GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
		gst_message_unref(m);
	}
	gst_object_unref(bus);
	gst_element_set_state(p, GST_STATE_NULL);
	gst_object_unref(src);
	gst_object_unref(p);
	if (a->debug)
		fprintf(stderr, "tabcam: photo %s, camera %u, display %d, device %d, rotation %d, %s\n",
			path, a->cam, a->display, a->device, photo_rotation(a),
			ok ? "ok" : "failed");
	if (!ok) {
		unlink(path);
		show_toast(a, "Foto fehlgeschlagen");
		return;
	}
	load_thumb(a, path);
	a->flash_until = SDL_GetTicks() + 120;
	show_toast(a, "Foto gespeichert");
}

static void open_last_photo(struct app *a)
{
	char *uri;

	if (!a->last_photo[0])
		return;
	uri = g_filename_to_uri(a->last_photo, NULL, NULL);
	if (uri && !g_app_info_launch_default_for_uri(uri, NULL, NULL))
		show_toast(a, "Kein Bildbetrachter");
	g_free(uri);
}

static void shutter(struct app *a)
{
	if (a->countdown_end) {
		a->countdown_end = 0;
		show_toast(a, "Selbstauslöser abgebrochen");
	} else if (a->timer) {
		a->countdown_end = SDL_GetTicks() + a->timer * 1000;
	} else {
		take_photo(a);
	}
}

/* ---- toggles ------------------------------------------------------------ */

static bool locked(struct app *a)
{
	return a->lock && g_settings_get_boolean(a->lock, LOCK_KEY);
}

static void toggle_lock(struct app *a)
{
	bool on;

	if (!a->lock)
		return;
	on = !locked(a);
	g_settings_set_boolean(a->lock, LOCK_KEY, on);
	g_settings_sync();
	show_toast(a, on ? "Drehsperre an" : "Drehsperre aus");
}

static void cycle_timer(struct app *a)
{
	char msg[48];

	a->timer = a->timer == 0 ? 3 : a->timer == 3 ? 10 : 0;
	a->countdown_end = 0;
	if (a->timer)
		snprintf(msg, sizeof(msg), "Selbstauslöser %d s", a->timer);
	else
		snprintf(msg, sizeof(msg), "Selbstauslöser aus");
	show_toast(a, msg);
	save_state(a);
}

static void toggle_grid(struct app *a)
{
	a->grid = !a->grid;
	save_state(a);
}

/* ---- drawing helpers ---------------------------------------------------- */

/* a white disc with a soft edge, used for all round shapes */
static SDL_Texture *make_disc(SDL_Renderer *ren, int px)
{
	SDL_Surface *s = SDL_CreateSurface(px, px, SDL_PIXELFORMAT_ARGB8888);
	SDL_Texture *t;
	float c = (px - 1) / 2.f, r = px / 2.f - 1;
	int x, y;

	if (!s)
		return NULL;
	for (y = 0; y < px; y++) {
		Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);

		for (x = 0; x < px; x++) {
			float d = hypotf(x - c, y - c);
			float al = SDL_clamp(r - d + 0.5f, 0.f, 1.f);

			row[x] = (Uint32)(al * 255) << 24 | 0xffffff;
		}
	}
	t = SDL_CreateTextureFromSurface(ren, s);
	SDL_DestroySurface(s);
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	return t;
}

/* a symbolic Adwaita icon, rendered at px and recoloured to white */
static SDL_Texture *load_icon(SDL_Renderer *ren, const char *file, int px)
{
	char path[256];
	SDL_IOStream *io;
	SDL_Surface *s, *c;
	SDL_Texture *t;
	int x, y;

	snprintf(path, sizeof(path), ICON_DIR "%s", file);
	io = SDL_IOFromFile(path, "rb");
	if (!io)
		return NULL;
	s = IMG_LoadSizedSVG_IO(io, px, px);
	SDL_CloseIO(io);
	if (!s)
		return NULL;
	c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
	SDL_DestroySurface(s);
	if (!c)
		return NULL;
	for (y = 0; y < c->h; y++) {
		Uint32 *row = (Uint32 *)((Uint8 *)c->pixels + y * c->pitch);

		for (x = 0; x < c->w; x++)
			row[x] |= 0xffffff;
	}
	t = SDL_CreateTextureFromSurface(ren, c);
	SDL_DestroySurface(c);
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	return t;
}

static void draw_disc(struct app *a, SDL_FPoint p, float r, SDL_FColor col)
{
	SDL_FRect d = { p.x - r, p.y - r, 2 * r, 2 * r };

	SDL_SetTextureColorModFloat(a->disc, col.r, col.g, col.b);
	SDL_SetTextureAlphaModFloat(a->disc, col.a);
	SDL_RenderTexture(a->ren, a->disc, NULL, &d);
}

static void draw_centered(struct app *a, SDL_Texture *t, SDL_FPoint p,
			  float w, float h, SDL_FColor col)
{
	SDL_FRect d = { p.x - w / 2, p.y - h / 2, w, h };

	if (!t)
		return;
	SDL_SetTextureColorModFloat(t, col.r, col.g, col.b);
	SDL_SetTextureAlphaModFloat(t, col.a);
	SDL_RenderTextureRotated(a->ren, t, NULL, &d, ui_angle(a), NULL,
				 SDL_FLIP_NONE);
}

static struct text *text(struct app *a, struct text *c, TTF_Font *font,
			 const char *str)
{
	SDL_Color white = { 255, 255, 255, 255 };
	SDL_Surface *s;

	if (c->tex && !strcmp(c->str, str))
		return c;
	SDL_DestroyTexture(c->tex);
	c->tex = NULL;
	snprintf(c->str, sizeof(c->str), "%s", str);
	s = TTF_RenderText_Blended(font, str, 0, white);
	if (!s)
		return c;
	c->tex = SDL_CreateTextureFromSurface(a->ren, s);
	c->w = s->w;
	c->h = s->h;
	SDL_DestroySurface(s);
	return c;
}

/* the last photo, cropped to a circle and turned with the icons */
static void draw_thumb(struct app *a, SDL_FPoint p, float r)
{
	enum { N = 40 };
	SDL_Vertex v[N + 1];
	int idx[N * 3], i;
	float side, su, sv, rot = ui_angle(a) * (float)M_PI / 180;
	SDL_FColor white = { 1, 1, 1, 1 };
	float tw, th;

	SDL_GetTextureSize(a->thumb, &tw, &th);
	side = SDL_min(tw, th);
	su = side / tw / 2;
	sv = side / th / 2;
	v[0] = (SDL_Vertex){ p, white, { 0.5f, 0.5f } };
	for (i = 0; i < N; i++) {
		float t = 2 * (float)M_PI * i / N;

		v[i + 1].position = (SDL_FPoint){ p.x + r * cosf(t),
						  p.y + r * sinf(t) };
		v[i + 1].color = white;
		v[i + 1].tex_coord = (SDL_FPoint){ 0.5f + su * cosf(t - rot),
						   0.5f + sv * sinf(t - rot) };
		idx[3 * i] = 0;
		idx[3 * i + 1] = 1 + i;
		idx[3 * i + 2] = 1 + (i + 1) % N;
	}
	SDL_RenderGeometry(a->ren, a->thumb, v, N + 1, idx, N * 3);
}

/* ---- layout and drawing ------------------------------------------------- */

static void reset_text(struct text *t)
{
	SDL_DestroyTexture(t->tex);
	memset(t, 0, sizeof(*t));
}

static void layout(struct app *a, int w, int h)
{
	float u = SDL_min(w, h) / 10.f, bar = 2.4f * u, tool = 1.5f * u;
	int px, i;

	if (w == a->w && h == a->h)
		return;
	a->w = w;
	a->h = h;
	a->r_shutter = 0.85f * u;
	a->r_button = 0.55f * u;
	if (h >= w) {		/* portrait: bars at the bottom and the top */
		a->bar_main = (SDL_FRect){ 0, h - bar, w, bar };
		a->bar_tool = (SDL_FRect){ 0, 0, w, tool };
		a->p_shutter = (SDL_FPoint){ w / 2.f, h - bar / 2 };
		a->p_gallery = (SDL_FPoint){ w * 0.18f, h - bar / 2 };
		a->p_switch = (SDL_FPoint){ w * 0.82f, h - bar / 2 };
		a->p_lock = (SDL_FPoint){ w * 0.18f, tool / 2 };
		a->p_timer = (SDL_FPoint){ w * 0.5f, tool / 2 };
		a->p_grid = (SDL_FPoint){ w * 0.82f, tool / 2 };
	} else {		/* landscape: bars at the right and the left */
		a->bar_main = (SDL_FRect){ w - bar, 0, bar, h };
		a->bar_tool = (SDL_FRect){ 0, 0, tool, h };
		a->p_shutter = (SDL_FPoint){ w - bar / 2, h / 2.f };
		a->p_switch = (SDL_FPoint){ w - bar / 2, h * 0.18f };
		a->p_gallery = (SDL_FPoint){ w - bar / 2, h * 0.82f };
		a->p_lock = (SDL_FPoint){ tool / 2, h * 0.18f };
		a->p_timer = (SDL_FPoint){ tool / 2, h * 0.5f };
		a->p_grid = (SDL_FPoint){ tool / 2, h * 0.82f };
	}

	/* textures at the sizes this layout draws them */
	px = (int)(a->r_button * 1.1f);
	for (i = 0; i < IC_N; i++) {
		SDL_DestroyTexture(a->icons[i]);
		a->icons[i] = load_icon(a->ren, icon_files[i], px);
	}
	SDL_DestroyTexture(a->disc);
	a->disc = make_disc(a->ren, (int)(2.2f * a->r_shutter));
	if (a->font)
		TTF_CloseFont(a->font);
	if (a->big)
		TTF_CloseFont(a->big);
	a->font = TTF_OpenFont(FONT_FILE, 0.42f * u);
	a->big = TTF_OpenFont(FONT_FILE, 3.0f * u);
	reset_text(&a->t_toast);
	reset_text(&a->t_count);
	reset_text(&a->t_timer);
}

/*
 * The picture is drawn as its own camera needs it: after a switch the
 * last picture of the old camera stays, dimmed, until the new one sends.
 */
static void draw_frame(struct app *a)
{
	int rot = preview_rotation(a, a->frame_cam);
	float pw, ph, s;
	SDL_FRect dst;

	if (!a->frame)
		return;
	pw = (rot % 180) ? a->frame_h : a->frame_w;
	ph = (rot % 180) ? a->frame_w : a->frame_h;
	s = SDL_min(a->w / pw, a->h / ph);
	/* the rectangle is that of the unrotated texture */
	dst.w = a->frame_w * s;
	dst.h = a->frame_h * s;
	dst.x = (a->w - dst.w) / 2;
	dst.y = (a->h - dst.h) / 2;
	/*
	 * SDL flips the texture before turning it: for a left-right mirror
	 * on the screen a picture turned by 90 or 270 degrees needs a
	 * top-bottom flip of the texture.
	 */
	SDL_RenderTextureRotated(a->ren, a->frame, NULL, &dst, rot, NULL,
				 !cams[a->frame_cam].mirror ? SDL_FLIP_NONE :
				 rot % 180 ? SDL_FLIP_VERTICAL :
				 SDL_FLIP_HORIZONTAL);
	if (a->frame_cam != a->cam) {
		SDL_SetRenderDrawColor(a->ren, 0, 0, 0, 140);
		SDL_RenderFillRect(a->ren, NULL);
	}

	if (a->grid) {
		float x0 = (a->w - pw * s) / 2, y0 = (a->h - ph * s) / 2;
		int i;

		SDL_SetRenderDrawColor(a->ren, 255, 255, 255, 90);
		for (i = 1; i < 3; i++) {
			float x = x0 + pw * s * i / 3, y = y0 + ph * s * i / 3;

			SDL_RenderLine(a->ren, x, y0, x, y0 + ph * s);
			SDL_RenderLine(a->ren, x0, y, x0 + pw * s, y);
		}
	}
}

static void draw(struct app *a)
{
	SDL_FColor white = { 1, 1, 1, 1 }, dim = { 0, 0, 0, 0.45f };
	SDL_FColor ring = { 1, 1, 1, 0.9f }, red = { 0.9f, 0.2f, 0.2f, 1 };
	Uint64 now = SDL_GetTicks();
	float ic = a->r_button * 1.1f;
	char buf[16];

	SDL_SetRenderDrawBlendMode(a->ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->ren, 0, 0, 0, 255);
	SDL_RenderClear(a->ren);
	draw_frame(a);

	/* bars */
	SDL_SetRenderDrawColor(a->ren, 0, 0, 0, 110);
	SDL_RenderFillRect(a->ren, &a->bar_main);
	SDL_RenderFillRect(a->ren, &a->bar_tool);

	/* shutter: ring and disc, red while the self-timer runs */
	draw_disc(a, a->p_shutter, a->r_shutter, ring);
	draw_disc(a, a->p_shutter, a->r_shutter * 0.86f,
		  (SDL_FColor){ 0, 0, 0, 1 });
	draw_disc(a, a->p_shutter, a->r_shutter * 0.76f,
		  a->countdown_end ? red : white);

	/* camera switch */
	draw_disc(a, a->p_switch, a->r_button, dim);
	draw_centered(a, a->icons[IC_SWITCH], a->p_switch, ic, ic, white);

	/* last photo */
	if (a->thumb) {
		draw_disc(a, a->p_gallery, a->r_button * 1.08f, ring);
		draw_thumb(a, a->p_gallery, a->r_button);
	} else {
		draw_disc(a, a->p_gallery, a->r_button, dim);
		draw_centered(a, a->icons[IC_IMAGE], a->p_gallery, ic * 0.8f,
			      ic * 0.8f, (SDL_FColor){ 1, 1, 1, 0.6f });
	}

	/* tool bar: rotation lock, self-timer, grid */
	draw_centered(a, a->icons[locked(a) ? IC_LOCKED : IC_UNLOCKED],
		      a->p_lock, ic * 0.8f, ic * 0.8f,
		      locked(a) ? accent : white);
	if (a->timer && a->font) {
		struct text *t;

		snprintf(buf, sizeof(buf), "%d s", a->timer);
		t = text(a, &a->t_timer, a->font, buf);
		draw_centered(a, t->tex, a->p_timer, t->w, t->h, accent);
	} else {
		draw_centered(a, a->icons[IC_TIMER], a->p_timer, ic * 0.8f,
			      ic * 0.8f, white);
	}
	draw_centered(a, a->icons[IC_GRID], a->p_grid, ic * 0.8f, ic * 0.8f,
		      a->grid ? accent : white);

	/* self-timer countdown */
	if (a->countdown_end && a->big) {
		struct text *t;

		snprintf(buf, sizeof(buf), "%d",
			 (int)((a->countdown_end - now + 999) / 1000));
		t = text(a, &a->t_count, a->big, buf);
		draw_centered(a, t->tex, (SDL_FPoint){ a->w / 2.f, a->h / 2.f },
			      t->w, t->h, (SDL_FColor){ 1, 1, 1, 0.9f });
	}

	/* toast, a box under the tool bar */
	if (now < a->toast_until && a->font) {
		struct text *t = text(a, &a->t_toast, a->font, a->toast);
		SDL_FPoint p;
		SDL_FRect box;

		if (a->h >= a->w)
			p = (SDL_FPoint){ a->w / 2.f,
					  a->bar_tool.h + t->h * 1.4f };
		else
			p = (SDL_FPoint){ a->w / 2.f, t->h * 1.4f };
		box = (SDL_FRect){ p.x - t->w / 2 - t->h * 0.6f,
				   p.y - t->h * 0.7f,
				   t->w + t->h * 1.2f, t->h * 1.4f };
		SDL_SetRenderDrawColor(a->ren, 0, 0, 0, 150);
		SDL_RenderFillRect(a->ren, &box);
		draw_centered(a, t->tex, p, t->w, t->h, white);
	}

	if (now < a->flash_until) {
		SDL_SetRenderDrawColor(a->ren, 255, 255, 255, 200);
		SDL_RenderFillRect(a->ren, NULL);
	}
	SDL_RenderPresent(a->ren);
}

static bool near(SDL_FPoint p, float r, float x, float y)
{
	return hypotf(x - p.x, y - p.y) <= r * 1.3f;
}

static void tap(struct app *a, float x, float y)
{
	if (near(a->p_shutter, a->r_shutter, x, y))
		shutter(a);
	else if (near(a->p_switch, a->r_button, x, y))
		switch_camera(a);
	else if (near(a->p_gallery, a->r_button, x, y))
		open_last_photo(a);
	else if (near(a->p_lock, a->r_button, x, y))
		toggle_lock(a);
	else if (near(a->p_timer, a->r_button, x, y))
		cycle_timer(a);
	else if (near(a->p_grid, a->r_button, x, y))
		toggle_grid(a);
}

static GSettings *lock_settings(void)
{
	GSettingsSchemaSource *src = g_settings_schema_source_get_default();
	GSettingsSchema *s;
	GSettings *g;

	if (!src)
		return NULL;
	s = g_settings_schema_source_lookup(src, LOCK_SCHEMA, TRUE);
	if (!s)
		return NULL;
	g = g_settings_new_full(s, NULL, NULL);
	g_settings_schema_unref(s);
	return g;
}

static void sensor_call(struct app *a, const char *method)
{
	GVariant *r;

	if (!a->system)
		return;
	r = g_dbus_connection_call_sync(a->system, "net.hadess.SensorProxy",
					"/net/hadess/SensorProxy",
					"net.hadess.SensorProxy", method, NULL,
					NULL, G_DBUS_CALL_FLAGS_NONE, 500, NULL,
					NULL);
	if (r)
		g_variant_unref(r);
}

int main(int argc, char **argv)
{
	struct app a = { 0 };
	bool run = true;
	SDL_Event ev;
	int w, h;

	gst_init(&argc, &argv);
	load_state(&a);
	a.debug = getenv("TABCAM_DEBUG");
	if (argc > 1 && !strcmp(argv[1], "front"))
		a.cam = 1;
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init()) {
		fprintf(stderr, "tabcam: %s\n", SDL_GetError());
		return 1;
	}
	/*
	 * No vsync: under Wayland a hidden window gets no frame callbacks and
	 * a vsynced present would block until it is shown again. Frames come
	 * at the sensor rate anyway, and the loop draws only when something
	 * changed.
	 */
	if (!SDL_CreateWindowAndRenderer("Kamera", 800, 1280,
					 SDL_WINDOW_RESIZABLE |
					 SDL_WINDOW_FULLSCREEN,
					 &a.win, &a.ren)) {
		fprintf(stderr, "tabcam: %s\n", SDL_GetError());
		return 1;
	}
	a.session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	a.system = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	/* the accelerometer only reports while someone claims it */
	sensor_call(&a, "ClaimAccelerometer");
	a.lock = lock_settings();
	find_last_photo(&a);
	if (!start_camera(&a))
		return 1;

	while (run && !quit) {
		Uint64 now = SDL_GetTicks();
		bool dirty = now < a.flash_until + 50 ||
			     now < a.toast_until + 50 || a.countdown_end;
		GstSample *s;

		while (SDL_PollEvent(&ev)) {
			dirty = true;
			if (ev.type == SDL_EVENT_QUIT)
				run = false;
			else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
				tap(&a, ev.button.x, ev.button.y);
			else if (ev.type == SDL_EVENT_KEY_DOWN &&
				 !ev.key.repeat &&
				 (ev.key.scancode == SDL_SCANCODE_VOLUMEUP ||
				  ev.key.scancode == SDL_SCANCODE_VOLUMEDOWN))
				shutter(&a);
		}
		s = gst_app_sink_try_pull_sample(a.sink, 20 * GST_MSECOND);
		if (s && !a.frames++ && a.debug)
			fprintf(stderr, "tabcam: first frame, camera %u, display %d, preview %d\n",
				a.cam, a.display, preview_rotation(&a, a.cam));
		if (s) {
			upload(&a, s);
			if (a.last)
				gst_sample_unref(a.last);
			a.last = s;
			dirty = true;
		}
		if (a.countdown_end && now >= a.countdown_end) {
			a.countdown_end = 0;
			take_photo(&a);
		}
		check_bus(&a);
		g_main_context_iteration(NULL, FALSE);	/* GSettings updates */
		poll_orientation(&a);
		SDL_GetCurrentRenderOutputSize(a.ren, &w, &h);
		layout(&a, w, h);
		if (dirty)
			draw(&a);
	}
	stop_camera(&a);
	sensor_call(&a, "ReleaseAccelerometer");
	SDL_Quit();
	return 0;
}
