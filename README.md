# vidwall — 视频桌面壁纸

把一段视频当成桌面壁纸播放：**只覆盖主显示器、静音、鼠标点击穿透、永不抢焦点、一直循环**。
Windows 和图标都正常工作，视频在它们下面。

只支持 **X11** 会话（当前系统就是 X11，没问题）。Wayland 下这个方案不成立。

---

---

## 图形界面

已经装好，**在应用菜单里搜「视频壁纸」**就能打开（也放在了 `~/.local/share/applications/vidgui.desktop`）。

也可以直接运行：

```bash
cd /home/jun/文档/ChatGPT/Real_item/video-wallpaper
./vidgui
```

界面里能做的事：

| 控件 | 作用 |
|---|---|
| **选择视频…** | 弹文件对话框挑视频（等价于 `setwall`） |
| **启动 / 停止** | 开关壁纸，状态实时刷新 |
| **开机自动启动** | 勾选/取消开机自启 |
| **硬件解码** | `nvdec` / `auto-safe` / `no` |
| **视频输出** | `gpu` / `gpu-next` / `x11`（画面异常时换） |
| **循环播放** | 关掉就播一遍停住 |
| **应用并重新播放** | 保存设置并立刻重启壁纸 |

GUI 只是个外壳：它只做三件事——读写 `~/.config/vidwall/config`、起停 `vidwall` 进程、开关 `~/.config/autostart/vidwall.desktop`。**不碰 X11、不碰窗口层叠**，那些都是 `vidwall` 自己管的。

## 怎么用

### 换壁纸

在终端里：

```bash
cd /home/jun/文档/ChatGPT/Real_item/video-wallpaper
./setwall
```

不带参数会**弹出文件选择框**，挑一个视频就行。也可以直接指定：

```bash
./setwall ~/视频/我的壁纸.mp4
```

换完立刻生效，不用重启，也不用重新编译。

### 开机自动播放

已经装好了，位置在 `~/.config/autostart/vidwall.desktop`。
以后每次登录，系统会在 3 秒后自动启动壁纸，视频就是配置文件里那个。

### 配置文件

`~/.config/vidwall/config`：

```ini
video=/home/jun/下载/qq/preview.mp4   # 换成你想用的视频
hwdec=nvdec                          # 硬件解码方式
vo=gpu                               # 视频输出后端
loop=yes                             # 循环播放
```

手动改这一行也等价于用 `setwall`。命令行参数优先级高于配置文件。

### 临时手动运行

```bash
./vidwall /path/to/video.mp4          # 指定视频跑
./vidwall                             # 读配置跑(自启用的就是这个)
./vidwall --help                      # 看全部选项
```

按 `Ctrl-C` 退出。想关掉正在跑的壁纸：`pkill -x vidwall`。

---

## 依赖

| 依赖 | 用途 | 状态 |
|---|---|---|
| gcc + libX11/libXext/libXrandr 开发包 | 编译本程序 | 系统自带 |
| **mpv** | 解码和渲染视频 | 已安装 |
| zenity | `setwall` 的文件选择框 | 已安装（没有也能用命令行） |

不需要 libmpv-dev、不需要 Qt、不需要 Python、不需要 ffmpeg（除非要自己转码片源）。

## 编译

```bash
make
```

---

## 选项

| 选项 | 说明 |
|---|---|
| `--hwdec=NAME` | 硬件解码，默认 `nvdec`。可换 `auto-safe`，或 `no` 强制软解 |
| `--vo=NAME` | 视频输出，默认 `gpu`。画不出来时试 `x11` |
| `--no-loop` | 不循环 |
| `--below` | 额外设 `_NET_WM_STATE_BELOW`（**会跑到桌面图标之上，一般别用**） |
| `--override-redirect` | 用非托管窗口（实验用，会盖住一切） |
| `--keep-window` | mpv 退出后保留窗口，调试用 |
| `--verbose` | 显示 mpv 日志 |

---

## 原理：怎么才能压在桌面图标之下

这是整个项目最麻烦的地方，记录踩过的坑。

GNOME 的窗口分层：

```
DESKTOP 层   ← 桌面图标(ding)在这里
BOTTOM 层
NORMAL 层    ← 普通应用窗口
DOCK 层      ← GNOME 顶栏
顶部层        ← override-redirect 窗口
```

作为第三方窗口，必须待在 **DESKTOP 层**，否则会盖住普通窗口。但光有正确类型还不够——新映射的窗口会排在**图标之上**，把桌面图标盖住。

试过**全部无效**的手段：

- `XLowerWindow()` —— 降到整栈最底，越过普通窗口，违反分层模型被驳回
- `XConfigureWindow(自己, Below, sibling=图标窗口)`
- `XConfigureWindow(图标窗口, Above, sibling=自己)`
- `_NET_RESTACK_WINDOW` —— EWMH 官方的重排请求，Mutter 直接忽略
- `_NET_WM_STATE_BELOW` —— 把窗口放进 BOTTOM 层，反而在图标**之上**
- 合成鼠标移动（`XWarpPointer` / XTEST motion）—— 无效

**Mutter 对托管窗口的层叠请求一概不理。唯一有效的是走"激活"这条路：**

1. 记下当前活动窗口
2. 发 `_NET_ACTIVE_WINDOW` 激活桌面图标窗口 —— Mutter 在激活时会把窗口抬到所在层顶端，图标随即排到我们上面
3. 隔 200ms 把焦点还给原来那个窗口

实测 0.5 秒内生效、长期稳定，且**不抢走键盘焦点**。程序启动时密集收敛一次，之后每秒复查，被新窗口挤掉会自动重来。

另外窗口先以全透明 map，等 mpv 出画后再恢复不透明度，遮掉"黑窗口"那几百毫秒。

> 顺带记一笔：这个坑是靠用户的观察点破的——"鼠标一操作图标就出来了"。真实输入触发 Mutter 的点击抬升，而程序化的层叠请求完全无效。

---

## GPU 占用（RTX 5070 实测）


| 指标 | 未开壁纸 | 播放 4K60 |
|---|---|---|
| **功耗** | ~21 W | **~38 W** |
| dec（解码引擎） | 0% | **6 ~ 9%** |
| sm（合成） | 1 ~ 3% | **6 ~ 8%** |
| mem（显存带宽） | 2% | 3% |
| 显存占用 | ~700 MB | ~1450 MB |


## 文件

```
video-wallpaper/
├── src/vidwall.c    壁纸本体: X11 窗口 + 层叠 + 起停 mpv
├── src/vidgui.c     GTK3 图形界面
├── Makefile
├── setwall          命令行换壁纸 (带文件选择框), GUI 的轻量替代
├── vidwall          编译产物 (壁纸)
├── vidgui           编译产物 (界面)
└── README.md
```
