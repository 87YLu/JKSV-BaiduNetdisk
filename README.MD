# JKSV 百度网盘存档同步

> v1.1.0 · 适用于运行 Atmosphère 的 Nintendo Switch · 版本变化见 [CHANGELOG.MD](CHANGELOG.MD)

基于 [J-D-K/JKSV](https://github.com/J-D-K/JKSV) 的存档管理工具，只保留本地备份和百度网盘两种存档方式，界面为简体中文。配置一次后，游戏关闭时会自动备份存档并上传到百度网盘，也可以在 JKSV 里手动上传、恢复和管理云端备份。

## 主要功能

- 百度网盘 App 扫码登录，登录状态自动续期，通常不需要反复扫码。
- 游戏关闭后自动备份并上传；存档没有变化时跳过。
- 断网或上传失败时保留任务，之后按退避间隔自动重试。
- 安装 Ultrahand 时，上传成功后显示 Toast 提示。
- 在 JKSV 中上传本地 ZIP、恢复云端备份、覆盖或删除云端文件，备份列表按时间倒序并标记 `[最新]`。
- 自动备份与 JKSV 格式兼容，可以直接恢复。

## 准备

- 可以运行 Atmosphère 和 Homebrew Menu 的 Switch，并能连接互联网。
- 百度网盘 App，用于扫码授权。
- 在[百度网盘开放平台](https://pan.baidu.com/union/)创建应用，取得 AppKey 和 SecretKey。本项目不提供公共密钥。

## 安装

把发布包解压到 SD 卡根目录，确认以下文件存在：

    /switch/JKSV/JKSV.nro
    /atmosphere/contents/420000004A4B5344/exefs.nsp
    /atmosphere/contents/420000004A4B5344/toolbox.json
    /atmosphere/contents/420000004A4B5344/flags/boot2.flag
    /atmosphere/contents/420000004A4B5345/exefs.nsp
    /config/JKSV/baidu.example.json

从 v1.0.4 或更早版本升级时，**420000004A4B5345** 是新增的目录，一定要一起复制。复制后请完整关机再开机，休眠或返回主界面不会重新加载后台模块。

## 配置

把示例复制为 **/config/JKSV/baidu.json** 并填写：

    {
        "app_key": "你的 AppKey",
        "secret_key": "你的 SecretKey",
        "basepath": "/apps/JKSV",
        "background_sync": true
    }

百度只允许应用访问自己的目录。建议把开放平台的应用名称设为 JKSV；如果不是，把 basepath 改成 `/apps/<应用名称>`。

## 登录

1. 保持联网，建议按住 **R** 启动任意游戏进入完整内存模式的 Homebrew Menu，再打开 JKSV。
2. 屏幕出现二维码后，用百度网盘 App 扫码并确认授权。
3. 看到“成功登录百度网盘”即完成。

账号相关的操作都在 JKSV 设置页：

| 项目 | 作用 |
| --- | --- |
| 百度网盘账号 | 显示当前账号；已退出时按 **[A]** 重新登录；显示“未连接”时按 **[A]** 重试连接 |
| 重新扫码登录百度网盘 | 显示新二维码，已登录或授权失效时都可使用。新登录授权成功前旧登录保持有效，取消不会有任何影响 |
| 退出百度网盘 | 清除本机令牌并暂停后台上传，云端备份、AppKey、设置和待传队列都会保留 |
| 后台自动备份 | 显示后台模块是否正常；因连续异常被停用时按 **[A]** 重新启用，重启后生效 |

登录信息保存在 baidu.json 中，到期前自动续期。百度的 refresh_token 有效期为 10 年但只能使用一次，JKSV 和后台模块会加锁、按顺序刷新，不会互相作废。只有授权被撤销或刷新失败时才需要重新扫码。

二维码过期后会自动换一张新的，连续 5 张都没扫码才会结束。重新扫码登录成功后，后台会把下一次关闭的游戏当作新存档重新上传，退出期间玩过的其他游戏不会一次性补传。

## 自动备份

在主界面按 **[X]** 彻底关闭游戏后，后台模块会等存档写完，再打包并上传。只按 Home 返回时游戏仍在运行，不会备份。

后台由两个程序组成：

| 程序 | 目录 | 运行方式 |
| --- | --- | --- |
| 监视程序 | 420000004A4B5344 | 开机常驻，约 60 KiB 内存。线程在系统里休眠，只有进程退出时才被唤醒；仅在 10.0.0 以下的系统上改为每 5 秒检查一次 |
| 备份程序 | 420000004A4B5345 | 由监视程序按需启动，完成打包、上传和通知后立即退出并释放内存 |

备份程序从系统游玩记录中查找刚关闭的游戏，游玩时间再短也不会漏掉。游戏运行时它不会打包或上传；打包或上传途中启动了游戏，会立即停下，等游戏再次关闭后继续。JKSV 打开期间后台也会暂停，退出 JKSV 后约 5 分钟内继续。

还没扫码登录时，后台不会打包任何存档。同一份备份连续 8 次上传失败，或超过百度 4 GiB 的单文件上限时，会移到 **/config/JKSV/background/failed/** 并弹出提示，不再反复重试。SD 卡剩余空间不足时跳过打包。

云端路径为：

    <basepath>/<游戏名>/AUTO - <用户 [账号标识]> - <时间>.zip

游戏名取主机语言下的名称，JKSV 手动备份和后台自动备份使用同一个文件夹。旧版本 JKSV 会把中文游戏名换成 16 位的游戏 ID 作为文件夹名；打开该游戏的备份列表时，旧文件夹会自动改名为游戏名，原有备份不受影响。如果两个文件夹都已存在，旧文件夹会保留，可在百度网盘中手动合并。在 JKSV 中修改游戏的本地文件夹名不会影响云端文件夹。

## 手动备份与恢复

- **新建云端备份**：选择用户和游戏后选“新备份”。设置中开启“自动上传备份到远程”时会直接上传，开启“保留本地备份”时 SD 卡也保留一份。云端文件夹暂时无法访问时，会改为创建本地备份并提示。
- **全部备份**：在主界面或用户菜单中备份全部游戏时，无法访问云端文件夹的游戏会被跳过，结束后提示跳过的数量。
- **上传本地备份**：在备份列表中选中本地 ZIP，按 **[ZR]**。只支持 ZIP，需要先开启“导出存档为 ZIP”。
- **恢复云端备份**：先完全关闭游戏，在备份列表中选中带 **[BD]** 前缀的备份，按 **[Y]** 并确认。恢复会覆盖当前存档，建议开启“恢复时自动备份”。

后台模块只负责备份和上传，不会自动把云端存档写回主机。

## 后台选项

以下选项都写在 baidu.json 中，修改后建议完整重启：

| 选项 | 默认值 | 用途 |
| --- | ---: | --- |
| background_sync | true | 开启或暂停自动备份 |
| background_include_device_saves | true | 同时备份设备共享存档 |
| background_keep_local | false | 上传成功后在 **/JKSV/Background/** 保留 ZIP |
| background_verify_tls | true | 校验 HTTPS 证书，同时作用于 JKSV 和后台，建议保持开启 |
| background_compression | 1 | ZIP 压缩等级，0–9 |
| background_close_delay_seconds | 3 | 关闭游戏后等待存档写入的秒数，1–30 |
| background_retry_minutes | 5 | 上传失败后的基础重试间隔，连续失败时逐步加长，1–180 |

## 文件位置

| 内容 | 路径 |
| --- | --- |
| 百度网盘配置与登录信息 | /config/JKSV/baidu.json |
| 后台日志 | /config/JKSV/background-sync.log |
| 待上传队列 | /config/JKSV/background/queue/ |
| 多次上传失败的备份 | /config/JKSV/background/failed/ |
| JKSV 日志 | /JKSV/log.txt |

## 暂停或卸载

- 暂停自动备份：把 background_sync 改为 false。
- 停止开机加载：删除或改名 **/atmosphere/contents/420000004A4B5344/flags/boot2.flag** 后重启。
- 卸载后台模块：删除 **420000004A4B5344** 和 **420000004A4B5345** 两个目录。只用手动备份时可以保留 JKSV.nro 和 baidu.json。

## 常见问题

**关闭游戏后没有上传**：确认游戏是按 [X] 关闭的、安装后完整重启过、两个 exefs.nsp 和 boot2.flag 位置正确、background_sync 为 true，并且设置页显示已登录。再查看后台日志：没有 `WORKER_START` 说明备份程序没被启动；`SAVE_UNCHANGED` 表示存档没有变化。

**没有出现二维码**：检查 baidu.json 的 AppKey 和 SecretKey 以及网络。设置页显示“已退出”时，在“百度网盘账号”上按 [A]。

**提示无法连接百度网盘**：JKSV 启动时没能连上百度，可能是网络未就绪。联网后在设置页的“百度网盘账号”上按 [A] 重试，不需要重新扫码。

**上传失败**：检查网络、网盘剩余空间，以及 basepath 是否与应用目录一致，然后查看日志中 `UPLOAD_FAILED` 后面的错误信息。必要时退出后重新扫码。

**上传成功但没有 Toast**：需要由 nx-ovlloader 加载 Ultrahand（不是 Tesla Menu），建议 2.3.0 或更新版本，并在 **/config/ultrahand/config.ini** 的 `[ultrahand]` 段设置 `notifications=true`。开启 `notifications_hotkey` 时，长按 [-] 约 3 秒会切换这个开关。日志中的 `NOTIFY_QUEUED`、`NOTIFY_SKIPPED` 可以帮助判断。

**生成了多份备份**：每个账号存档和设备存档分别备份，文件名中的用户和账号标识用于区分。

**后台模块被停用**：备份程序在主机开着的情况下连续两次异常退出时，会把 boot2.flag 改名为 boot2.flag.crashed，并弹出提示。关机或断电打断的备份不计入。检查日志并解决问题后，在设置页的“后台自动备份”上按 [A] 重新启用，然后重启。

**提示后台备份程序无法启动**：监视程序连续 3 次没能启动备份程序，通常是 **420000004A4B5345** 目录缺失或文件损坏。重新复制发布包后重启。

## 使用安全

- 恢复前先备份当前进度，不要在游戏运行时恢复。
- 不要公开 baidu.json，其中有应用密钥和长期登录令牌。
- 不要随意删除 background/queue 中的文件，未上传成功的备份保存在这里。
- 重要存档建议另外保留离线副本。

## 编译

需要 devkitPro（switch-dev 和 switch-portlibs）：

```bash
make
```

```bash
make background
```

后者生成 sysmodule/JKSVBaiduSync.nsp（备份程序）和 sysmodule/monitor/JKSVBaiduMonitor.nsp（监视程序）。两者都编译完成后，可以打出发布包：

```bash
make dist
```

发布包位于 dist/JKSV-BaiduNetdisk-<版本>.zip，解压到 SD 卡根目录即可。版本号来自 Makefile 中的 APP_VERSION，同时显示在 JKSV 界面上并写入后台日志。

后台百度客户端可以在电脑上对着本地模拟服务器测试，需要主机版的 json-c 静态库：

```bash
python3 tests/test_baidu_upload.py --json-source <json-c 头文件目录> --json-build <含 libjson-c.a 的目录>
```
