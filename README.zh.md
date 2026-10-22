# Magpie Buddy

[English](README.md) · **简体中文**

一块放在桌上的小屏，显示 magpie 此刻在做什么：哪个 agent 发起、哪个账号应答、上下文窗口占了多少、每个订阅还剩多少。一个请求发出时，一只喜鹊会从 agent 飞向 magpie，再飞向应答的账号——账号失败时它会折返，把请求带给下一个。

硬件：**M5Stack CoreS3**（ESP32-S3，320×240，8 MB PSRAM，Wi-Fi）。

```
   ┌──────────────────────────────────────────┐
   │ claude                kimi/kimi-k2.5     │   顶部：谁发起，谁应答
   │                       group opus-anywhere│
   │                                          │
   │  ┌────┐      ┌────┐        ┌──────┐      │   舞台：agent → magpie → 账号
   │  │ ●  │──────│ 🐦 │───────▶│  ●   │      │
   │  │claude      magpie       kimi         │
   │  └────┘      └────┘        └──────┘      │
   │                                          │
   │   ◜◝     今日用量       Claude 5h   62%  │   上下文弧、今日用量、订阅余量
   │   24%    37 次调用     Codex 7d    18%   │
   │   48k    1.2M token    Kimi 月     84%   │
   └──────────────────────────────────────────┘
```

## 仓库里有什么

| 路径 | 是什么 |
| --- | --- |
| `docs/protocol.md` | 线协议：发现、帧、时序，以及一个帧**刻意不说什么** |
| `preview/flight.html` | 屏幕与飞行动画，320×240，在浏览器里跑。**设计基准**：固件画的就是它画的 |
| `firmware/` | 板子的固件（PlatformIO、Arduino、M5Unified） |
| `firmware/include/bird.h` | 喜鹊的蒙版，由 `tools/gen-bird.py` 从 magpie 自己的 SVG 烘焙而来 |
| `tools/gen-bird.py` | 把 magpie 的鸟——连同窗口自己的扇翅节奏——光栅化进那个头文件 |
| `firmware/include/ui_font.h` | **Inter，每像素 4 bit 覆盖率**，由 `tools/gen-font.py` 烘焙 |
| `tools/gen-font.py` | 把 Inter 光栅化进那个头文件；授权文件放在旁边一起分发 |
| `tools/render/` | **不用板子就画出板子的屏。** 把 `firmware/src/screen.cpp` 对着一个输出 SVG 的 shim 编译，所以版式在烧录前就能看到 |
| `bridge/` | **magpie 那一侧。** 一个 Node 文件，把任意 magpie 变成这块板子的数据源；细节看它自己的 [README](bridge/README.md) |

## 跑起来

### 1. 桥接（magpie 侧）

```sh
node bridge/magpie-display.mjs --discover
```

这就是全部。不用安装、不用编译、没有依赖：它只用了三个 Node 内置模块，读的是 magpie 自己本地 API 已经提供的接口。

**它是桥接而不是插件，这是有意的。** magpie 的插件做不了这件事：

| 类型 | 怎么装 | 为什么不行 |
| --- | --- | --- |
| 内置插件（magpie 源码里的 `plugins/display`） | 无需安装——它被编进主程序 | 加一个就要改 `plugins/all/all.go` 并重新编译 magpie。这不是一个板子的配套包能分发的做法，而且等于把 magpie 拉成分支。 |
| JS 网关插件 | `magpie plugin add ./x.mjs` | 跑在网关**内部**的 moejs 上，那里没有网络访问：没有 `fetch`，没有 socket。它能改写请求，但够不到板子。 |

所以这个包不碰 magpie 任何源码。它只读网关本来就应答的三个 GET，然后推送固件期待的 v1 帧。**固件并不知道是哪一侧在推它。**

想要 token 就设一个，两端给同一个字符串。

### 2. 固件（板子侧）

```sh
cd firmware
pio run -t upload        # 走 USB-C
pio device monitor       # 115200，串口日志
```

然后用手机配网——不用重新编译，源码里不存密码：

1. 板子会升起一个叫 **`Magpie-Buddy-XXXX`** 的热点（后两位来自芯片 id），并把这个名字显示在自己的屏幕上。
2. 连上它。配网页会自己弹出；没弹出就打开 **`http://192.168.4.1`**。
3. 选你的网络、填密码、连接。板子把它存进 NVS，然后重启接入你的网络。

网络存在 NVS 里，断电不会丢；把板子搬到别处只需要重新配一次网——**按住屏幕三秒**即可进入配网页，已保存的网络不会被删掉，看完点一下屏幕就回去。

`config.h` 不再需要了。用 `config.h` 编译的板子仍然会连它写明的网络，所以旧版本不受影响；不放这个文件，板子就走空中配网。如果你不想用手机，串口上的 `join <ssid> <password>` 做同样的事，`forget` 则清掉它。

### 3. 看它跑

```sh
```

板子每 30 秒广播一次 `MAGPIE-DISPLAY?`，直到有人开始推它，所以它的地址永远不用手输进任何东西。桥接会应答这个广播（`--discover`）。板子自己的 `/health` 会回答它最后画了什么：

