/*
 * vidwall -- 把视频播放成桌面壁纸 (X11 / GNOME)
 *
 * 目标:
 *   - 只覆盖主显示器
 *   - 静音
 *   - 鼠标点击穿透, 永不获取输入焦点
 *   - 一直播放, 不暂停
 *
 * 做法:
 *   本程序只负责"造一个正确的窗口", 解码和渲染全部交给 mpv 子进程,
 *   通过 mpv 的 --wid 把画面嵌进这个窗口.
 *
 *   窗口是 _NET_WM_WINDOW_TYPE_DESKTOP 类型的托管窗口, Mutter 会把它
 *   放进桌面层(所有普通窗口之下), 并且不会给它焦点.
 *   鼠标穿透靠清空 XShape 输入区域实现, 由 X server 直接强制, 与合成器无关.
 *
 * 仅支持 X11. Wayland 下无法把自己贴到桌面层.
 */

#define _GNU_SOURCE

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define APP_NAME "vidwall"

static volatile sig_atomic_t g_quit = 0;
static int g_verbose = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s [选项] <视频文件>\n"
        "\n"
        "选项:\n"
        "  --hwdec=NAME   硬件解码方式 (默认 nvdec; 可选 auto-safe / no)\n"
        "  --vo=NAME      视频输出 (默认 gpu; 可选 gpu-next / x11)\n"
        "  --no-loop      不循环播放\n"
        "  --below        额外设置 _NET_WM_STATE_BELOW (会跑到图标上面, 一般别用)\n"
        "  --verbose      显示 mpv 的输出\n"
        "  --keep-window  mpv 退出后保留窗口 (调试窗口层叠用)\n"
        "  --override-redirect  用非托管窗口 (层叠实验用)\n"
        "  -h, --help     显示本帮助\n"
        "\n"
        "本程序只支持 X11 会话, 只覆盖主显示器, 静音, 鼠标穿透, 不抢焦点.\n"
        "不带视频参数时, 会读取 ~/.config/vidwall/config 里的 video= 设置.\n",
        prog);
}

/* ---------- 窗口属性小工具 ---------- */

static void set_atom_prop(Display *dpy, Window w, const char *name, Atom type,
                          int format, const void *data, int count)
{
    Atom prop = XInternAtom(dpy, name, False);
    XChangeProperty(dpy, w, prop, type, format, PropModeReplace,
                    (const unsigned char *)data, count);
}

/* 把一个 atom 列表写进某个属性 (例如 _NET_WM_STATE) */
static void set_atom_list(Display *dpy, Window w, const char *name,
                          const char *const *names, int n)
{
    Atom atoms[8];

    if (n > (int)(sizeof atoms / sizeof atoms[0]))
        n = (int)(sizeof atoms / sizeof atoms[0]);

    for (int i = 0; i < n; i++)
        atoms[i] = XInternAtom(dpy, names[i], False);

    set_atom_prop(dpy, w, name, XA_ATOM, 32, atoms, n);
}

static void set_cardinal(Display *dpy, Window w, const char *name, unsigned long value)
{
    long v = (long)value;
    set_atom_prop(dpy, w, name, XA_CARDINAL, 32, &v, 1);
}

static void set_utf8(Display *dpy, Window w, const char *name, const char *value)
{
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom prop = XInternAtom(dpy, name, False);

    XChangeProperty(dpy, w, prop, utf8, 8, PropModeReplace,
                    (const unsigned char *)value, (int)strlen(value));
}

/* 没有标题栏, 没有边框 */
static void set_no_decorations(Display *dpy, Window w)
{
    struct {
        unsigned long flags;
        unsigned long functions;
        unsigned long decorations;
        long          input_mode;
        unsigned long status;
    } mwm;

    memset(&mwm, 0, sizeof mwm);
    mwm.flags = 2;        /* MWM_HINTS_DECORATIONS */
    mwm.decorations = 0;

    Atom prop = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    XChangeProperty(dpy, w, prop, prop, 32, PropModeReplace,
                    (const unsigned char *)&mwm, 5);
}

/*
 * 让窗口永远拿不到鼠标和键盘:
 *   1) WM_HINTS.input = False  -- 告诉窗口管理器别把键盘焦点给它
 *   2) 输入形状清空           -- X server 层面直接把鼠标事件放行到下层
 * 注意只动 ShapeInput, 绝不能动 ShapeBounding, 否则窗口会变不可见.
 */
