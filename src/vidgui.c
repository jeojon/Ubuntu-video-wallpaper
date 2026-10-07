/*
 * vidgui -- vidwall 的图形界面
 *
 * 只做三件事:
 *   1. 读写 ~/.config/vidwall/config   (纯文本, 格式和 vidwall 共用)
 *   2. 起停 vidwall 进程
 *   3. 开关 ~/.config/autostart/vidwall.desktop
 *
 * 不碰 X11, 不碰窗口层叠 —— 那些是 vidwall 的事, 这里不重复实现.
 *
 * 编译: make  (需要 libgtk-3-dev, 系统已装)
 */

#define _GNU_SOURCE

#include <gtk/gtk.h>
#include <stdarg.h>

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define APP_NAME "vidgui"

static GtkWidget *lbl_video;
static GtkWidget *lbl_status;
static GtkWidget *btn_toggle;
static GtkWidget *chk_autostart;
static GtkWidget *cmb_hwdec;
static GtkWidget *cmb_vo;
static GtkWidget *chk_loop;
static GtkWidget *btn_choose;

static char cur_video[PATH_MAX] = "";

/* ---------- 路径 ---------- */

/* vidwall 和本程序同目录, 由 /proc/self/exe 反推 */
static const char *bin_path(void)
{
    static char *path = NULL;

    if (!path) {
        char *self = g_file_read_link("/proc/self/exe", NULL);
        char *dir = self ? g_path_get_dirname(self) : g_strdup(".");

        path = g_build_filename(dir, "vidwall", NULL);
        g_free(dir);
        g_free(self);
    }
    return path;
}

/* 都返回新分配的字符串, 调用方负责 g_free */
static char *conf_dir_new(void)
{
    return g_build_filename(g_get_user_config_dir(), "vidwall", NULL);
}

static char *conf_file_new(void)
{
    char *d = conf_dir_new();
    char *p = g_build_filename(d, "config", NULL);

    g_free(d);
    return p;
}

static char *autostart_file_new(void)
{
    return g_build_filename(g_get_user_config_dir(), "autostart",
                            "vidwall.desktop", NULL);
}

/* ---------- 进度 ---------- */

/* 在 /proc 里找一个叫 vidwall 的进程, 返回 pid, 没有返回 -1 */
static pid_t wall_pid(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;

    if (!d)
        return -1;

    while ((e = readdir(d))) {
        char path[PATH_MAX], comm[64];
        FILE *f;

        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(comm, sizeof comm, f)) {
            size_t l = strlen(comm);

            if (l && comm[l - 1] == '\n')
                comm[l - 1] = '\0';
            if (!strcmp(comm, "vidwall")) {
                pid_t p = (pid_t)atoi(e->d_name);

                fclose(f);
                closedir(d);
                return p;
            }
        }
        fclose(f);
    }
    closedir(d);
    return -1;
}

