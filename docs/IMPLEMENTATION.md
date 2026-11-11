# 实现说明与接口调查

## 布局选择

官方 Notepad++ 插件接口提供停靠窗口、创建 Scintilla 控件及获取两个编辑区句柄，但没有给每个编辑区顶部插入内容的布局槽。Scintilla 的左右 margin、annotation 与正文行关联，不是独立固定顶部区域。覆盖窗口会挡住正文；改变编辑区 HWND 的父窗口会干扰宿主通知路由、双视图和布局管理。

使用 Windows 公共 `SetWindowSubclass` API，截获编辑器的 `WM_WINDOWPOSCHANGING`，记录宿主分配的完整矩形，再把编辑器向下移动、等量缩小高度。在相同父窗口下创建一个无滚动条的固定区子窗口填充上方空间，保留原编辑器 HWND、父窗口和通知路由。宿主每次重新布局都会更新完整矩形，不累计偏移；插件自己的布局通过保护标志避免再次调整。

固定区使用 `WM_PAINT` 双缓冲绘制，`WM_LBUTTONDOWN` 导航；`WM_NCHITTEST` 显式返回 `HTCLIENT`，避免 STATIC 控件默认将真实指针命中透传给父窗口。不会拦截正文绘制或修改 `WM_NCCALCSIZE`。0.4.0 在右侧另留一个系统滚动条宽度，放置独立 `SCROLLBAR` 控件及其私有消息父窗口；这三个插件窗口均不改变原编辑器的父窗口。这是 Win32 布局适配，不是 Notepad++ 官方专用扩展槽。

所有子类化、布局、绘制、命中及恢复在 `src/StickyView.*` 内。高度或滚动条宽度变化时才调整编辑器；插件窗口矩形未变化时不重复布局。禁用时恢复 `SCI_GETVSCROLLBAR` 记录的原设置和宿主完整矩形，再销毁插件窗口和移除子类；宿主后续的 `SCI_SETVSCROLLBAR` 请求更新该设置并控制独立轨道的显示。`WM_NCDESTROY` 清除插件窗口和 GDI 资源；`NPPN_SHUTDOWN` 销毁工作消息窗口。`DllMain` 不执行窗口操作。

## 固定轨道与实时高度

编辑器自带垂直滚动条属于其非客户区，移动／缩小编辑器必然改变轨道；因此不能同时用该轨道、实时改变固定区高度并保证拖动映射稳定。0.4.0 使用公开 `SCI_SETVSCROLLBAR(false)` 隐藏它，在完整窗格的右侧放置独立 Win32 `SCROLLBAR`，固定区和正文只改变左侧空间。右侧轨道与固定区并排，不属于固定区内容。

范围从 `SCI_VISIBLEFROMDOCLINE(SCI_GETLINECOUNT)` 获取实际显示行总数，页大小来自 `SCI_LINESONSCREEN`，并按 `SCI_GETENDATLASTLINE` 复现宿主末尾约束，保持折叠／自动换行的显示行单位。位置来自 `SCI_GETFIRSTVISIBLELINE`。仅在值发生变化时调用 `SetScrollInfo`。箭头、翻页、首尾指令交给编辑器原有 `WM_VSCROLL` 路径；拖动用 `GetScrollInfo(SB_CTL, SIF_TRACKPOS)` 的完整 32 位位置，不能用只有 16 位的 `HIWORD(WPARAM)`，再调用 `SCI_SETFIRSTVISIBLELINE`。

`SB_THUMBTRACK` 至 `SB_ENDSCROLL` 期间不重新设置控件的范围／页面大小或滑块位置，保证 Windows 正在跟踪的像素映射不变；固定区内容和高度继续刷新。释放、取消或捕获丢失后恢复同步。编辑器缩放至文件末尾时仍允许宿主正常钳制首行，不强改光标或文本。

控件主题使用公开 `NPPM_DARKMODESUBCLASSANDTHEME` 消息；直接应用主题的 `0x10` 标志已在 v8.9.1 的 `NppDarkMode::autoSubclassAndThemePlugin` 源码中核对，该标志没有在 SDK 头文件中命名。主题绘制和物理鼠标拖动仍需跨机器人工验收，不宣称其他宿主版本已兼容。

## 层数设置