static void make_click_through(Display *dpy, Window w, int have_shape)
{
    XWMHints hints;

    memset(&hints, 0, sizeof hints);
    hints.flags = InputHint;
    hints.input = False;
    XSetWMHints(dpy, w, &hints);

    if (have_shape) {
        XRectangle dummy;
        memset(&dummy, 0, sizeof dummy);
        XShapeCombineRectangles(dpy, w, ShapeInput, 0, 0, &dummy, 0,
                                ShapeSet, Unsorted);
    }

    XFlush(dpy);
}

/*
 * 设置 _NET_WM_STATE. 必须能重复调用: 窗口 map 之后 Mutter 会往这个属性里
 * 塞 _NET_WM_STATE_DEMANDS_ATTENTION, 我们过一秒再写一次把它盖掉.
 */
static void apply_wm_state(Display *dpy, Window w, int below)
{
    const char *states[4];
    int n = 0;

    states[n++] = "_NET_WM_STATE_SKIP_TASKBAR";
    states[n++] = "_NET_WM_STATE_SKIP_PAGER";
    if (below)
        states[n++] = "_NET_WM_STATE_BELOW";

    set_atom_list(dpy, w, "_NET_WM_STATE", states, n);
}

/* 读取当前活动窗口(EWMH), 没有则返回 None */
static Window get_active_window(Display *dpy, Window root)
{
    Atom prop = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    Atom actual;
    int fmt;
    unsigned long n, rest;
    unsigned char *p = NULL;
    Window ret = None;

    if (XGetWindowProperty(dpy, root, prop, 0, 1, False, XA_WINDOW,
                           &actual, &fmt, &n, &rest, &p) == Success && p) {
        if (n > 0)
            ret = *(Window *)p;
        XFree(p);
    }
    return ret;
}

/* 设置窗口不透明度 (0 = 全透明, 0xffffffff = 完全不透明) */
static void set_opacity(Display *dpy, Window w, unsigned long value)
{
    set_cardinal(dpy, w, "_NET_WM_WINDOW_OPACITY", value);
    XFlush(dpy);
}

