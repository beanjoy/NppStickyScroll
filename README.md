# NppStickyScroll

Windows x64 Notepad++ 插件，仅提供 JSON/XML 的 Sticky Scroll。滚动时，把已经离开正文视野的祖先折叠起始行和源行号固定在所属编辑区顶部，沿用正文的语法前景色；按外到内显示，默认保留最近 5 层，可设置最大行数和保留策略。点击固定行或行号跳转至源行。

## 安装与使用

建议 Notepad++ **8.9.1 x64 或更新版本**、Windows 10 1607 或更新版本。本次实际验证为 Windows 10、Notepad++ 8.9.1 x64；其他版本未验证。

1. 完全退出目标 Notepad++。
2. 在该实例的 `plugins` 下创建 `NppStickyScroll` 文件夹，把 `dist/NppStickyScroll.dll` 放入其中：`plugins/NppStickyScroll/NppStickyScroll.dll`。
3. 重启，打开格式化后的 JSON/XML，确认使用内置 JSON/XML 语言模式，然后向下滚动。
4. 菜单 **插件 → NppStickyScroll → 启用 Sticky Scroll** 控制开关，默认开启。菜单勾选表示开启。
5. **插件 → NppStickyScroll → 设置...** 可选择最大父级行数（1–20，默认 5），以及超出限制时保留最近层级或最外层级。无论选择哪一种，显示顺序都是从外到内。设置立即作用于两个视图，取消不修改设置；开关及选项保存在宿主插件配置目录的 `NppStickyScroll.ini`。
6. 固定行的源行号沿用正文行号栏的宽度、字体和配色，水平滚动时行号不移动。宿主隐藏行号栏时固定区也隐藏行号。正文很矮时实际显示行数会缩减，给正文留出空间。
7. 没有可显示的祖先时自动隐藏；固定区域使用编辑器默认字体、字号、颜色，跟随缩放、水平滚动和每个视图的 DPI。单击固定行才会导航并激活对应编辑区。
8. **插件 → NppStickyScroll → 关于...** 显示完整版本号（当前为 **0.4.1**）；**? → 调试信息** 的插件列表也会读取 DLL 版本。Notepad++ 会省略末尾的零，本版显示 `NppStickyScroll (0.4.1)`；Windows 文件属性中的“详细信息”显示 `0.4.1`。

**插件管理里为什么显示 `NppStickyScroll.dll`？** Notepad++ 对未收录在官方插件列表中的手动安装插件，直接使用 DLL 文件名作为“已安装”列表的名称，不读取 `getName()`、`ProductName` 或 `FileDescription`。本插件尚未被官方列表收录，因此该处仍显示文件名；插件菜单与“关于”显示 `NppStickyScroll`。这是宿主行为，不是版本资源缺失，插件没有公开接口可覆盖这个名称。

JSON 字段名、括号、数字以及 XML 标签、属性和值的前景色直接来自当前编辑器的 lexer 样式，随主题配色更新。顶部与文件标签栏相邻的边框沿用编辑器实际边框厚度；浅色模式使用原生凹边，关闭宿主边框时固定区也不增加该边框。

0.4.0 将垂直滚动条改为编辑区右侧的独立 Win32 滚动条，轨道覆盖整个所属窗格的高度，固定行及正文位于它左侧。拖动期间固定内容和高度都可以实时更新；仅冻结滚动条的位置映射，释放后同步页面大小，避免固定区伸缩带动轨道跳动。连续事件合并到约 16 ms 的一次性更新中，空闲时不轮询。实际鼠标拖动的视觉流畅性尚待人工验收，已验证的消息与性能检查见验证记录。

0.4.1 修复 `Large.json` 停止滚动后固定内容反复闪烁：宿主在后台计算后续行的折叠信息时，不再清空当前仍然有效的父级。实际修改父级或其依赖行时仍立即失效。通过直接打开约 5 MB／15 万行文件的自然着色测试，静止与连续滚动均未产生空白帧或高度变化。

配置文件兼容旧版仅有 `Enabled` 的设置。也可在退出 Notepad++ 后手动修改：

```ini
[StickyScroll]
Enabled=1
MaxRows=5
KeepNearest=1
```

