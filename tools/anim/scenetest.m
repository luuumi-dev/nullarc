// scenetest: a chess-like 3D scene for measuring 3D rendering, the way Chess.app draws it.
//   scenetest gl  [seconds] [w h]   OpenGL 3.2 core, 4x MSAA, 33 lit meshes (Apple's GL runs on Metal, like Chess);
//                                   SWAP0=1 in the environment turns vsync off
//   scenetest scn [seconds] [w h]   SceneKit (Metal), 4x MSAA, PBR pieces, shadowed light
// Prints one line: "<mode> <w>x<h> frames <n> fps <f> frame ms avg <a> p50 <m> p95 <p> max <x> cpu ms/frame <c>".
// Run inside the GUI session: launchctl asuser <uid> sudo -u <user> ./scenetest gl 10
#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>
#import <SceneKit/SceneKit.h>
#include <mach/mach_time.h>
#include <sys/resource.h>

static double now_ms(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * (double)tb.numer / tb.denom / 1e6; }
static double cpu_ms(void) { struct rusage u; getrusage(RUSAGE_SELF, &u); return (u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1e3 + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e3; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static NSString *g_mode; static double g_secs = 10; static int g_w = 2400, g_h = 1500;
static double *g_ft; static int g_n, g_cap; static double g_last, g_t0, g_cpu0;

static void frame_mark(void)
{
    const double t = now_ms();
    if (!g_t0) { g_t0 = g_last = t; g_cpu0 = cpu_ms(); return; }
    if (g_n < g_cap) g_ft[g_n++] = t - g_last;
    g_last = t;
    if (t - g_t0 >= g_secs * 1e3) {
        const double cpu = cpu_ms() - g_cpu0, wall = t - g_t0;
        double sum = 0, mx = 0; for (int i = 0; i < g_n; i++) { sum += g_ft[i]; if (g_ft[i] > mx) mx = g_ft[i]; }
        qsort(g_ft, g_n, sizeof *g_ft, cmpd);
        printf("%s %dx%d frames %d fps %.1f frame ms avg %.2f p50 %.2f p95 %.2f max %.1f cpu ms/frame %.2f\n",
               g_mode.UTF8String, g_w, g_h, g_n, g_n * 1e3 / wall, sum / g_n, g_ft[g_n / 2], g_ft[(int)(g_n * 0.95)], mx, cpu / g_n);
        fflush(stdout); exit(0);
    }
}

// ---- a lathe mesh (a chess piece silhouette) shared by both modes ----
typedef struct { float p[3], n[3]; } V;
static const float prof[][2] = { {0.00f,0.00f},{0.42f,0.00f},{0.42f,0.08f},{0.30f,0.14f},{0.22f,0.30f},{0.16f,0.62f},{0.24f,0.70f},{0.14f,0.78f},{0.20f,0.92f},{0.12f,1.04f},{0.00f,1.06f} };
static V *lathe(int seg, int *count)
{
    const int np = sizeof prof / sizeof prof[0]; V *v = malloc(sizeof(V) * seg * (np - 1) * 6); int k = 0;
    for (int s = 0; s < seg; s++) for (int i = 0; i < np - 1; i++) {
        const float a0 = 2 * M_PI * s / seg, a1 = 2 * M_PI * (s + 1) / seg;
        const float r0 = prof[i][0], y0 = prof[i][1], r1 = prof[i + 1][0], y1 = prof[i + 1][1];
        const float dy = y1 - y0, dr = r1 - r0, l = sqrtf(dy * dy + dr * dr) ?: 1, nr = dy / l, ny = -dr / l;
        const float q[4][2] = { {r0, y0}, {r1, y1}, {r1, y1}, {r0, y0} }; const float an[4] = { a0, a0, a1, a1 };
        V c[4]; for (int j = 0; j < 4; j++) c[j] = (V){ { q[j][0] * cosf(an[j]), q[j][1], q[j][0] * sinf(an[j]) }, { nr * cosf(an[j]), ny, nr * sinf(an[j]) } };
        v[k++] = c[0]; v[k++] = c[1]; v[k++] = c[2]; v[k++] = c[0]; v[k++] = c[2]; v[k++] = c[3];
    }
    *count = k; return v;
}

// ---- OpenGL ----
@interface GLView : NSOpenGLView @end
@implementation GLView { GLuint prog, vao, vbo, bvao, bvbo; int nv; GLint uMVP, uM, uCol; }
static GLuint sh(GLenum t, const char *src) { GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, NULL); glCompileShader(s); return s; }
- (void)prepareOpenGL
{
    GLint one = getenv("SWAP0") ? 0 : 1; [[self openGLContext] setValues:&one forParameter:NSOpenGLContextParameterSwapInterval];   // SWAP0=1: no vsync
    const char *vs = "#version 150\nin vec3 p; in vec3 n; uniform mat4 mvp, m; out vec3 wn; out vec3 wp;\n"
                     "void main(){ wn = mat3(m)*n; wp = (m*vec4(p,1)).xyz; gl_Position = mvp*vec4(p,1); }";
    const char *fs = "#version 150\nin vec3 wn; in vec3 wp; uniform vec3 col; out vec4 o;\n"
                     "void main(){ vec3 N=normalize(wn), L=normalize(vec3(0.4,1.0,0.3)), Vd=normalize(vec3(0,6,8)-wp), H=normalize(L+Vd);\n"
                     " float d=max(dot(N,L),0.0), s=pow(max(dot(N,H),0.0),48.0); o=vec4(col*(0.18+0.82*d)+vec3(0.6)*s,1); }";
    prog = glCreateProgram(); glAttachShader(prog, sh(GL_VERTEX_SHADER, vs)); glAttachShader(prog, sh(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(prog, 0, "p"); glBindAttribLocation(prog, 1, "n"); glLinkProgram(prog);
    uMVP = glGetUniformLocation(prog, "mvp"); uM = glGetUniformLocation(prog, "m"); uCol = glGetUniformLocation(prog, "col");
    V *v = lathe(96, &nv);
    glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(V) * nv, v, GL_STATIC_DRAW); free(v);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, 0, sizeof(V), 0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, 0, sizeof(V), (void *)12);
    const V board[6] = { {{-4,0,-4},{0,1,0}}, {{4,0,-4},{0,1,0}}, {{4,0,4},{0,1,0}}, {{-4,0,-4},{0,1,0}}, {{4,0,4},{0,1,0}}, {{-4,0,4},{0,1,0}} };
    glGenVertexArrays(1, &bvao); glBindVertexArray(bvao); glGenBuffers(1, &bvbo); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof board, board, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, 0, sizeof(V), 0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, 0, sizeof(V), (void *)12);
    glEnable(GL_DEPTH_TEST); glEnable(GL_MULTISAMPLE);
    [NSTimer scheduledTimerWithTimeInterval:0 repeats:YES block:^(NSTimer *t) { [self setNeedsDisplay:YES]; }];
}
static void mul(float *o, const float *a, const float *b) { float r[16]; for (int c = 0; c < 4; c++) for (int i = 0; i < 4; i++) { r[c*4+i] = 0; for (int k = 0; k < 4; k++) r[c*4+i] += a[k*4+i] * b[c*4+k]; } memcpy(o, r, sizeof r); }
- (void)drawRect:(NSRect)r
{
    const NSSize px = [self convertRectToBacking:self.bounds].size; glViewport(0, 0, px.width, px.height);
    glClearColor(0.12f, 0.12f, 0.14f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glUseProgram(prog);
    const float t = now_ms() / 1000.0f, ang = t * 0.6f, asp = px.width / px.height, f = 1 / tanf(0.45f), zn = 0.5f, zf = 40;
    const float P[16] = { f / asp,0,0,0, 0,f,0,0, 0,0,(zf+zn)/(zn-zf),-1, 0,0,2*zf*zn/(zn-zf),0 };
    const float ex = 9 * sinf(ang), ez = 9 * cosf(ang), ey = 6;           // camera circles the board, looking at its centre
    float fx = -ex, fy = -ey, fz = -ez, fl = sqrtf(fx*fx+fy*fy+fz*fz); fx /= fl; fy /= fl; fz /= fl;
    float sx = fy*0 - fz*1, sy = fz*0 - fx*0, sz = fx*1 - fy*0, sl = sqrtf(sx*sx+sy*sy+sz*sz); sx /= sl; sy /= sl; sz /= sl;
    const float ux = sy*fz - sz*fy, uy = sz*fx - sx*fz, uz = sx*fy - sy*fx;
    const float Vw[16] = { sx,ux,-fx,0, sy,uy,-fy,0, sz,uz,-fz,0, -(sx*ex+sy*ey+sz*ez), -(ux*ex+uy*ey+uz*ez), fx*ex+fy*ey+fz*ez, 1 };
    float PV[16]; mul(PV, P, Vw);
    const float Id[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    glUniformMatrix4fv(uMVP, 1, 0, PV); glUniformMatrix4fv(uM, 1, 0, Id); glUniform3f(uCol, 0.45f, 0.32f, 0.2f);
    glBindVertexArray(bvao); glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(vao);
    for (int i = 0; i < 32; i++) {
        const float x = -3.5f + (i % 8), z = i < 16 ? -3.5f + (i / 8) : 2.5f + ((i - 16) / 8), s = (i % 8 == 3 || i % 8 == 4) ? 1.3f : 1.0f;
        const float M[16] = { s,0,0,0, 0,s,0,0, 0,0,s,0, x,0,z,1 }; float MVP[16]; mul(MVP, PV, M);
        glUniformMatrix4fv(uMVP, 1, 0, MVP); glUniformMatrix4fv(uM, 1, 0, M);
        if (i < 16) glUniform3f(uCol, 0.92f, 0.9f, 0.85f); else glUniform3f(uCol, 0.15f, 0.15f, 0.17f);
        glDrawArrays(GL_TRIANGLES, 0, nv);
    }
    [[self openGLContext] flushBuffer];
    frame_mark();
}
@end

// ---- SceneKit ----
@interface SCNDelegate : NSObject <SCNSceneRendererDelegate> @end
@implementation SCNDelegate
- (void)renderer:(id<SCNSceneRenderer>)r didRenderScene:(SCNScene *)s atTime:(NSTimeInterval)t { dispatch_async(dispatch_get_main_queue(), ^{ frame_mark(); }); }
@end

static SCNView *scn_view(NSRect fr, SCNDelegate *d)
{
    SCNView *v = [[SCNView alloc] initWithFrame:fr options:@{ SCNPreferredRenderingAPIKey: @(SCNRenderingAPIMetal) }];
    v.antialiasingMode = SCNAntialiasingModeMultisampling4X; v.preferredFramesPerSecond = 120; v.rendersContinuously = YES;
    v.backgroundColor = [NSColor colorWithWhite:0.12 alpha:1]; v.delegate = d;
    SCNScene *sc = [SCNScene scene]; v.scene = sc;
    SCNBox *b = [SCNBox boxWithWidth:8 height:0.2 length:8 chamferRadius:0.05];
    b.firstMaterial.lightingModelName = SCNLightingModelPhysicallyBased; b.firstMaterial.diffuse.contents = [NSColor brownColor]; b.firstMaterial.roughness.contents = @0.4;
    SCNNode *bn = [SCNNode nodeWithGeometry:b]; bn.position = SCNVector3Make(0, -0.1, 0); [sc.rootNode addChildNode:bn];
    int nv; V *mv = lathe(96, &nv);
    NSMutableData *pos = [NSMutableData dataWithLength:12 * nv], *nor = [NSMutableData dataWithLength:12 * nv];
    for (int i = 0; i < nv; i++) { memcpy((char *)pos.mutableBytes + 12 * i, mv[i].p, 12); memcpy((char *)nor.mutableBytes + 12 * i, mv[i].n, 12); }
    free(mv);
    SCNGeometrySource *ps = [SCNGeometrySource geometrySourceWithData:pos semantic:SCNGeometrySourceSemanticVertex vectorCount:nv floatComponents:YES componentsPerVector:3 bytesPerComponent:4 dataOffset:0 dataStride:12];
    SCNGeometrySource *ns = [SCNGeometrySource geometrySourceWithData:nor semantic:SCNGeometrySourceSemanticNormal vectorCount:nv floatComponents:YES componentsPerVector:3 bytesPerComponent:4 dataOffset:0 dataStride:12];
    NSMutableData *idx = [NSMutableData dataWithLength:4 * nv]; for (int i = 0; i < nv; i++) ((uint32_t *)idx.mutableBytes)[i] = i;
    SCNGeometryElement *el = [SCNGeometryElement geometryElementWithData:idx primitiveType:SCNGeometryPrimitiveTypeTriangles primitiveCount:nv / 3 bytesPerIndex:4];
    for (int i = 0; i < 32; i++) {
        SCNGeometry *g = [SCNGeometry geometryWithSources:@[ps, ns] elements:@[el]];
        SCNMaterial *m = [SCNMaterial material]; m.lightingModelName = SCNLightingModelPhysicallyBased;
        m.diffuse.contents = i < 16 ? [NSColor colorWithWhite:0.9 alpha:1] : [NSColor colorWithWhite:0.15 alpha:1];
        m.roughness.contents = @0.3; m.metalness.contents = @0.1; g.materials = @[m];
        SCNNode *n = [SCNNode nodeWithGeometry:g];
        const float x = -3.5f + (i % 8), z = i < 16 ? -3.5f + (i / 8) : 2.5f + ((i - 16) / 8), s = (i % 8 == 3 || i % 8 == 4) ? 1.3f : 1.0f;
        n.position = SCNVector3Make(x, 0, z); n.scale = SCNVector3Make(s, s, s); n.castsShadow = YES; [sc.rootNode addChildNode:n];
    }
    SCNLight *l = [SCNLight light]; l.type = SCNLightTypeDirectional; l.castsShadow = YES; l.shadowMapSize = CGSizeMake(2048, 2048); l.intensity = 1000;
    SCNNode *ln = [SCNNode node]; ln.light = l; ln.eulerAngles = SCNVector3Make(-1.0, 0.4, 0); [sc.rootNode addChildNode:ln];
    SCNLight *amb = [SCNLight light]; amb.type = SCNLightTypeAmbient; amb.intensity = 300; SCNNode *an = [SCNNode node]; an.light = amb; [sc.rootNode addChildNode:an];
    SCNNode *pivot = [SCNNode node]; [sc.rootNode addChildNode:pivot];
    SCNNode *cam = [SCNNode node]; cam.camera = [SCNCamera camera]; cam.position = SCNVector3Make(0, 6, 9); cam.eulerAngles = SCNVector3Make(-0.58, 0, 0); [pivot addChildNode:cam];
    [pivot runAction:[SCNAction repeatActionForever:[SCNAction rotateByX:0 y:2 * M_PI z:0 duration:10]]];
    v.pointOfView = cam;
    return v;
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        g_mode = argc > 1 ? @(argv[1]) : @"gl"; if (argc > 2) g_secs = atof(argv[2]); if (argc > 4) { g_w = atoi(argv[3]); g_h = atoi(argv[4]); }
        g_cap = (int)(g_secs * 400) + 16; g_ft = calloc(g_cap, sizeof *g_ft);
        [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSRect fr = NSMakeRect(0, 0, g_w, g_h);
        NSWindow *w = [[NSWindow alloc] initWithContentRect:fr styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        w.title = [NSString stringWithFormat:@"scenetest %@", g_mode];
        static SCNDelegate *d;
        if ([g_mode isEqualToString:@"scn"]) { d = [SCNDelegate new]; w.contentView = scn_view(fr, d); }
        else {
            NSOpenGLPixelFormatAttribute at[] = { NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core, NSOpenGLPFADoubleBuffer, NSOpenGLPFAAccelerated,
                NSOpenGLPFAColorSize, 24, NSOpenGLPFADepthSize, 24, NSOpenGLPFAMultisample, NSOpenGLPFASampleBuffers, 1, NSOpenGLPFASamples, 4, 0 };
            GLView *v = [[GLView alloc] initWithFrame:fr pixelFormat:[[NSOpenGLPixelFormat alloc] initWithAttributes:at]];
            if (!v) { printf("gl: no pixel format\n"); return 1; }
            v.wantsBestResolutionOpenGLSurface = YES; w.contentView = v;
        }
        [w center]; [w makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    return 0;
}