/* 请求窗口管理器激活(抬升)某个窗口 */
static void request_activate(Display *dpy, Window root, Window w)
{
    XEvent ev;

    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2;   /* source indication: 工具程序 */
    ev.xclient.data.l[1] = 0;   /* timestamp */
    ev.xclient.data.l[2] = 0;   /* requestor */
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

/*
 * 保证自己排在桌面图标窗口(ding)的下方.
 *
 * 做法是"先检测, 再纠正", 而不是无脑发请求:
 *   - XQueryTree 返回的顺序是从下到上, 索引越大越靠上
 *   - 只有当自己索引 >= 图标索引(即自己压在图标上面)时才动手
 *   - 已经对了就什么都不做, 避免和 Mutter 来回拉锯
 *
 * 关键点: 不要试图调整"我们自己的"位置. 无论 XLowerWindow 还是
 * XConfigureWindow(我们, Below, sibling=图标窗口), 都可能被 Mutter 排回去,
 * 实测时灵时不灵.
 *
 * 可靠的做法是反过来动图标窗口: 明确告诉 X 服务器"把图标窗口排在我们上面"
 * (XConfigureWindow(图标, Above, sibling=我们)). 实测 1 秒内生效且稳定.
 * 这也正是用户点击桌面时 DING 自己做的事.
 *
 * 返回: 1 = 刚刚调整过, 0 = 本来就在下面(无需动作), -1 = 没找到图标窗口
 */
static int ensure_below_desktop(Display *dpy, Window root, Window win)
{
    Window r, parent, *kids = NULL;
    unsigned int n = 0;
    Window target = None;
    Atom type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom desktop_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    long our_idx = -1, tgt_idx = -1;

    if (!XQueryTree(dpy, root, &r, &parent, &kids, &n))
        return -1;

    for (unsigned int i = 0; i < n; i++) {
        XWindowAttributes wa;
        Atom actual;
        int fmt;
        unsigned long cnt, rest;
        unsigned char *prop = NULL;

        if (kids[i] == win) {
            our_idx = (long)i;
            continue;
        }
        if (!XGetWindowAttributes(dpy, kids[i], &wa) || wa.map_state != IsViewable)
            continue;
        if (XGetWindowProperty(dpy, kids[i], type_atom, 0, 8, False, XA_ATOM,
                               &actual, &fmt, &cnt, &rest, &prop) != Success || !prop)
            continue;
        if (fmt == 32 && cnt > 0) {
            Atom *a = (Atom *)prop;
            for (unsigned long k = 0; k < cnt; k++)
                if (a[k] == desktop_atom) {
                    target = kids[i];    /* 取最后一个, 即最靠上的那个 */
                    tgt_idx = (long)i;
                }
        }
        XFree(prop);
    }

    if (kids)
        XFree(kids);

    if (target == None)
        return -1;

    /* 已经排在图标下面了, 什么都不用做 */
    if (our_idx >= 0 && our_idx < tgt_idx)
        return 0;

    /*
     * 抬升图标窗口.
     *
     * 试过但无效的手段: XLowerWindow, XConfigureWindow(改自己或改图标窗口),
     * _NET_RESTACK_WINDOW —— Mutter 对托管窗口的层叠请求一概不理.
     *
     * 唯一有效的是走"激活"这条路: Mutter 在激活窗口时会把它抬到所在层的
     * 顶端. 图标窗口(DING)属于 DESKTOP 层, 激活它就会排到我们上面.
     * 代价是焦点会跟着跑过去, 所以紧接着把焦点还给用户原来的窗口.
     */
    {
        Window prev = get_active_window(dpy, root);

        request_activate(dpy, root, target);
        usleep(200000);          /* 等 Mutter 处理完这次激活 */

        if (prev != None && prev != target)
            request_activate(dpy, root, prev);
        XFlush(dpy);
    }
    return 1;
}

/* ---------- 显示器几何 ---------- */

typedef struct {
    int x, y, w, h;
} Rect;

/* 取主显示器(Mutter 认为的主输出)的几何; 失败则退回整个屏幕 */
static Rect primary_monitor(Display *dpy, Window root, int screen)
{
    Rect r;
    int major = 0, minor = 0;
    XRRScreenResources *res;
    XRROutputInfo *oinfo = NULL;
    XRRCrtcInfo *cinfo = NULL;
    RROutput primary;

    r.x = 0;
    r.y = 0;
    r.w = DisplayWidth(dpy, screen);
    r.h = DisplayHeight(dpy, screen);

    if (!XRRQueryVersion(dpy, &major, &minor))
        return r;

    res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res)
        return r;

    primary = XRRGetOutputPrimary(dpy, root);
    if (primary != None)
        oinfo = XRRGetOutputInfo(dpy, res, primary);

    if ((!oinfo || oinfo->crtc == None) && res->noutput > 0) {
        if (oinfo) {
            XRRFreeOutputInfo(oinfo);
            oinfo = NULL;
        }
        for (int i = 0; i < res->noutput; i++) {
            XRROutputInfo *o = XRRGetOutputInfo(dpy, res, res->outputs[i]);
            if (o && o->connection == RR_Connected && o->crtc != None) {
                oinfo = o;
                break;
            }
            if (o)
                XRRFreeOutputInfo(o);
        }
    }

    if (oinfo && oinfo->crtc != None) {
        cinfo = XRRGetCrtcInfo(dpy, res, oinfo->crtc);
        if (cinfo) {
            r.x = cinfo->x;
            r.y = cinfo->y;
            r.w = (int)cinfo->width;
            r.h = (int)cinfo->height;
            XRRFreeCrtcInfo(cinfo);
        }
    }

    if (oinfo)
        XRRFreeOutputInfo(oinfo);
    XRRFreeScreenResources(res);

    return r;
}

/* 单调毫秒时钟, 用于节流层叠复查 */
static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- 配置文件 ---------- */

static char cfg_video[1024];
static char cfg_hwdec[64];
static char cfg_vo[64];
static int  cfg_loop = 1;
static int  cfg_have_video = 0;

/* 去掉首尾空白, 返回指向内容开头的指针 */
static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
    return s;
}

/*
 * 读取 ~/.config/vidwall/config, 格式:
 *     video=/path/to/video.mp4
 *     hwdec=nvdec
 *     vo=gpu
 *     loop=yes
 * 命令行参数优先级更高, 会覆盖这里读到的值.
 */