/* 脱离终端启动 vidwall, 关掉 GUI 后它继续跑 */
static int wall_start(void)
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0) {
        setsid();
        pid = fork();
        if (pid != 0)
            _exit(0);              /* 中间进程退出, 孙进程被 init 收养 */
        {
            int fd = open("/dev/null", O_WRONLY);

            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > 2)
                    close(fd);
            }
        }
        execl(bin_path(), "vidwall", (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
    return 0;
}

static void wall_stop(void)
{
    pid_t p = wall_pid();

    if (p > 0) {
        kill(p, SIGTERM);
        for (int i = 0; i < 30; i++) {
            usleep(100000);
            if (wall_pid() < 0)
                break;
        }
        p = wall_pid();
        if (p > 0)
            kill(p, SIGKILL);
    }
}

/* 弹一个简单的提示框 */
static void show_msg(GtkMessageType type, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    GtkWidget *m;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    m = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", buf);
    gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
}

/* vidwall 和本程序必须放在一起, 少了就没法干活, 得明确告诉用户 */
static int wall_bin_ok(void)
{
    if (g_file_test(bin_path(), G_FILE_TEST_IS_EXECUTABLE))
        return 1;
    show_msg(GTK_MESSAGE_ERROR,
             "找不到壁纸程序 vidwall。\n\n"
             "它必须和 vidgui 放在同一个文件夹里:\n"
             "%s\n\n"
             "请把整个文件夹一起拷贝过去, 不要只拿 vidgui 这一个文件。",
             bin_path());
    return 0;
}

/* ---------- 配置文件 ---------- */

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

static void config_load(char *video, size_t vsz, char *hwdec, size_t hsz,
                        char *vo, size_t osz, int *loop)
{
    char line[1200];
    char *path = conf_file_new();
    FILE *f = fopen(path, "r");

    video[0] = '\0';
    snprintf(hwdec, hsz, "nvdec");
    snprintf(vo, osz, "gpu");
    *loop = 1;
    if (!f) {
        g_free(path);
        return;
    }

    while (fgets(line, sizeof line, f)) {
        char *k, *v, *eq;

        k = trim(line);
        if (*k == '\0' || *k == '#')
            continue;
        eq = strchr(k, '=');
        if (!eq)
            continue;
        *eq = '\0';
        v = trim(eq + 1);
        k = trim(k);

        if (!strcmp(k, "video"))
            snprintf(video, vsz, "%s", v);
        else if (!strcmp(k, "hwdec"))
            snprintf(hwdec, hsz, "%s", v);
        else if (!strcmp(k, "vo"))
            snprintf(vo, osz, "%s", v);
        else if (!strcmp(k, "loop"))
            *loop = !(v[0] == 'n' || v[0] == 'N' || v[0] == '0');
    }
    fclose(f);
    g_free(path);
}

static void config_save(const char *video, const char *hwdec,
                        const char *vo, int loop)
{
    char *dir = conf_dir_new();
    char *path = conf_file_new();
    FILE *f;

    g_mkdir_with_parents(dir, 0755);
    f = fopen(path, "w");
    if (!f) {
        g_printerr("写配置失败: %s\n", path);
        g_free(dir);
        g_free(path);
        return;
    }
    fprintf(f, "# vidwall 配置 —— 由 vidgui 维护, 也可以手动改\n");
    fprintf(f, "video=%s\n", video);
    fprintf(f, "hwdec=%s\n", hwdec);
    fprintf(f, "vo=%s\n", vo);
    fprintf(f, "loop=%s\n", loop ? "yes" : "no");
    fclose(f);
    g_free(dir);
    g_free(path);
}

/* ---------- 开机自启 ---------- */

static int autostart_enabled(void)
{
    char *p = autostart_file_new();
    int on = g_file_test(p, G_FILE_TEST_EXISTS);

    g_free(p);
    return on;
}

static void autostart_set(int on)
{
    char *p = autostart_file_new();
    char *disabled = g_strconcat(p, ".disabled", NULL);

    if (on) {
        char *dir = g_path_get_dirname(p);
        char *bin = g_strdup(bin_path());
        char *body = g_strdup_printf(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=视频壁纸\n"
            "Comment=用 mpv 播放视频作为桌面壁纸 (vidwall)\n"
            "Exec=\"%s\"\n"
            "X-GNOME-Autostart-enabled=true\n"
            "X-GNOME-Autostart-Delay=3\n"
            "Terminal=false\n",
            bin);

        g_mkdir_with_parents(dir, 0755);
        g_file_set_contents(p, body, -1, NULL);
        g_free(body);
        g_free(bin);
        g_free(dir);
        unlink(disabled);
    } else {
        /* 改名而不是删除, 方便随时再打开 */
        if (rename(p, disabled) != 0)
            unlink(p);
    }

    g_free(disabled);
    g_free(p);
}

/* ---------- 界面 ---------- */

static void refresh_status(void)
{
    int run = wall_pid() > 0;

    if (run)
        gtk_label_set_markup(GTK_LABEL(lbl_status),
                             "<span foreground='#2e7d32'>● 运行中</span>");
    else
        gtk_label_set_markup(GTK_LABEL(lbl_status),
                             "<span foreground='#9e9e9e'>○ 已停止</span>");

    gtk_button_set_label(GTK_BUTTON(btn_toggle), run ? "停止" : "启动");
}

static void refresh_video_label(void)
{
    if (cur_video[0]) {
        char *esc = g_markup_escape_text(cur_video, -1);
        char *markup = g_strdup_printf("<span foreground='#333333'>%s</span>", esc);
        const char *base = strrchr(cur_video, '/');

        gtk_label_set_markup(GTK_LABEL(lbl_video), markup);
        gtk_widget_set_tooltip_text(lbl_video, cur_video);
        if (base)
            (void)base;
        g_free(markup);
        g_free(esc);
    } else {
        gtk_label_set_markup(GTK_LABEL(lbl_video),
                             "<span foreground='#9e9e9e'>(还没有选择视频)</span>");
    }
}

static void on_choose(GtkWidget *w, gpointer data)
{
    GtkWidget *dlg;
    GtkFileFilter *ff;

    (void)w;
    (void)data;

    dlg = gtk_file_chooser_dialog_new("选择壁纸视频", NULL,
                                      GTK_FILE_CHOOSER_ACTION_OPEN,
                                      "_取消", GTK_RESPONSE_CANCEL,
                                      "_选择", GTK_RESPONSE_ACCEPT, NULL);

    ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "视频文件");
    gtk_file_filter_add_pattern(ff, "*.mp4");
    gtk_file_filter_add_pattern(ff, "*.mkv");
    gtk_file_filter_add_pattern(ff, "*.webm");
    gtk_file_filter_add_pattern(ff, "*.mov");
    gtk_file_filter_add_pattern(ff, "*.avi");
    gtk_file_filter_add_pattern(ff, "*.m4v");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);
    ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "所有文件");
    gtk_file_filter_add_pattern(ff, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);

    if (cur_video[0]) {
        char dir[PATH_MAX];
        char *slash;

        snprintf(dir, sizeof dir, "%s", cur_video);
        slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir);
        }
    }

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));

        if (fn) {
            snprintf(cur_video, sizeof cur_video, "%s", fn);
            g_free(fn);
            refresh_video_label();
        }
    }
    gtk_widget_destroy(dlg);
}