```sh
curl -s http://magpie-desk.local:9877/health
```

## 预览

有两样东西都在画这块屏，但它们回答的是不同的问题：

| | `preview/flight.html` | `tools/render/` |
| --- | --- | --- |
| 画的是什么 | 一份独立的设计绘制 | **固件自己的 `screen.cpp`**，编译后运行 |
| 回答 | "它应该长什么样" | "板子实际会画出什么" |
| 依赖 | 浏览器 | C++ 编译器 |

预览是设计。渲染器是校验：它把板子用来绘制的那个文件，对着一个把 M5GFX 调用翻译成 SVG 的 shim 编译，于是版式改动可以在没有板子插在 USB 上的情况下被看到——也可以被 diff。

```sh
make -C tools/render                  # 所有状态，输出到 tools/render/out/
make -C tools/render one STATE=route  # 只出一个
```

状态有 `offline`、`route`、`context`、`usage`、`limits`、`failover`、`tight`、`flying`、`failed`，以及板子自己的 `setup` 和 `setup-failed`。

**文字是板子自己的。** M5GFX 的字体是 1 bit 的——一个字形就是一张开/关像素的蒙版，所以每条曲线都是台阶，这也是为什么这类板子上的文字无论用什么字体都显得粗糙。于是固件从 `ui_font.h` 自己画字：Inter 按屏幕用到的字号光栅化，每像素 4 bit 覆盖率，与底下的内容混合。`tools/gen-font.py` 烘焙它，`ui_text.h` 画它，而固件和这个渲染器编译的是同样两个文件——所以预览展示的是板子的字形，不是浏览器对它的近似。

```sh
python3 tools/gen-font.py            # 重新烘焙字库
python3 tools/gen-font.py --check    # 过期则退出码 1
```

Inter 采用 SIL Open Font License；授权文件与头文件放在一起，名为 `Inter-LICENSE.txt`。

| URL | 作用 |
| --- | --- |
| （无） | 控制面板：播放演示、发送一次成功、一次故障转移，或粘贴真实帧 |
| `?auto=1` | 载入即播放演示 |
| `?frame=1` | 只画一帧样例，鸟停在飞行途中，然后停下——每次都一样，用于截图或 diff |

把真实帧粘进输入框，就能看到板子会画出的确切画面。帧是从 magpie 发往板子的，所以拿到帧有两条路：

- **填入样例帧**：桥接发送的那种形状，用来玩绘制。
- 真实的一帧，用 `node bridge/magpie-display.mjs --dry-run --once`，它会打印出将要推送的确切内容。板子本身也会回答它最后画了什么，内容和它收到的是同一份 JSON：

```sh
curl -s http://magpie-desk.local:9877/health
```

## 飞行动画是怎么走的

它是 magpie 的 Routing 页面，一步一步，用它自己的时序（`internal/gui/assets/plugins/routing/client.js` 里的 `play()`）：

| 阶段 | 时长 |
| --- | --- |
| 请求离开 agent，穿过 magpie，到达账号 | 1400 ms（magpie 已经在承载时 900 ms） |
| 鸟松口；圆点在账号处等待应答 | 900 ms |
| 已应答：圆点沿原路返回 | 1400 ms |
| 失败，且路由还会再试：回到 magpie | 620 ms |
| 失败，且这就是最终结果：回家，红色 | 1350 ms |

鸟用的是 magpie 自己的形状，翅膀按窗口自己的 `@keyframes flap` 扇动：`tools/gen-bird.py` 用无头 Chrome 渲染它的四帧，烘焙成 4 bit alpha 蒙版，所以板子不是这份绘制的第二种解读，不会和它漂移。运行时蒙版用它为之飞行的 agent 的颜色混合。

## 一个帧不说什么

一块屏摆在别人会走过的房间里，所以帧只说 provider、model 和 agent，别的都不说：

- 没有账号、邮箱、密钥名或密钥 id；
- 没有会话、请求 id 或目录；
- 没有提示词文本、回复文本或错误文本。

桥接读的 trace 比帧携带的更多——`tries[].id` 里的账号、`affinity` 里应答的账号、每张配额卡上的一个账号、会话与目录——而桥接把这些全部丢弃。[桥接的 README](bridge/README.md#privacy) 有丢弃内容的表格。

token 也不通过发现发送：探测是广播的，所以一个点名 token 的应答等于把它交给任何发问的人。它在两端手动设置。

## 验证

```sh
# 桥接，对着运行中的 magpie：打印一帧
node bridge/magpie-display.mjs --dry-run --once

# 烘焙的鸟是最新的
python3 tools/gen-bird.py --check

# 固件能为板子编译
cd firmware && pio run
```

预览里的 JavaScript 按和固件输出同样的方式检查：

```sh
python3 - <<'PY'
import re; src = open("preview/flight.html").read()
open("/tmp/flight.js","w").write(re.findall(r"<script>(.*?)</script>", src, re.S)[-1])
PY
node --check /tmp/flight.js
```
