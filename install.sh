#!/bin/bash
#
# 视频壁纸 —— 安装脚本
#
# 用法:
#   ./install.sh                    交互式安装(会弹窗让你选壁纸视频)
#   ./install.sh /path/to/video.mp4 直接指定视频
#
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/vidwall"
AUTO_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"

echo "==================================="
echo "        视频壁纸  安装"
echo "==================================="
echo

# ---- 1. 环境检查 ----
if [ "${XDG_SESSION_TYPE:-}" != "x11" ]; then
    echo "!! 警告: 当前不是 X11 会话 (XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-未知})"
    echo "   这个壁纸依赖 X11 才能贴在桌面层, Wayland 下无法工作。"
    echo
fi

if ! command -v mpv >/dev/null 2>&1; then
    echo "!! 缺少 mpv —— 这是必需的播放引擎, 请先安装:"
    echo
    echo "       sudo apt install mpv"
    echo
    exit 1
fi
echo "✓ 播放引擎 mpv"

for b in vidwall vidgui; do
    if [ ! -x "$DIR/$b" ]; then
        echo "!! 缺少可执行文件: $DIR/$b"
        exit 1
    fi
done
echo "✓ 程序文件完整"

if ! ls /usr/lib/x86_64-linux-gnu/libgtk-3.so.0 >/dev/null 2>&1; then
    echo "! 提示: 没找到 GTK3, 图形界面可能打不开 (壁纸本身不受影响)"
    echo "  装的话: sudo apt install libgtk-3-0"
fi
echo

# ---- 2. 选壁纸视频 ----
VIDEO=""
if [ $# -ge 1 ]; then
    VIDEO="$1"
elif command -v zenity >/dev/null 2>&1; then
    echo "请在弹出的窗口里选择壁纸视频..."
    VIDEO="$(zenity --file-selection --title='选择壁纸视频' \
        --file-filter='视频文件 | *.mp4 *.mkv *.webm *.mov *.avi *.m4v' \
        --file-filter='所有文件 | *' 2>/dev/null || true)"
else
    read -r -p "请输入壁纸视频的完整路径 (直接回车跳过): " VIDEO
fi

if [ -n "$VIDEO" ] && [ -f "$VIDEO" ]; then
    VIDEO="$(readlink -f "$VIDEO")"
else
    VIDEO=""
    echo "! 没选视频, 之后打开「视频壁纸」界面再选也行"
fi

# ---- 3. 写配置 ----
mkdir -p "$CONF_DIR"
{
    echo "# vidwall 配置 —— 也可以直接用图形界面改"
    [ -n "$VIDEO" ] && echo "video=$VIDEO"
    echo "hwdec=nvdec"
    echo "vo=gpu"
    echo "loop=yes"
} > "$CONF_DIR/config"
[ -n "$VIDEO" ] && echo "✓ 壁纸视频: $VIDEO"
echo "✓ 配置: $CONF_DIR/config"

# ---- 4. 开机自启 ----
mkdir -p "$AUTO_DIR"
cat > "$AUTO_DIR/vidwall.desktop" << EOF
[Desktop Entry]
Type=Application
Name=视频壁纸
Comment=用 mpv 播放视频作为桌面壁纸 (vidwall)
Exec="$DIR/vidwall"
X-GNOME-Autostart-enabled=true
X-GNOME-Autostart-Delay=3
Terminal=false
EOF
chmod 644 "$AUTO_DIR/vidwall.desktop"
echo "✓ 开机自启已开启 (想关掉就在图形界面里取消勾选)"

# ---- 5. 应用菜单入口 ----
mkdir -p "$APPS_DIR"
cat > "$APPS_DIR/vidgui.desktop" << EOF
[Desktop Entry]
Type=Application
Name=视频壁纸
Comment=更换视频壁纸、开关开机自动启动
Exec="$DIR/vidgui"
Icon=preferences-desktop-wallpaper
Terminal=false
Categories=Settings;DesktopSettings;GTK;
Keywords=wallpaper;video;壁纸;视频;
EOF
chmod 644 "$APPS_DIR/vidgui.desktop"
command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database "$APPS_DIR" 2>/dev/null || true
echo "✓ 应用菜单入口 (搜「视频壁纸」)"

echo
echo "==================================="
echo " 安装完成"
echo "==================================="
echo
echo "怎么用:"
echo "  在应用菜单搜「视频壁纸」打开界面, 点「启动」"
echo
echo "  想换壁纸: 界面里点「选择视频…」再点「应用并重新播放」"
echo
if [ -n "$VIDEO" ]; then
    echo "现在就开始播放? (y/N)"
    read -r ans
    if [ "${ans:-n}" = "y" ] || [ "${ans:-n}" = "Y" ]; then
        pkill -x vidwall 2>/dev/null || true
        sleep 0.5
        nohup "$DIR/vidwall" >/dev/null 2>&1 &
        disown 2>/dev/null || true
        echo "已启动 —— 按 Ctrl-C 退出这个终端不影响它"
    fi
fi
