# Redis 会话缓存设计

## 目标

为现有 AR 场景会话 API 增加 Redis 会话缓存，减少按 token 查询 MySQL 的次数，同时保持 MySQL 为最终数据来源。Redis 不可用或缓存未命中时，服务必须回退到 MySQL。

## 范围

- 新增 `SessionCache`，复用现有 `RedisConnectionPool`。
- 缓存键使用 `session:{token}`。
- 缓存会话 ID、用户 ID、场景 ID、状态、创建时间、更新时间；不缓存密码及密码哈希。
- 会话 TTL 固定为 30 分钟（1800 秒）。每次成功查询会话或成功进入场景后续期。
- 登录/注册创建会话后写入缓存；退出场景成功后删除缓存。

不在本次改动中缓存用户资料，也不更改 MySQL 表结构。

## 数据格式与接口

`SessionCache` 提供以下接口：

- `get(token, session)`：读取并反序列化缓存会话；命中后把 TTL 刷新为 1800 秒。
- `put(session)`：序列化会话并使用 `SETEX` 写入，TTL 为 1800 秒。
- `refresh(token)`：使用 `EXPIRE` 将 TTL 重置为 1800 秒。
- `remove(token)`：使用 `DEL` 删除会话缓存。

缓存值使用与 `TwoLevelCache` 相同的简单字符串序列化思路，字段顺序为：

`id|userId|status|sceneId|createdAt|updatedAt`

`sceneId` 当前为受控短字符串，不包含分隔符；反序列化失败视为缓存未命中并回退 MySQL。

## 请求流程

### 认证 `/api/auth`

1. 先完成 MySQL 用户查询/注册和 `sessions` 记录创建。
2. 创建成功后构造状态为 `0` 的 `Session` 并写入 Redis。
3. Redis 写失败不回滚 MySQL，响应仍按 MySQL 结果返回。

### 查询 `/api/session`

1. 先读取 `session:{token}`。
2. 命中时返回缓存内容，并将 TTL 续期 30 分钟。
3. 未命中或 Redis 不可用时查询 MySQL。
4. MySQL 查到后回填 Redis，再返回结果。

### 进入场景 `/api/session/enter`

1. 先更新 MySQL 的 `scene_id` 和 `status=1`。
2. 更新成功后读取/构造最新会话并写入 Redis，TTL 重置为 30 分钟。
3. Redis 更新失败不回滚 MySQL。

### 退出场景 `/api/session/exit`

1. 先在 MySQL 中更新 `scene_id=''`、`status=0`。
2. 更新成功后删除 `session:{token}`。
3. Redis 删除失败不回滚 MySQL；后续查询可从 MySQL 获得状态。

## 一致性与降级

- 所有写操作先写 MySQL，成功后再更新或删除 Redis，避免缓存先于持久化层反映未成功的状态。
- Redis 连接池借用失败、Redis 命令失败、缓存数据格式异常均视为缓存未命中，回退 MySQL。
- MySQL 仍负责持久化、状态正确性及灾后缓存重建；Redis 仅为加速层。

## 验证

- 单元测试覆盖序列化、反序列化和 TTL 续期命令路径（使用可替代的 Redis 命令边界）。
- 构建启用 Redis/MySQL 的项目。
- 集成验证：认证后出现 `session:{token}`；查询后 TTL 被刷新；进入场景后缓存内容更新；退出后键被删除；停用 Redis 后查询仍能从 MySQL 返回。