`KeepNearest=1` 保留最近层级，`0` 保留最外层级。

关闭功能会立即移除窗口子类并恢复完整编辑区域。卸载时先退出 Notepad++，删除 `plugins/NppStickyScroll` 文件夹；可选删除插件配置目录下的 `NppStickyScroll.ini`。不需要修改注册表。

## 构建

已包含官方 SDK 头文件，构建无需网络。要求 CMake 3.21+、Visual Studio 2022 的“使用 C++ 的桌面开发”和 Windows SDK。

```powershell
./build.ps1
```

等价命令：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

产物为 `build/Release/NppStickyScroll.dll`；脚本还会复制到 `dist/NppStickyScroll.dll`。使用静态 MSVC 运行库，无额外框架或插件依赖。不同 CMake/VS 版本可调整 `build.ps1 -Generator`。发布构建不要打开 `STICKY_HOST_TESTS`。

## 示例与验证

- `samples/DeviceLog.json`、`DeviceLog.xml`：普通多层结构和结束边界。
- `samples/Deep.json`：超过 5 层。
- `samples/DragLevels.json`：连续多段不同深度，便于拖动滚动条检查实时高度。
- `samples/Wrapped.json`：超长父行、自动换行和水平滚动。
- `samples/Empty.json`：空文件。
- `pwsh -File samples/Generate-Samples.ps1 -Large`：重新生成示例和约 15 万行 `Large.json`。

隔离宿主测试：从官方 GitHub 下载 `npp.8.9.1.portable.x64.zip`，然后运行：

```powershell
pwsh -File tests/Run-HostTests.ps1 -PortableZip C:/Downloads/npp.8.9.1.portable.x64.zip
```

脚本检查 ZIP 的固定 SHA-256，创建新的 `.tools/host-<随机值>` 便携目录，构建测试专用 DLL，再在该实例中执行真实 Notepad++/Scintilla 操作。测试版会创建及编辑临时文档，**不可安装到日常使用的 Notepad++**。正式 DLL 不包含这些测试入口。

具体实现、接口来源与布局限制见 [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md)。已经验证和未验证的项目、测量方法见 [docs/VALIDATION.md](docs/VALIDATION.md)。

## 已知差异与范围

- 使用同一父窗口下的固定区和滚动条子窗口，将宿主空间分为左侧的固定区／正文与右侧固定轨道。通过 `SCI_SETVSCROLLBAR` 暂时关闭编辑器自带的垂直滚动条；禁用插件时恢复原设置及全部空间。水平滚动条仍由编辑器管理。固定区不修改编辑器非客户区、不覆盖正文，没有 VS Code 的逐行推挤动画。
- 仅支持内置 JSON/XML lexer。仅依赖现有折叠信息；折叠关闭、尚未完成着色或没有可靠层级时隐藏，不自行解析和猜测。
- 首次跳到未索引的远处、编辑较早位置后，会短暂隐藏，等待分批重建。每批最多 16,384 个折叠层级，并每 64 行检查 2 毫秒时间预算。空闲时不轮询。
- 按文档行展示，结束行采用入站／出站较浅的层级，因此关闭括号或 XML 结束标签所在行不再显示刚结束的层级。同一行交错关闭和新建多个层级不提供字符级语义。
- XML 跨多行的起始标签若被宿主折叠器标记在属性末行，本版跳过该层，不猜测真正的起始行；注释、CDATA、处理指令不作为 XML 元素父级显示。XML 文档最后一行没有下一行折叠信息时保守隐藏。
- 超长固定行只读取前 4,096 字节，保持完整字符边界，单行裁切／省略。复用字符样式的前景色，字体和背景仍使用编辑器默认样式，不复制选择背景、搜索标记或每种 token 的独立字体。GDI 文本与宿主 DirectWrite 的字形及抗锯齿可能略有差别。
- 不提供面包屑、结构树、格式化、校验、x86/ARM64 或无障碍导航界面。其他也修改 Scintilla 窗口布局的插件未验证兼容性。

项目代码采用 GPL-3.0-or-later；官方 SDK 保留各自许可，来源见 `vendor/README.md`。