static void load_config(void)
{
    char path[512];
    char line[1200];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    FILE *f;

    if (xdg && *xdg)
        snprintf(path, sizeof path, "%s/vidwall/config", xdg);
    else if (home && *home)
        snprintf(path, sizeof path, "%s/.config/vidwall/config", home);
    else
        return;

    f = fopen(path, "r");
    if (!f)
        return;

    while (fgets(line, sizeof line, f)) {
        char *key, *val, *eq;

        key = trim(line);
        if (*key == '\0' || *key == '#')
            continue;

        eq = strchr(key, '=');
        if (!eq)
            continue;
        *eq = '\0';
        val = trim(eq + 1);
        key = trim(key);

        if (!strcmp(key, "video")) {
            snprintf(cfg_video, sizeof cfg_video, "%s", val);
            cfg_have_video = (val[0] != '\0');
        } else if (!strcmp(key, "hwdec")) {
            snprintf(cfg_hwdec, sizeof cfg_hwdec, "%s", val);
        } else if (!strcmp(key, "vo")) {
            snprintf(cfg_vo, sizeof cfg_vo, "%s", val);
        } else if (!strcmp(key, "loop")) {
            cfg_loop = !(val[0] == 'n' || val[0] == 'N' || val[0] == '0');
        }
    }
    fclose(f);
}

/* ---------- 启动 mpv ---------- */

static pid_t spawn_mpv(Window win, const char *video, const char *hwdec,
                       const char *vo, int loop, int verbose)
{
    static char wid_str[32];
    char hwdec_arg[96];
    char vo_arg[96];
    char loop_arg[32];
    const char *args[32];
    int n = 0;
    pid_t pid;

    snprintf(wid_str, sizeof wid_str, "--wid=%lu", (unsigned long)win);
    snprintf(hwdec_arg, sizeof hwdec_arg, "--hwdec=%s", hwdec);
    snprintf(vo_arg, sizeof vo_arg, "--vo=%s", vo);
    snprintf(loop_arg, sizeof loop_arg, "--loop-file=%s", loop ? "inf" : "no");

    args[n++] = "mpv";
    args[n++] = "--no-config";
    args[n++] = wid_str;
    args[n++] = "--no-audio";
    args[n++] = "--no-osc";
    args[n++] = "--input-default-bindings=no";
    args[n++] = "--input-vo-keyboard=no";
    args[n++] = loop_arg;
    args[n++] = hwdec_arg;
    args[n++] = vo_arg;
    if (!strcmp(vo, "gpu") || !strcmp(vo, "gpu-next"))
        args[n++] = "--gpu-api=opengl";
    args[n++] = "--panscan=1.0";          /* 铺满屏幕, 不留黑边 */
    if (!verbose)
        args[n++] = "--really-quiet";
    args[n++] = "--";
    args[n++] = video;
    args[n] = NULL;

    pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        pid_t parent = getppid();

        /* 父进程没了就跟着走, 避免留下野进程 */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)
            _exit(127);

        execvp("mpv", (char *const *)args);
        fprintf(stderr, APP_NAME ": 无法启动 mpv: %s\n", strerror(errno));
        _exit(127);
    }

    return pid;
}

/* ---------- 主程序 ---------- */

