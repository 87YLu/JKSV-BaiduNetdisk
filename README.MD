# JKSV 百度网盘存档同步

> v1.0.1 · 适用于运行 Atmosphère 的 Nintendo Switch

版本变化请参阅 [CHANGELOG.MD](CHANGELOG.MD)。

这是一个加入百度网盘支持的 JKSV 版本。完成一次配置后，它可以在游戏结束时自动备份对应存档并上传到百度网盘，也可以在 JKSV 中查看、上传和恢复云端备份。

## 主要功能

- 使用百度网盘 App 扫描二维码登录，不需要在 Switch 上输入百度账号密码。
- 自动保存并续期登录状态，通常不需要反复扫码。
- 游戏关闭后自动备份对应存档并上传百度网盘。
- 使用 Ultrahand 时，后台上传成功后显示系统级 Toast 提示。
- 存档没有变化时自动跳过，避免重复上传。
- 断网或上传失败时保留任务，恢复联网后自动重试。
- 支持账号存档和可选的设备共享存档。
- 在 JKSV 中查看当前百度网盘账号，并可随时退出或重新登录。
- 在 JKSV 中上传本地 ZIP、恢复云端备份、覆盖或删除云端文件。
- 自动备份采用 JKSV 兼容格式，可以直接恢复。

## 开始前需要准备

- 一台可以运行 Atmosphère 和 Homebrew Menu 的 Nintendo Switch。
- 可正常读写的 SD 卡。
- 百度网盘 App，用于扫描授权二维码。
- 能够访问百度网盘开放平台的百度账号。
- 在百度网盘开放平台创建应用后取得的 AppKey 和 SecretKey。
- Switch 可正常连接互联网。

