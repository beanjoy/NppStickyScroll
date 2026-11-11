# 官方 SDK 来源

这些文件未经文本转换，保留原始字节、版权及许可声明。

- `npp/PluginInterface.h`：官方 `npp-plugins/plugintemplate` 提交 `abd8c2394936e934753db08ab1684aae1dcf7fd4` 的 `src/PluginInterface.h`，GPL-3.0-or-later。
- `npp/Notepad_plus_msgs.h`、`npp/menuCmdID.h`：Notepad++ `v8.9.1`，tag commit `f617e4e7c5d11f318cd78d5235c199ca7de74a76`，`PowerEditor/src/MISC/PluginsManager/Notepad_plus_msgs.h` 和 `PowerEditor/src/menuCmdID.h`。`menuCmdID.h` 仅供宿主测试使用。许可见 `npp/LICENSE` 与文件头。
- `scintilla/Scintilla.h`、`Sci_Position.h`：同一 Notepad++ tag 的 `scintilla/include`；许可见 `scintilla/License.txt`。
- `lexilla/SciLexer.h`：同一 tag 的 `lexilla/include`；许可见 `lexilla/License.txt`。

构建不下载依赖。`.tools` 内下载的完整源码和便携宿主是本地验证工具，不是插件运行依赖，也不包含在发布包中。