static void on_toggle(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;

    if (wall_pid() > 0) {
        wall_stop();
    } else {
        if (!wall_bin_ok())
            return;
        wall_start();
    }
    refresh_status();
}

static void on_apply(GtkWidget *w, gpointer data)
{
    const char *hw, *vo;
    int loop;

    (void)w;
    (void)data;

    hw = gtk_combo_box_get_active_id(GTK_COMBO_BOX(cmb_hwdec));
    vo = gtk_combo_box_get_active_id(GTK_COMBO_BOX(cmb_vo));
    loop = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_loop));

    if (!cur_video[0]) {
        show_msg(GTK_MESSAGE_WARNING,
                 "还没有选择视频。\n先点“选择视频…”挑一个。");
        return;
    }
    if (!g_file_test(cur_video, G_FILE_TEST_IS_REGULAR)) {
        show_msg(GTK_MESSAGE_ERROR,
                 "视频文件不存在了:\n%s\n\n"
                 "可能被移动或删除了, 请重新选择。", cur_video);
        return;
    }

    if (!wall_bin_ok())
        return;

    config_save(cur_video, hw ? hw : "nvdec", vo ? vo : "gpu", loop);
    autostart_set(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_autostart)));

    wall_stop();
    wall_start();
    refresh_status();
    show_msg(GTK_MESSAGE_INFO, "已应用, 壁纸正在重新播放。");
}

/* 启动时先检查 vidwall 在不在, 缺了就早点告诉用户 */
static gboolean on_idle_check_bin(gpointer data)
{
    (void)data;
    if (!g_file_test(bin_path(), G_FILE_TEST_IS_EXECUTABLE))
        wall_bin_ok();     /* 内部会弹错误框 */
    return G_SOURCE_REMOVE;
}

/* 窗口显示完成后再处理焦点. 直接 grab_focus 在窗口实现之前调用会失效,
 * 而可选中的标签一旦拿到焦点就会被全选, 出现一块高亮, 很难看. */
static gboolean on_idle_focus(gpointer data)
{
    (void)data;
    gtk_label_select_region(GTK_LABEL(lbl_video), 0, 0);   /* 清掉选中 */
    gtk_widget_grab_focus(btn_choose);
    return G_SOURCE_REMOVE;
}

