# NppStickyScroll

Windows x64 Notepad++ 插件，仅提供 JSON/XML 的 Sticky Scroll。滚动时，把已经离开正文视野的祖先折叠起始行固定在所属编辑区顶部；按外到内显示，最多保留最近 5 层。点击固定行跳转至源行。

## 安装与使用

建议 Notepad++ **8.9.1 x64 或更新版本**、Windows 10 1607 或更新版本。本次实际验证为 Windows 10、Notepad++ 8.9.1 x64；其他版本未验证。

1. 完全退出目标 Notepad++。
2. 在该实例的 `plugins` 下创建 `NppStickyScroll` 文件夹，把 `dist/NppStickyScroll.dll` 放入其中：`plugins/NppStickyScroll/NppStickyScroll.dll`。
3. 重启，打开格式化后的 JSON/XML，确认使用内置 JSON/XML 语言模式，然后向下滚动。
4. 菜单 **插件 → NppStickyScroll → 启用 Sticky Scroll** 控制开关，默认开启。菜单勾选表示开启。开关保存在宿主插件配置目录的 `NppStickyScroll.ini`。
5. 没有可显示的祖先时自动隐藏；固定区域使用编辑器默认字体、字号、颜色，跟随缩放、水平滚动和每个视图的 DPI。单击固定行才会导航并激活对应编辑区。

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

- 使用 Win32 非客户区预留空间，不覆盖正文，不使用浮动窗口、通用停靠面板或修改文档的注释行。顶部区域出现时可用正文高度会改变；滚动条从正文顶部开始。没有 VS Code 的逐行推挤动画。
- 仅支持内置 JSON/XML lexer。仅依赖现有折叠信息；折叠关闭、尚未完成着色或没有可靠层级时隐藏，不自行解析和猜测。
- 首次跳到未索引的远处、编辑较早位置后，会短暂隐藏，等待分批重建。每批最多 16,384 个折叠层级，并每 64 行检查 2 毫秒时间预算。空闲时不轮询。
- 按文档行展示，结束行采用入站／出站较浅的层级，因此关闭括号或 XML 结束标签所在行不再显示刚结束的层级。同一行交错关闭和新建多个层级不提供字符级语义。
- XML 跨多行的起始标签若被宿主折叠器标记在属性末行，本版跳过该层，不猜测真正的起始行；注释、CDATA、处理指令不作为 XML 元素父级显示。XML 文档最后一行没有下一行折叠信息时保守隐藏。
- 超长固定行只读取前 4,096 字节，单行裁切／省略，不做逐字符语法着色。GDI 文本与宿主 DirectWrite 的字形及抗锯齿可能略有差别。
- 不提供面包屑、结构树、格式化、校验、x86/ARM64 或无障碍导航界面。其他也修改 Scintilla 非客户区的插件未验证兼容性。

项目代码采用 GPL-3.0-or-later；官方 SDK 保留各自许可，来源见 `vendor/README.md`。
