# KafkaLauncher 非法附魔 Hook 实现文档

目标版本：MCBE `arm64-v8a.so` 1.21.51.02（246MB，全符号 strip）
实现文件：`app/src/main/cpp/minecraft_world_hook.c`

---

## 一、设计思路

在**单机 / 本机 LAN host** 场景下（客户端即服务端），附魔校验全部在客户端进程内完成。
因此无需向服务端发送任何数据包，直接在客户端代码段上做**字节级补丁**即可让 `/enchant`
命令接受超等级、冲突组合等非法附魔，并在游戏内生效。

> 注意：LAN 客机场景（连接他人开房）中，`/enchant` 由 host 服务端执行并校验权限，
> 客户端补丁无法授予 operator，也不会有任何实际效果（详见 `mcbe-enchant-hook.md` 第八节）。
> 本实现仅覆盖单机 / 自己开房场景。

### 与旧实现的关系

旧实现通过 `screen_model_send_chat_hook` 把聊天内容 "sharpness N" 改写成
`/enchant @s sharpness N` 再经 `executeCommand` 发送。该链路依赖聊天 UI 钩子，
且只能发送合法命令（权限/等级门无法绕过）。**本版本已移除该链路**，改为直接在
代码段打补丁，玩家直接输入 `/enchant @s sharpness 1000` 即可生效。

---

## 二、三个字节级补丁点（均已字节级确认）

| # | 偏移 | 原指令 (bytes) | 替换 (bytes) | 作用 |
|---|---|---|---|---|
| 1 | `0x8F73200` | `TBZ W0,#0` (`C0 01 00 36`) | `NOP` (`1F 20 03 D5`) | 忽略 `ItemEnchantableComponent::addEnchantment` 内 `sub_9095AB0` 校验结果 |
| 2 | `0x5D253A4` | `TBZ W0,#0` (`60 3B 00 36`) | `NOP` (`1F 20 03 D5`) | 忽略 `EnchantCommand::execute` 内 `sub_98EF9F8` 等级范围门（`level > maxLevel`） |
| 3 | `0xB631E4C` | `CMP W1,#0; CSET W8,NE` (`3F 00 00 71 E8 07 9F 1A`) | `MOV X0,#1; RET` (`20 00 80 D2 C0 03 5F D6`) | `sub_B631E4C`（物品/附魔兼容位掩码校验，唯一调用者 = 附魔应用核心）恒返回 true |

### 补丁作用链

```
玩家输入  /enchant @s sharpness 1000
  → sub_5D251D0 (EnchantCommand::execute)
      ├─ sub_98EF9F8(level,1,maxLevel,out)   ── 等级门     → 补丁 #2 绕过
      ├─ sub_984CA38 解析目标 @s
      ├─ (vtable+648) 取手持 ItemStack
      ├─ sub_B635C28 → sub_B5A29A8
      │    └─ sub_B631E4C(兼容位掩码)          ── 兼容门     → 补丁 #3 绕过
      └─ (vtable+640) 写回手持物品
```

补丁 #1 覆盖脚本 API / 附魔台 roll 路径（`sub_8F73120`），与命令路径互补。

---

## 三、代码结构（minecraft_world_hook.c）

### 新增函数

```c
// 通用代码段补丁器：解析 so 基址 + RVA，校验原字节，mprotect 后写入并刷 cache
static int patch_code_bytes(const mapping *mappings, size_t mapping_count,
        uintptr_t load_bias, uintptr_t rva,
        const uint8_t *expected, const uint8_t *replacement,
        size_t size, const char *label);

// 三个补丁入口（每补丁一个函数，含期望字节签名）
static int patch_enchant_validator(...);   // 0x8F73200
static int patch_enchant_level_gate(...);  // 0x5D253A4
static int patch_enchant_compat(...);      // 0xB631E4C
```

### 安装流程

`kafka_install_world_entered_hook` 在既有聊天/生成钩子基础上新增：

```c
int enchant_validator_patched = patch_enchant_validator(...);
int enchant_level_gate_patched = patch_enchant_level_gate(...);
int enchant_compat_patched = patch_enchant_compat(...);
```

任一补丁成功即视为整体安装成功（进入 state=2）。每个补丁都做了：
- **字节签名校验**：原字节不符直接失败并 `LOGE`，不破坏不匹配版本。
- **幂等**：已是目标字节则跳过并返回成功。
- **可回滚**：保存 `expected`，mprotect 还原即可。

### 已移除内容

- `parse_sharpness_chat_command`（"sharpness N" → `/enchant @s sharpness N` 翻译器）
- `send_with_screen_model`（经 screen-model 执行命令的发送器）
- `make_short_std_string` / `make_heap_std_string` / `make_std_string_any_length` / `heap_string_storage`（仅被上述发送器使用的字符串构造辅助）
- `screen_model_send_chat_hook` 中的 `enchant_action` / `#` / `@` 动作分支（现仅透传+日志）

---

## 四、使用方式

1. 用本 launcher 启动 MCBE，进入**单机世界**（或本机开 LAN 房）。
2. 游戏内打开聊天框，直接输入：
   ```
   /enchant @s sharpness 1000
   ```
   或任意非法组合，如互斥附魔、超上限等级。
3. 手持物品即被附魔，等级/冲突限制均被绕过。

---

## 五、验证与回滚

- 每个补丁打印 `[enchant-validator|enchant-level-gate|enchant-compat] patched +0x...`
  日志，可用于确认打点。
- 字节不匹配时打印 `bytes mismatch`，说明版本已变，需重新确认偏移。
- 恢复：将目标地址写回 `expected` 数组即可（4B 对齐，单/双指令，无跳转依赖）。

---

## 六、局限性

- **仅单机 / 本机 LAN host 生效**；LAN 客机场景需另行方案（见 `mcbe-enchant-hook.md`）。
- 补丁地址基于 1.21.51.02；版本更新需重新用 IDA 确认偏移。
- 若游戏校验代码段 hash（反作弊），需要额外处理。
