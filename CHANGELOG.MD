# 变更日志

## v1.0.2

### 优化

- 优化后台空闲与待机策略，减少无任务时不必要的唤醒和 SD 卡访问；游戏退出后的自动备份仍会及时启动。
- 连续上传失败时逐步延长重试间隔，默认按 5、10、20、40、60 分钟退避；出现新存档或上传恢复正常后自动回到基础间隔。

## v1.0.1

### 新增

- 新增后台上传成功 Toast 通知。自动备份或失败任务重试成功后，会提示本批次成功上传的存档数量；通知不可用不会影响备份和上传。

### Toast 开启条件

> 以下条件必须全部满足。仅安装 nx-ovlloader 不会显示通知。

1. nx-ovlloader 已正常运行，并加载的是 **Ultrahand Overlay**，而不是 Tesla Menu。
2. Ultrahand 支持 API Notifications，建议使用 **2.3.0 或更新版本**。
3. SD 卡上的 `/config/ultrahand/config.ini` 已存在，并在 `[ultrahand]` 段中设置：

       notifications=true

4. 修改配置后已重新加载 Ultrahand，或将 Switch 完整重启。
5. 本次确实有存档上传成功。日志为 `SAVE_UNCHANGED` 或 `UPLOAD_FAILED` 时不会显示 Toast，旧的成功记录也不会补发通知。

成功上传后，后台日志应依次出现 `UPLOAD_OK` 和 `NOTIFY_QUEUED`。如果只有 `NOTIFY_QUEUED` 而没有画面提示，请首先确认 `notifications=true` 没有被 Ultrahand 的通知快捷键再次关闭。`notifications_hotkey=true` 时，单独长按 **[-]** 约 3 秒会切换并保存这个开关；如需避免误关，可将其设为 `false`。

## v1.0.0

- 首个版本，加入百度网盘登录、手动云端管理及游戏退出后的后台自动备份与上传。