int main(int argc, char **argv)
{
    const char *video = NULL;
    const char *hwdec = "nvdec";
    const char *vo = "gpu";
    int loop = 1;
    int below = 0;
    int verbose = 0;
    int keep_window = 0;
    int override_redirect = 0;

    Display *dpy;
    Window root, win;
    int screen;
    int xfd;
    int have_shape;
    int have_randr;
    int rr_event_base = 0, rr_error_base = 0;
    int shape_event_base = 0, shape_error_base = 0;
    XSetWindowAttributes attrs;
    XClassHint class_hint;
    Rect rect;
    pid_t mpv_pid;
    int tick = 0;
    int warned_no_icons = 0;
    long long start_ms = 0, last_check_ms = 0, mpv_start_ms = 0;
    int opacity_done = 0;
    int exit_code = 0;

    load_config();
    if (cfg_have_video)
        video = cfg_video;
    if (cfg_hwdec[0])
        hwdec = cfg_hwdec;
    if (cfg_vo[0])
        vo = cfg_vo;
    loop = cfg_loop;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strncmp(a, "--hwdec=", 8)) {
            hwdec = a + 8;
        } else if (!strncmp(a, "--vo=", 5)) {
            vo = a + 5;
        } else if (!strcmp(a, "--no-loop")) {
            loop = 0;
        } else if (!strcmp(a, "--below")) {
            below = 1;
        } else if (!strcmp(a, "--verbose")) {
            verbose = 1;
            g_verbose = 1;
        } else if (!strcmp(a, "--keep-window")) {
            keep_window = 1;
        } else if (!strcmp(a, "--override-redirect")) {
            override_redirect = 1;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, APP_NAME ": 未知选项 %s\n", a);
            usage(argv[0]);
            return 2;
        } else {
            video = a;
        }
    }

    if (!video) {
        fprintf(stderr,
                APP_NAME ": 没有指定视频, 也没找到配置文件.\n\n"
                "用法一: %s <视频文件>\n"
                "用法二: 把视频路径写进 ~/.config/vidwall/config, 例如\n"
                "            video=/home/jun/视频/我的壁纸.mp4\n"
                "        然后直接运行 %s 即可, 这条路径也是开机自启用的.\n",
                argv[0], argv[0]);
        return 2;
    }

    {
        struct sigaction sa;

        memset(&sa, 0, sizeof sa);
        sa.sa_handler = handle_signal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        const char *disp = getenv("DISPLAY");
        fprintf(stderr,
                APP_NAME ": 无法连接 X 服务器 (DISPLAY=%s)\n"
                "本程序只支持 X11 会话. 如果你现在跑在 Wayland 下, 这个方案不适用.\n",
                disp ? disp : "(未设置)");
        return 1;
    }

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    have_shape = XShapeQueryExtension(dpy, &shape_event_base, &shape_error_base);
    if (!have_shape)
        fprintf(stderr, APP_NAME ": 警告: 服务器不支持 XShape, 鼠标穿透只能靠 WM_HINTS.\n");

    have_randr = XRRQueryExtension(dpy, &rr_event_base, &rr_error_base);
    if (have_randr) {
        int major = 0, minor = 0;

        XRRQueryVersion(dpy, &major, &minor);
        XRRSelectInput(dpy, root,
                       RRScreenChangeNotifyMask |
                       RROutputChangeNotifyMask |
                       RRCrtcChangeNotifyMask);
    }

    rect = primary_monitor(dpy, root, screen);

    memset(&attrs, 0, sizeof attrs);
    attrs.background_pixel = BlackPixel(dpy, screen);
    attrs.override_redirect = override_redirect ? True : False;
    attrs.event_mask = StructureNotifyMask | ExposureMask;

    win = XCreateWindow(dpy, root, rect.x, rect.y, rect.w, rect.h, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWBackPixel | CWOverrideRedirect | CWEventMask, &attrs);
    if (!win) {
        fprintf(stderr, APP_NAME ": 创建窗口失败\n");
        XCloseDisplay(dpy);
        return 1;
    }

    /* 属性必须在 map 之前设好, 这样 WM 第一次看到的就是"桌面窗口" */
    {
        const char *type = "_NET_WM_WINDOW_TYPE_DESKTOP";

        set_atom_list(dpy, win, "_NET_WM_WINDOW_TYPE", &type, 1);
    }

    apply_wm_state(dpy, win, below);

    set_cardinal(dpy, win, "_NET_WM_DESKTOP", 0xFFFFFFFFUL);  /* 所有工作区都显示 */
    set_cardinal(dpy, win, "_NET_WM_USER_TIME", 0);           /* 不要触发抢焦点 */
    set_cardinal(dpy, win, "_NET_WM_PID", (unsigned long)getpid());
    set_utf8(dpy, win, "_NET_WM_NAME", APP_NAME);
    set_utf8(dpy, win, "WM_NAME", APP_NAME);

    class_hint.res_name = (char *)APP_NAME;
    class_hint.res_class = (char *)"Vidwall";
    XSetClassHint(dpy, win, &class_hint);

    set_no_decorations(dpy, win);
    make_click_through(dpy, win, have_shape);

    /*
     * 先设成全透明再 map. 从 map 到 mpv 真正出画之间有几百毫秒, 窗口背景
     * 是黑的, 直接露出来会闪一下. 透明过渡掉这段, 等 mpv 稳了再恢复.
     */
    set_opacity(dpy, win, 0);

    XMapWindow(dpy, win);
    XFlush(dpy);

    /*
     * 启动阶段密集收敛层叠.
     * 放在 mpv 出画之前, 这样用户不会看到图标被盖住的那一瞬间.
     * Mutter 不是一次就接受重排, 通常要连试几次, 所以这里小步快跑.
     */
    for (int i = 0; i < 100; i++) {
        int rc;

        usleep(20000);
        rc = ensure_below_desktop(dpy, root, win);
        if (rc != 1)
            break;          /* 0=已经对了, -1=没有桌面图标窗口 */
    }

    mpv_pid = spawn_mpv(win, video, hwdec, vo, loop, verbose);
    if (mpv_pid < 0) {
        fprintf(stderr, APP_NAME ": fork 失败: %s\n", strerror(errno));
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return 1;
    }

    printf(APP_NAME ": 主屏 %dx%d @ (%d,%d), 窗口 0x%lx, mpv pid %d\n",
           rect.w, rect.h, rect.x, rect.y, (unsigned long)win, (int)mpv_pid);
    fflush(stdout);

    xfd = ConnectionNumber(dpy);
    start_ms = now_ms();
    mpv_start_ms = start_ms;
    last_check_ms = start_ms;
    ensure_below_desktop(dpy, root, win);   /* map 后先纠一次, 尽量缩短被盖的时间 */

    while (!g_quit) {
        fd_set fds;
        struct timeval tv;
        int n;

        while (XPending(dpy)) {
            XEvent ev;

            XNextEvent(dpy, &ev);

            if (have_randr && ev.type == rr_event_base + RRScreenChangeNotify) {
                Rect nr;

                XRRUpdateConfiguration(&ev);
                nr = primary_monitor(dpy, root, screen);
                if (nr.x != rect.x || nr.y != rect.y ||
                    nr.w != rect.w || nr.h != rect.h) {
                    rect = nr;
                    XMoveResizeWindow(dpy, win, rect.x, rect.y, rect.w, rect.h);
                    make_click_through(dpy, win, have_shape);
                    XFlush(dpy);
                    printf(APP_NAME ": 分辨率变化, 主屏 %dx%d @ (%d,%d)\n",
                           rect.w, rect.h, rect.x, rect.y);
                    fflush(stdout);
                }
            } else if (have_randr && ev.type == rr_event_base + RRNotify) {
                Rect nr = primary_monitor(dpy, root, screen);

                if (nr.x != rect.x || nr.y != rect.y ||
                    nr.w != rect.w || nr.h != rect.h) {
                    rect = nr;
                    XMoveResizeWindow(dpy, win, rect.x, rect.y, rect.w, rect.h);
                    make_click_through(dpy, win, have_shape);
                    XFlush(dpy);
                }
            }
        }

        if (!keep_window) {
            int status;
            pid_t w = waitpid(mpv_pid, &status, WNOHANG);

            if (w == mpv_pid) {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
                    fprintf(stderr, APP_NAME ": mpv 启动失败, 请确认已安装 mpv (sudo apt install mpv)\n");
                else if (WIFSIGNALED(status))
                    fprintf(stderr, APP_NAME ": mpv 被信号 %d 终止\n", WTERMSIG(status));
                exit_code = 1;
                break;
            }
        }

        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;   /* 100ms, 只用来做那个一次性 lower */

        n = select(xfd + 1, &fds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, APP_NAME ": select 失败: %s\n", strerror(errno));
            exit_code = 1;
            break;
        }

        if (!opacity_done && now_ms() - mpv_start_ms > 1200) {
            set_opacity(dpy, win, 0xFFFFFFFFUL);
            opacity_done = 1;
        }
        tick++;
        if (tick == 10) {
            /* map 后约 1 秒, 顺手盖掉 _NET_WM_STATE_DEMANDS_ATTENTION */
            apply_wm_state(dpy, win, below);
        }
        {
            long long now = now_ms();
            /* 启动前 3 秒密集复查(每 50ms), 之后放宽到每秒一次 */
            long long interval = (now - start_ms < 3000) ? 50 : 1000;

            if (now - last_check_ms >= interval) {
                int rc;

                last_check_ms = now;
                rc = ensure_below_desktop(dpy, root, win);
                if (rc == 1 && g_verbose)
                    fprintf(stderr, APP_NAME ": 层叠被改动, 已重新把图标排到壁纸之上\n");
                if (rc < 0 && !warned_no_icons) {
                    if (g_verbose)
                        fprintf(stderr, APP_NAME ": 没找到桌面图标窗口, 跳过层叠调整\n");
                    warned_no_icons = 1;
                }
            }
        }
    }

    if (mpv_pid > 0) {
        kill(mpv_pid, SIGTERM);
        waitpid(mpv_pid, NULL, 0);
    }

    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    return exit_code;
}