`设置...` 使用 Win32 原生模态对话框，最大行数为 1–20，默认 5；保留策略默认最近层级，也可选择最外层级。确定后应用两个视图并写入 INI，取消不修改。`FoldIndex::ancestors` 在最近模式收集足够数量后停止；最外模式遍历祖先链，用有界环形缓冲保留最外条目。两种模式最终均按外到内显示，不需要扫描全文。窗口空间不足时使用同一保留策略，拖动时不再限制为开始拖动时的行数。

## 行号与祖先

1. `SCI_GETFIRSTVISIBLELINE` 返回显示行，不是文档行。
2. 通过 `SCI_DOCLINEFROMVISIBLE` 获取正文首个文档行。光标位置不参与计算。
3. 祖先候选必须 `SCI_GETLINEVISIBLE` 为真，且 `SCI_VISIBLEFROMDOCLINE(parent) + SCI_WRAPCOUNT(parent) <= firstDisplay`。仍有换行片段可见的父行不重复显示；折叠隐藏的行不当作已滚出的起始行。
4. 点击前重新验证当前文档和祖先，使用 `SCI_ENSUREVISIBLE`、`SCI_GOTOLINE`，再把源文档行通过 `SCI_VISIBLEFROMDOCLINE` 转换后传给 `SCI_SETFIRSTVISIBLELINE`。

检查了 v8.9.1 的 `scintilla/src/PerLine.cxx::LineLevels::GetFoldParent`：它从当前文档行向前线性寻找折叠起始行。对十万行平坦数组，在每次滚动调用该函数仍可能扫描很长区间。因此使用相同公开折叠信息 `SCI_GETFOLDLEVEL`，维护每行的父级指针和原始 level，以持久链方式保存当前祖先栈。每行约 16 字节，15 万行约 2.4 MB（vector 容量可能更大），两个视图分别维护，不存全文。

首次构建和失效后从有效前缀继续，最多索引到当前首行（XML 再多一行）。每次任务最多 16,384 行，每 64 行检查 2ms 预算；未完成才安排一次性 16ms 工作续接定时器。空闲时无定时器，非高频轮询。查询折叠 level 使用 `SCI_GETDIRECTFUNCTION`／`SCI_GETDIRECTPOINTER` 返回的官方函数，仅在宿主 UI 线程调用。正常滚动无需扫描全文；读取每条固定行最多 4,096 字节，条数受用户配置限制（XML 过滤还读取候选起始行）。

JSON lexer 把行前层级放在低位、行后层级放在高 16 位；XML lexer 把行后层级放到下一行。比较较浅层级排除本行刚结束的父级。此规则依据随宿主打包的 `lexilla/lexers/LexJSON.cxx`、`LexHTML.cxx` 实现；不能泛化到其他 lexer。

## 事件和失效

- `SCN_UPDATEUI` 的内容／垂直／水平滚动标志、`SCN_PAINTED`、`SCN_ZOOM` 触发合并更新。
- `SCN_MODIFIED` 的文本插入、删除从受影响行前一行截断索引，立即清除旧点击目标；克隆同一文档的两个视图均失效。
- `SC_MOD_CHANGEFOLD` 只从通知的实际行截断已有索引，不涉及尚未索引的行。记录当前已显示祖先所依赖的最后一个折叠行：JSON 是正文首行，XML 再向后取一行。变更位于该范围内才立即清空点击目标；位于范围后面只截断缓存后缀，保留仍有效的固定行。这样宿主空闲着色后续行时，不会反复把顶部绘制成空白；编辑和切换文档仍完整失效，不靠延迟隐藏或保留过期行号掩盖问题。
- 使用官方 `NPPM_ADDSCNMODIFIEDFLAGS` 订阅额外折叠和样式通知，不覆盖宿主的通知掩码。`SC_MOD_CHANGESTYLE` 与固定行相交时，仅刷新固定行文本／样式缓存，不重建祖先索引。
- 文档指针／lexer 改变、`NPPN_BUFFERACTIVATED`、语言切换、关闭、全局修改、样式和深色模式通知使缓存失效。
- 窗口尺寸、可见性、DPI、主题、折叠、换行及滚动消息由集中子类补充触发。
- 更新使用事件触发的 16 ms 一次性 `WM_TIMER`，密集滚动只保留一个待执行任务，执行前清除定时器，空闲时无任务。输入和绘制优先于定时器。索引仍有剩余工作时才续接。
- 所有插件内部 Scintilla 调用均在 UI 线程通过公开 direct function 执行，避免多次经过 Windows／宿主子类链。字体、边距、DPI 和行高仅在相关消息、文档或主题变化时刷新；普通滚动只读取位置、查询索引及必要的固定行文本。
- `SCI_GETENDSTYLED` 尚未覆盖所需行时不采用旧层级，不强制全文 `SCI_COLOURISE`。生产代码不调用修改文本、lexer 属性或折叠状态的接口。

