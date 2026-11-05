/*
 * GLES 2 fill-rate benchmark for the GPU clock tests under LineageOS
 * (lima on the Mali-400 MP2). Renders full-screen quads with a few ALU
 * ops per fragment into an 800x1280 pbuffer and prints frames per second
 * once a second; at a fixed workload the rate follows the GPU clock.
 *
 * Build (macOS, NDK r28):
 *   $NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi29-clang \
 *     -O2 -o out/gpubench tools/lab/gpubench.c -lEGL -lGLESv2
 * Run on the tablet as root: /data/local/tmp/gpubench [seconds] [quads]
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *vs =
	"attribute vec2 p; varying vec2 uv;\n"
	"void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";

static const char *fs =
	"precision mediump float; varying vec2 uv; uniform float t;\n"
	"void main() {\n"
	"  vec2 q = uv * 7.0 + t;\n"
	"  float v = sin(q.x) * cos(q.y) + sin(q.x * 1.7 + q.y * 2.3);\n"
	"  gl_FragColor = vec4(fract(v), fract(v * 1.3), fract(v * 2.1), 1.0);\n"
	"}\n";

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];

		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(1);
	}
	return s;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 10;
	int quads = argc > 2 ? atoi(argv[2]) : 4;
	static const EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE
	};
	static const EGLint pb_attr[] = { EGL_WIDTH, 800, EGL_HEIGHT, 1280, EGL_NONE };
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLConfig cfg;
	EGLint n;
	EGLSurface surf;
	EGLContext ctx;
	GLuint prog;
	GLint t_loc;
	double start, mark;
	int frames = 0, total = 0;

	if (!eglInitialize(dpy, NULL, NULL) ||
	    !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
		fprintf(stderr, "egl init failed: 0x%x\n", eglGetError());
		return 1;
	}
	surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "egl surface/context failed: 0x%x\n", eglGetError());
		return 1;
	}
	printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

	prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glUseProgram(prog);
	t_loc = glGetUniformLocation(prog, "t");
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, 800, 1280);

	start = mark = now();
	while (now() - start < secs) {
		for (int i = 0; i < quads; i++) {
			glUniform1f(t_loc, (float)(total * quads + i));
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		glFinish();
		frames++;
		total++;
		if (now() - mark >= 1.0) {
			printf("%.1f fps\n", frames / (now() - mark));
			fflush(stdout);
			frames = 0;
			mark = now();
		}
	}
	printf("avg %.2f fps over %d frames\n", total / (now() - start), total);
	return 0;
}
