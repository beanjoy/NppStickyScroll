# 实现说明与接口调查

## 布局选择

官方 Notepad++ 插件接口提供停靠窗口、创建 Scintilla 控件及获取两个编辑区句柄，但没有给每个编辑区顶部插入内容的布局槽。Scintilla 的左右 margin、annotation 与正文行关联，不是独立固定顶部区域。覆盖窗口会挡住正文；改变编辑区 HWND 的父窗口会干扰宿主通知路由、双视图和布局管理；缩小窗口外部矩形还会与宿主重新布局竞争。

本实现保留宿主窗口的外部矩形和父窗口，使用 Windows 公共 `SetWindowSubclass` API，在 `WM_NCCALCSIZE` 默认处理后缩小客户区上边界。`WM_NCPAINT` 使用双缓冲 GDI 绘制固定区域；鼠标命中通过 `WM_NCHITTEST`／`WM_NCLBUTTONDOWN` 单独处理。正文客户区及滚动条从固定区域下方开始。它不是 Notepad++ 官方专用扩展槽，属于有明确兼容性边界的 Win32 适配。

所有子类化、绘制、命中及恢复在 `src/StickyView.*` 内。`SWP_FRAMECHANGED` 仅在高度变化时执行，设有布局递归保护，并合并后续更新。禁用时先将高度恢复为 0，再移除子类；`WM_NCDESTROY` 移除子类和 GDI 资源；`NPPN_SHUTDOWN` 销毁工作消息窗口。`DllMain` 不执行窗口操作。

## 行号与祖先

1. `SCI_GETFIRSTVISIBLELINE` 返回显示行，不是文档行。
2. 通过 `SCI_DOCLINEFROMVISIBLE` 获取正文首个文档行。光标位置不参与计算。
3. 祖先候选必须 `SCI_GETLINEVISIBLE` 为真，且 `SCI_VISIBLEFROMDOCLINE(parent) + SCI_WRAPCOUNT(parent) <= firstDisplay`。仍有换行片段可见的父行不重复显示；折叠隐藏的行不当作已滚出的起始行。
4. 点击前重新验证当前文档和祖先，使用 `SCI_ENSUREVISIBLE`、`SCI_GOTOLINE`，再把源文档行通过 `SCI_VISIBLEFROMDOCLINE` 转换后传给 `SCI_SETFIRSTVISIBLELINE`。

检查了 v8.9.1 的 `scintilla/src/PerLine.cxx::LineLevels::GetFoldParent`：它从当前文档行向前线性寻找折叠起始行。对十万行平坦数组，在每次滚动调用该函数仍可能扫描很长区间。因此使用相同公开折叠信息 `SCI_GETFOLDLEVEL`，维护每行的父级指针和原始 level，以持久链方式保存当前祖先栈。每行约 16 字节，15 万行约 2.4 MB（vector 容量可能更大），两个视图分别维护，不存全文。

首次构建和失效后从有效前缀继续，最多索引到当前首行（XML 再多一行）。每次任务最多 16,384 行，每 64 行检查 2ms 预算；未完成才安排一次性 16ms 工作续接定时器。空闲时无定时器，非高频轮询。查询折叠 level 使用 `SCI_GETDIRECTFUNCTION`／`SCI_GETDIRECTPOINTER` 返回的官方函数，仅在宿主 UI 线程调用。正常滚动无需扫描全文；读固定行最多 5 × 4,096 字节（XML 过滤还读取候选起始行）。

JSON lexer 把行前层级放在低位、行后层级放在高 16 位；XML lexer 把行后层级放到下一行。比较较浅层级排除本行刚结束的父级。此规则依据随宿主打包的 `lexilla/lexers/LexJSON.cxx`、`LexHTML.cxx` 实现；不能泛化到其他 lexer。

## 事件和失效

- `SCN_UPDATEUI` 的内容／垂直／水平滚动标志、`SCN_PAINTED`、`SCN_ZOOM` 触发合并更新。
- `SCN_MODIFIED` 的文本插入、删除及 `SC_MOD_CHANGEFOLD` 从受影响行前一行截断索引，立即清除旧点击目标；克隆同一文档的两个视图均失效。
- 使用官方 `NPPM_ADDSCNMODIFIEDFLAGS` 订阅额外折叠通知，不覆盖宿主的通知掩码。
- 文档指针／lexer 改变、`NPPN_BUFFERACTIVATED`、语言切换、关闭、全局修改、样式和深色模式通知使缓存失效。
- 窗口尺寸、可见性、DPI、主题、折叠、换行及滚动消息由集中子类补充触发。
- `SCI_GETENDSTYLED` 尚未覆盖所需行时不采用旧层级，不强制全文 `SCI_COLOURISE`。生产代码不调用修改文本、lexer 属性或折叠状态的接口。

测试构建中的 `SCI_SETTEXT`、`SCI_COLOURISE` 等仅用于构造输入、驱动宿主测试；没有编入正式交付 DLL。

## 字体和绘制

使用 `STYLE_DEFAULT` 的前景、背景、字体名、粗细、斜体、字符集及分数字号，结合 `SCI_GETZOOM` 与 `GetDpiForWindow` 创建 GDI 字体。行高来自 `SCI_TEXTHEIGHT`；左边距来自宿主 margins，水平偏移来自 `SCI_GETXOFFSET`，制表位来自宿主 tab width。没有语法着色、hover 抢焦点或自动导航。

## 核对来源

- [官方插件模板](https://github.com/npp-plugins/plugintemplate/tree/abd8c2394936e934753db08ab1684aae1dcf7fd4)：入口 ABI 和 `PluginInterface.h`。
- [Notepad++ v8.9.1 源码](https://github.com/notepad-plus-plus/notepad-plus-plus/tree/v8.9.1)：消息常量、内置 Scintilla、Lexilla 与实际窗口处理。
- [插件通信文档](https://npp-user-manual.org/docs/plugin-communication/)：消息语义；仓库中实际 SDK 注释也记录了参数与返回值。
- [Scintilla 文档](https://www.scintilla.org/ScintillaDoc.html)：行号转换、折叠、直接调用、样式和通知；同时对照了下载源码的 `scintilla/doc/ScintillaDoc.html`。
- [WM_NCCALCSIZE](https://learn.microsoft.com/windows/win32/winmsg/wm-nccalcsize)、[SetWindowSubclass](https://learn.microsoft.com/windows/win32/api/commctrl/nf-commctrl-setwindowsubclass)。