测试构建中的 `SCI_SETTEXT`、`SCI_COLOURISE` 等仅用于构造输入、驱动宿主测试；没有编入正式交付 DLL。

## 字体和绘制

文本使用 `STYLE_DEFAULT` 的背景、字体名、粗细、斜体、字符集及分数字号，结合 `SCI_GETZOOM` 与 `GetDpiForWindow` 创建 GDI 字体。行高来自 `SCI_TEXTHEIGHT`；左边距来自宿主 margins，水平偏移来自 `SCI_GETXOFFSET`，制表位来自宿主 tab width。行号单独使用 `STYLE_LINENUMBER` 配色和字体，跟随宿主 `SC_MARGIN_NUMBER` 栏宽，绘制文档行号 + 1，固定在左侧不参与水平滚动。隐藏宿主行号栏时也隐藏固定区行号。没有 hover 抢焦点或自动导航。

0.3.0 使用 SDK 的 `SCI_GETSTYLEDTEXTFULL` 获取每条固定行的“字节／样式索引”交错数据，按连续相同样式分段，转换 UTF-8 或当前代码页后绘制。每段前景色来自 `SCI_STYLEGETFORE`，不引入解析器，不强制重新着色。只缓存当前固定行及用到的颜色；行集合、文本或样式改变时失效。主题通知和 `SCI_STYLESETFORE` 等样式消息触发更新。读取仍限制为每行 4,096 字节，截断前用 `SCI_POSITIONBEFORE` 保持多字节字符完整；制表位基于整行文本原点，不因颜色分段重新起算。

顶部边框厚度来自编辑器窗口顶部到客户区顶部的实际距离，并计入固定区高度和点击坐标。`WS_EX_CLIENTEDGE` 使用 `DrawEdge(..., EDGE_SUNKEN, BF_TOP)` 复现浅色原生凹边；平面边框使用与当前配色协调的分隔色；宿主关闭边框时厚度为零。不向编辑器增加边框或滚动条样式。

## 版本信息

版本以 CMake `project(... VERSION ...)` 为唯一来源，配置时生成 `Version.h`，同时供 `Version.rc` 的标准 Windows `VERSIONINFO` 和“关于...”菜单使用。Notepad++ 的 `Version::setVersionFrom` 读取 `VS_FIXEDFILEINFO`，调试信息列表通过 `getLoadedPluginNames` 显示插件版本；这一路径已在真实宿主中验证。Notepad++ 会省略尾部的零，因此资源中的 `0.4.1.0` 在其调试信息中显示为 `0.4.1`。

插件管理名称是另一条路径：`pluginsAdmin.cpp` 的 `PluginUpdateInfo(fullFilePath, filename)` 直接设置 `_displayName = filename`。已安装插件若能匹配官方插件列表才继承列表内的 `display-name`，否则显示 DLL 文件名，不查询 `getName()` 或文件描述。插件尚未被官方列表收录，本版不修改宿主列表或私有内存来覆盖名称。

## 核对来源

- [官方插件模板](https://github.com/npp-plugins/plugintemplate/tree/abd8c2394936e934753db08ab1684aae1dcf7fd4)：入口 ABI 和 `PluginInterface.h`。
- [Notepad++ v8.9.1 源码](https://github.com/notepad-plus-plus/notepad-plus-plus/tree/v8.9.1)：消息常量、内置 Scintilla、Lexilla 与实际窗口处理。
- [插件通信文档](https://npp-user-manual.org/docs/plugin-communication/)：消息语义；仓库中实际 SDK 注释也记录了参数与返回值。
- [Scintilla 文档](https://www.scintilla.org/ScintillaDoc.html)：行号转换、折叠、直接调用、样式和通知；同时对照了下载源码的 `scintilla/doc/ScintillaDoc.html`。
- [WM_WINDOWPOSCHANGING](https://learn.microsoft.com/windows/win32/winmsg/wm-windowposchanging)、[WM_VSCROLL](https://learn.microsoft.com/windows/win32/controls/wm-vscroll)、[SetWindowSubclass](https://learn.microsoft.com/windows/win32/api/commctrl/nf-commctrl-setwindowsubclass)。