static gboolean on_tick(gpointer data)
{
    (void)data;
    refresh_status();
    return G_SOURCE_CONTINUE;
}

static void on_quit(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    gtk_main_quit();
}

/* ---------- 主程序 ---------- */

int main(int argc, char **argv)
{
    GtkWidget *win, *vbox, *frame, *box, *grid, *btn, *lbl, *sep;
    char video[PATH_MAX], hwdec[64], vo[64];
    int loop;

    gtk_init(&argc, &argv);

    config_load(video, sizeof video, hwdec, sizeof hwdec, vo, sizeof vo, &loop);
    snprintf(cur_video, sizeof cur_video, "%s", video);

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "视频壁纸");
    gtk_window_set_default_size(GTK_WINDOW(win), 520, -1);
    gtk_container_set_border_width(GTK_CONTAINER(win), 12);
    g_signal_connect(win, "destroy", G_CALLBACK(on_quit), NULL);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* --- 当前视频 --- */
    frame = gtk_frame_new("当前壁纸");
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(frame), box);

    lbl_video = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_video), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable(GTK_LABEL(lbl_video), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lbl_video), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), lbl_video, FALSE, FALSE, 0);

    btn_choose = gtk_button_new_with_label("选择视频…");
    g_signal_connect(btn_choose, "clicked", G_CALLBACK(on_choose), NULL);
    gtk_box_pack_start(GTK_BOX(box), btn_choose, FALSE, FALSE, 0);

    /* --- 运行状态 --- */
    frame = gtk_frame_new("运行状态");
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(frame), box);

    lbl_status = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(box), lbl_status, FALSE, FALSE, 0);
    lbl = gtk_label_new("   (壁纸在桌面图标和窗口之下, 不影响正常使用)");
    gtk_widget_set_sensitive(lbl, FALSE);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    btn_toggle = gtk_button_new_with_label("停止");
    g_signal_connect(btn_toggle, "clicked", G_CALLBACK(on_toggle), NULL);
    gtk_box_pack_end(GTK_BOX(box), btn_toggle, FALSE, FALSE, 0);

    /* --- 选项 --- */
    frame = gtk_frame_new("选项");
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(frame), box);

    chk_autostart = gtk_check_button_new_with_label("开机自动启动");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_autostart), autostart_enabled());
    gtk_box_pack_start(GTK_BOX(box), chk_autostart, FALSE, FALSE, 0);

    sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(box), sep, FALSE, FALSE, 2);

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    lbl = gtk_label_new("硬件解码");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 0, 1, 1);
    cmb_hwdec = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(cmb_hwdec), "nvdec", "nvdec (NVIDIA 硬件解码)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(cmb_hwdec), "auto-safe", "auto-safe (自动, 失败回退软解)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(cmb_hwdec), "no", "no (纯软件解码, 最费 CPU)");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(cmb_hwdec), hwdec);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(cmb_hwdec)) < 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(cmb_hwdec), 0);
    gtk_grid_attach(GTK_GRID(grid), cmb_hwdec, 1, 0, 1, 1);

    lbl = gtk_label_new("视频输出");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 1, 1, 1);
    cmb_vo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(cmb_vo), "gpu", "gpu (推荐)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(cmb_vo), "gpu-next", "gpu-next");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(cmb_vo), "x11", "x11 (画面异常时试)");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(cmb_vo), vo);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(cmb_vo)) < 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(cmb_vo), 0);
    gtk_grid_attach(GTK_GRID(grid), cmb_vo, 1, 1, 1, 1);

    chk_loop = gtk_check_button_new_with_label("循环播放");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_loop), loop);
    gtk_grid_attach(GTK_GRID(grid), chk_loop, 0, 2, 2, 1);

    /* --- 底部按钮 --- */
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("应用并重新播放");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_apply), NULL);
    gtk_box_pack_end(GTK_BOX(box), btn, FALSE, FALSE, 0);

    refresh_video_label();
    refresh_status();
    g_timeout_add(1000, on_tick, NULL);

    gtk_widget_show_all(win);
    g_idle_add(on_idle_focus, NULL);
    g_idle_add(on_idle_check_bin, NULL);
    gtk_main();
    return 0;
}