百度网盘开放平台入口：[pan.baidu.com/union](https://pan.baidu.com/union/)

本项目不会提供公共 AppKey 或 SecretKey。请使用自己的应用信息，并且不要把包含密钥或登录令牌的配置文件发送给他人。

## 安装

将发布包解压到 SD 卡根目录。安装完成后，主要文件应位于：

    /switch/JKSV/JKSV.nro
    /atmosphere/contents/420000004A4B5344/exefs.nsp
    /atmosphere/contents/420000004A4B5344/flags/boot2.flag
    /config/JKSV/baidu.example.json

复制完成后，请将 Switch 完整关机并重新开机。只返回主界面或让主机休眠不会重新加载后台模块。

## 配置百度网盘

### 1. 创建开放平台应用

1. 登录百度网盘开放平台。
2. 创建一个拥有网盘文件访问权限的应用。
3. 建议将应用名称设为 JKSV。
4. 记录应用的 AppKey 和 SecretKey。

百度开放平台应用通常只能访问自己的应用数据目录。如果应用名称不是 JKSV，请把下面配置中的 basepath 改成该应用对应的目录。

### 2. 创建配置文件

复制下面的示例，在 SD 卡创建 **/config/JKSV/baidu.json**：

    {
        "app_key": "你的 AppKey",
        "secret_key": "你的 SecretKey",
        "basepath": "/apps/JKSV",
        "background_sync": true
    }

保存时请确认：

- 文件名是 baidu.json，而不是 baidu.json.txt。
- app_key 和 secret_key 没有多余空格。
- basepath 以 / 开头，并与开放平台应用目录一致。
- JSON 中最后一项后面不要添加逗号。

如果已经存在 Google Drive 的 client_secret.json 或 WebDAV 的 webdav.json，JKSV 可能会优先使用其他远端服务。需要使用百度网盘时，建议暂时移走其他远端配置文件。

## 首次扫码登录

1. 保持 Switch 联网。
2. 建议按住 **R** 启动任意游戏进入完整内存模式的 Homebrew Menu。
3. 启动 JKSV。
4. 屏幕出现二维码后，打开百度网盘 App 扫描并确认授权。
5. 等待 JKSV 显示“成功登录百度网盘”。

如果之前已经退出账号，可进入 JKSV 的设置页面，在“百度网盘账号”项目上按 **[A]** 重新扫码登录。

授权成功后，登录信息会写入 **/config/JKSV/baidu.json**。登录状态到期时会自动续期。

## 日常自动备份

完成安装和登录后，不需要每次手动打开 JKSV。

1. 正常游玩并保存游戏。
2. 回到 Switch 主界面。
3. 按 **[X]** 彻底关闭游戏。
4. 后台模块会等待存档写入完成，然后创建备份并上传。

仅按 **Home** 键返回主界面时，游戏仍在运行，不会开始自动备份。

云端备份默认保存在：

    /apps/JKSV/<游戏名>/AUTO - <用户 [账号标识]> - <时间>.zip

上传成功后会记录本次存档状态。下次退出游戏时，如果存档没有变化，就不会再次上传。

如果 nx-ovlloader 已加载支持 API Notifications 的 Ultrahand（建议 2.3.0 或更新版本），后台模块会在每批上传完成后显示一次 Toast。仅安装 nx-ovlloader 不够，还必须在 **/config/ultrahand/config.ini** 的 `[ultrahand]` 段设置 `notifications=true`，并重新加载 Ultrahand 或完整重启 Switch。通知不可用时只会跳过提示，不会影响备份和上传。

如果网络暂时不可用，待上传文件会留在队列中，并在联网且没有游戏运行时自动重试。

## 手动创建和上传备份

### 创建新的云端备份

1. 打开 JKSV。
2. 选择用户和游戏。
3. 选择“新备份”。
4. 如果设置中启用了“自动上传备份到远程”，新备份会直接上传百度网盘。
5. 如果启用了“保留本地备份”，上传后仍会在 SD 卡保留一份。

### 上传已有本地备份

1. 在游戏的备份列表中选中一个本地 ZIP。
2. 按 **[ZR]** 上传。
3. 等待上传完成。

远端上传只支持 ZIP。需要手动上传时，请先在设置中启用“导出存档为 ZIP”。

## 从百度网盘恢复存档

1. 完全关闭需要恢复的游戏。
2. 打开 JKSV，选择对应用户和游戏。
3. 在备份列表中找到带 **[BD]** 前缀的云端备份。
4. 选中备份并按 **[Y]**。
5. 阅读确认提示后执行恢复。

恢复会覆盖主机上的当前存档。建议先创建一个新的本地备份，并在设置中启用“恢复时自动备份”。

后台功能只负责备份和上传，不会自动把云端存档写回 Switch；恢复操作始终需要在 JKSV 中手动确认。

## 查看账号和退出登录

进入 JKSV 设置页面，可以看到“百度网盘账号”：

- 已登录时显示当前账号名。
- 暂时无法取得昵称时，会显示已登录状态。
- 已退出时，在该项目按 **[A]** 可以重新登录。
- 选择“退出百度网盘”会清除本机保存的登录令牌并暂停后台上传。

退出登录不会删除：

- 百度网盘中已经上传的备份。
- AppKey 和 SecretKey。
- 本机尚未上传成功的队列文件。
- 其他 JKSV 设置。

## 后台选项

可以在 **/config/JKSV/baidu.json** 中加入以下选项：

    {
        "background_sync": true,
        "background_include_device_saves": true,
        "background_keep_local": false,
        "background_verify_tls": true,
        "background_compression": 1,
        "background_close_delay_seconds": 3,
        "background_retry_minutes": 5
    }

| 选项 | 默认值 | 用途 |
| --- | ---: | --- |
| background_sync | true | 开启或暂停游戏退出后的自动备份 |
| background_include_device_saves | true | 同时备份设备或共享存档 |
| background_keep_local | false | 上传成功后在 SD 卡保留 ZIP |
| background_verify_tls | true | 校验 HTTPS 证书，建议保持开启 |
| background_compression | 1 | ZIP 压缩等级，范围 0–9 |
| background_close_delay_seconds | 3 | 关闭游戏后等待存档写入的秒数，范围 1–30 |
| background_retry_minutes | 5 | 上传失败后的重试间隔，范围 1–180 分钟 |

修改配置后，建议完整重启 Switch。

## 文件位置

| 内容 | 路径 |
| --- | --- |
| 百度网盘配置 | /config/JKSV/baidu.json |
| 后台运行日志 | /config/JKSV/background-sync.log |
| 待上传队列 | /config/JKSV/background/queue/ |
| 可选的本地自动备份 | /JKSV/Background/<游戏名>/ |
| JKSV 前台日志 | /JKSV/log.txt |
| 百度网盘云端目录 | /apps/JKSV/ |

配置中的 background_keep_local 为 false 时，上传成功后会删除队列中的临时 ZIP；这不会删除云端文件。

## 暂停或卸载

- 临时暂停：把 baidu.json 中的 background_sync 改为 false。
- 停止开机加载：删除或改名 **/atmosphere/contents/420000004A4B5344/flags/boot2.flag**，然后重启。
- 完全卸载后台模块：删除 **/atmosphere/contents/420000004A4B5344/**。
- 只使用手动备份：可以保留 **/switch/JKSV/JKSV.nro** 和百度网盘配置。

## 常见问题

### 关闭游戏后没有上传

依次确认：

1. 游戏是否通过 **[X]** 完全关闭，而不是只按 Home 返回。
2. Switch 是否在安装后完整重启过。
3. exefs.nsp 和 boot2.flag 是否位于正确目录。
4. baidu.json 中 background_sync 是否为 true。
5. JKSV 设置页是否显示已经登录百度网盘。
6. 后台日志中是否出现 GAME_CLOSE、UPLOAD_OK 或 UPLOAD_FAILED。

### 没有出现二维码

- 确认 baidu.json 存在并包含正确的 AppKey 和 SecretKey。
- 确认 Switch 可以联网。
- 暂时移走 Google Drive 或 WebDAV 配置。
- 如果设置页显示“已退出”，在“百度网盘账号”项目按 **[A]**。
- 如果授权码过期，退出登录后重新扫码。

### 上传失败

- 检查网络连接和百度网盘剩余空间。
- 确认 basepath 与开放平台应用目录一致。
- 在设置中退出百度网盘后重新扫码登录。
- 查看 **/config/JKSV/background-sync.log** 中最近一次 UPLOAD_FAILED 后面的错误信息。

### 上传成功但没有 Toast

- 确认 nx-ovlloader 当前加载的是 Ultrahand，而不是 Tesla Menu；仅有 nx-ovlloader 无法显示通知。
- 打开 **/config/ultrahand/config.ini**，确认 `[ultrahand]` 段中存在 `notifications=true`。
- 如果 `notifications_hotkey=true`，单独长按 **[-]** 约 3 秒会切换通知；请确认没有因此被改回 `notifications=false`。
- 修改配置后重新加载 Ultrahand，或将 Switch 完整重启。
- 确认本次日志包含 `UPLOAD_OK`；`SAVE_UNCHANGED` 和 `UPLOAD_FAILED` 不会触发通知。
- 建议使用 Ultrahand 2.3.0 或更新版本。
- 查看后台日志中是否出现 `NOTIFY_QUEUED`、`NOTIFY_SKIPPED` 或 `NOTIFY_FAILED`。

### 自动生成了多份备份

每个账号存档和设备存档会分别生成文件。文件名中的用户和账号标识用于区分不同存档。

同一个存档只有发生变化后才会再次上传；如果没有变化，日志会记录 SAVE_UNCHANGED。

### 开机后后台模块被停用

如果连续两次在备份或上传过程中异常中止，模块会把 boot2.flag 改名为 boot2.flag.crashed，以避免反复启动失败。

请先检查后台日志和配置。确认问题解决后，将文件重新命名为 boot2.flag 并完整重启 Switch。

## 使用安全

- 恢复存档前先备份当前进度。
- 不要在游戏运行时恢复对应存档。
- 不要公开 baidu.json，其中可能包含应用密钥和长期登录令牌。
- 不要随意删除 background/queue 中的文件，未上传成功的备份会保存在这里。
- 重要存档建议同时保留 SD 卡副本和其他离线副本。

## 项目说明

本项目基于 [J-D-K/JKSV](https://github.com/J-D-K/JKSV) 开发，并为百度网盘登录、管理和游戏退出后的自动存档同步进行了扩展。
